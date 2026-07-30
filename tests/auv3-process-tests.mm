#include "../src/detail/auv3/process.h"
#include <clapwrapper/event-registry.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

namespace
{
struct TestState
{
  std::vector<float *> expectedOutputPointers;
  bool outputPointersMatched = false;
  bool inputWasInPlace = false;
  bool pushOutputEvents = false;
  bool outputPushResults[3] = {};
  uint32_t inputEventCount = 0;
  bool inputEventsSorted = true;
  uint32_t processCalls = 0;
  float observedInputSamples[8] = {};
  uint32_t firstEventTimes[8] = {};
  struct EventSnapshot
  {
    clap_event_header_t header {};
    uint16_t midi2Port = 0;
    uint32_t midi2Data[4] = {};
    uint32_t rampDuration = 0;
    clap_id rampParameterId = CLAP_INVALID_ID;
    uint64_t rampParameterAddress = 0;
    void *rampCookie = nullptr;
    int32_t noteId = -1;
    int16_t noteKey = -1;
    int16_t noteChannel = -1;
  } inputEvents[32];
  uint32_t capturedEventCount = 0;
  const clap_wrapper_plugin_auv3_param_ramp_t *rampExtension = nullptr;
};

struct TestRampEvent
{
  clap_event_header_t header;
  clap_id parameterId;
  uint64_t parameterAddress;
  void *cookie;
  double targetValue;
  uint32_t durationFrames;
  uint64_t reserved;
};

static_assert(sizeof(TestRampEvent) == 64, "test ramp must exercise the full wrapper storage");

bool CLAP_ABI translateRamp(const clap_wrapper_auv3_param_ramp_info_t *info, void *storage,
                            uint32_t storageCapacity, uint32_t *eventSize)
{
  if (info == nullptr || storage == nullptr || eventSize == nullptr || storageCapacity < sizeof(TestRampEvent))
    return false;
  auto *event = static_cast<TestRampEvent *>(storage);
  *event = {{sizeof(TestRampEvent), info->sample_offset, 21, 3, 0}, info->parameter_id,
            info->parameter_address, info->cookie, info->target_value, info->duration_sample_frames, 0};
  *eventSize = sizeof(*event);
  return true;
}

bool CLAP_ABI translateMalformedRamp(const clap_wrapper_auv3_param_ramp_info_t *, void *storage,
                                     uint32_t storageCapacity, uint32_t *eventSize)
{
  if (storage == nullptr || eventSize == nullptr || storageCapacity < sizeof(clap_event_header_t))
    return false;
  auto *header = static_cast<clap_event_header_t *>(storage);
  *header = {storageCapacity + 1, 0, 21, 3, 0};
  *eventSize = storageCapacity;
  return true;
}

const void *getPluginExtension(const clap_plugin_t *plugin, const char *id)
{
  auto &state = *static_cast<TestState *>(plugin->plugin_data);
  if (state.rampExtension != nullptr && std::strcmp(id, CLAP_WRAPPER_EXT_AUV3_PARAM_RAMP) == 0)
    return state.rampExtension;
  return nullptr;
}

clap_process_status processPlugin(const clap_plugin_t *plugin, const clap_process_t *process)
{
  auto &state = *static_cast<TestState *>(plugin->plugin_data);
  ++state.processCalls;

  state.outputPointersMatched =
      state.expectedOutputPointers.size() == process->audio_outputs[0].channel_count;
  for (uint32_t ch = 0; state.outputPointersMatched && ch < process->audio_outputs[0].channel_count;
       ++ch)
    state.outputPointersMatched =
        process->audio_outputs[0].data32[ch] == state.expectedOutputPointers[ch];

  if (process->audio_inputs_count > 0 && process->audio_outputs_count > 0)
  {
    state.inputWasInPlace = true;
    const auto channels =
        std::min(process->audio_inputs[0].channel_count, process->audio_outputs[0].channel_count);
    for (uint32_t ch = 0; ch < channels; ++ch)
      state.inputWasInPlace &=
          process->audio_inputs[0].data32[ch] == process->audio_outputs[0].data32[ch];
  }

  state.inputEventCount = process->in_events->size(process->in_events);
  const auto processIndex = state.processCalls - 1;
  if (processIndex < std::size(state.observedInputSamples) && process->audio_inputs_count > 0 &&
      process->audio_inputs[0].channel_count > 0)
    state.observedInputSamples[processIndex] = process->audio_inputs[0].data32[0][0];
  if (processIndex < std::size(state.firstEventTimes))
    state.firstEventTimes[processIndex] = state.inputEventCount == 0 ? UINT32_MAX :
        process->in_events->get(process->in_events, 0)->time;
  uint32_t previousTime = 0;
  for (uint32_t i = 0; i < state.inputEventCount; ++i)
  {
    auto *event = process->in_events->get(process->in_events, i);
    state.inputEventsSorted &= event != nullptr && (i == 0 || event->time >= previousTime);
    if (event) previousTime = event->time;
    if (event != nullptr && state.capturedEventCount < std::size(state.inputEvents))
    {
      auto &snapshot = state.inputEvents[state.capturedEventCount++];
      snapshot.header = *event;
      if (event->type == CLAP_EVENT_MIDI2)
      {
        const auto *midi2 = reinterpret_cast<const clap_event_midi2_t *>(event);
        snapshot.midi2Port = midi2->port_index;
        std::memcpy(snapshot.midi2Data, midi2->data, sizeof(snapshot.midi2Data));
      }
      else if (event->type == CLAP_EVENT_NOTE_ON || event->type == CLAP_EVENT_NOTE_OFF)
      {
        const auto *note = reinterpret_cast<const clap_event_note_t *>(event);
        snapshot.noteId = note->note_id;
        snapshot.noteKey = note->key;
        snapshot.noteChannel = note->channel;
      }
      else if (event->space_id == 21 && event->type == 3 && event->size == sizeof(TestRampEvent))
      {
        const auto *ramp = reinterpret_cast<const TestRampEvent *>(event);
        snapshot.rampDuration = ramp->durationFrames;
        snapshot.rampParameterId = ramp->parameterId;
        snapshot.rampParameterAddress = ramp->parameterAddress;
        snapshot.rampCookie = ramp->cookie;
      }
    }
  }

  if (state.pushOutputEvents)
  {
    clap_event_midi_t event{};
    event.header.size = sizeof(event);
    event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    event.header.type = CLAP_EVENT_MIDI;
    for (size_t i = 0; i < 3; ++i)
      state.outputPushResults[i] = process->out_events->try_push(process->out_events, &event.header);
  }

  for (uint32_t bus = 0; bus < process->audio_outputs_count; ++bus)
    for (uint32_t ch = 0; ch < process->audio_outputs[bus].channel_count; ++ch)
      for (uint32_t frame = 0; frame < process->frames_count; ++frame)
        process->audio_outputs[bus].data32[ch][frame] =
            100.0f * (float)(bus + 1) + 10.0f * (float)ch + (float)frame;

  return CLAP_PROCESS_CONTINUE;
}

struct AudioBufferListDeleter
{
  void operator()(AudioBufferList *list) const { std::free(list); }
};

using AudioBufferListPtr = std::unique_ptr<AudioBufferList, AudioBufferListDeleter>;

AudioBufferListPtr makeBufferList(uint32_t buffers)
{
  const auto bytes =
      sizeof(AudioBufferList) + (buffers > 1 ? (buffers - 1) * sizeof(AudioBuffer) : 0);
  auto result = AudioBufferListPtr((AudioBufferList *)std::calloc(1, bytes));
  result->mNumberBuffers = buffers;
  return result;
}

AudioTimeStamp timestamp(double sampleTime, uint64_t hostTime)
{
  AudioTimeStamp result{};
  result.mSampleTime = sampleTime;
  result.mHostTime = hostTime;
  result.mFlags = kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid;
  return result;
}

std::vector<AURenderEvent> makeNoteOns(size_t count)
{
  std::vector<AURenderEvent> events(count);
  for (size_t i = 0; i < count; ++i)
  {
    auto &event = events[i].MIDI;
    event.next = i + 1 < count ? &events[i + 1] : nullptr;
    event.eventSampleTime = (AUEventSampleTime)(count - i - 1);
    event.eventType = AURenderEventMIDI;
    event.length = 3;
    event.data[0] = 0x90;
    event.data[1] = (uint8_t)(60 + i);
    event.data[2] = 100;
  }
  return events;
}

AURenderEvent makeParameterRamp(AUEventSampleTime sampleTime, AUParameterAddress address,
                                AUValue value, AUAudioFrameCount duration)
{
  AURenderEvent event {};
  event.parameter.eventSampleTime = sampleTime;
  event.parameter.eventType = AURenderEventParameterRamp;
  event.parameter.parameterAddress = address;
  event.parameter.value = value;
  event.parameter.rampDurationSampleFrames = duration;
  return event;
}

std::vector<uint8_t> makeMIDI2Event(AUEventSampleTime sampleTime, uint8_t cable,
                                    const std::vector<std::vector<uint32_t>> &packets)
{
  size_t bytes = offsetof(AUMIDIEventList, eventList.packet);
  for (const auto &words : packets)
    bytes += offsetof(MIDIEventPacket, words) + words.size() * sizeof(uint32_t);

  std::vector<uint8_t> storage(bytes, 0);
  auto *event = reinterpret_cast<AURenderEvent *>(storage.data());
  event->MIDIEventsList.eventSampleTime = sampleTime;
  event->MIDIEventsList.eventType = AURenderEventMIDIEventList;
  event->MIDIEventsList.cable = cable;
  event->MIDIEventsList.eventList.numPackets = static_cast<uint32_t>(packets.size());
  auto *packet = &event->MIDIEventsList.eventList.packet[0];
  for (const auto &words : packets)
  {
    packet->wordCount = static_cast<uint32_t>(words.size());
    std::memcpy(packet->words, words.data(), words.size() * sizeof(uint32_t));
    packet = MIDIEventPacketNext(packet);
  }
  return storage;
}

clap_plugin_t makePlugin(TestState &state)
{
  clap_plugin_t plugin{};
  plugin.plugin_data = &state;
  plugin.process = processPlugin;
  plugin.get_extension = getPluginExtension;
  return plugin;
}

bool expect(bool condition, const char *message)
{
  if (!condition) std::cerr << "FAILED: " << message << '\n';
  return condition;
}

bool testDirectAndInPlace()
{
  constexpr uint32_t frames = 8;
  uint32_t channels[] = {2};
  TestState state;
  auto plugin = makePlugin(state);
  Clap::AUv3::ProcessAdapter adapter;
  adapter.setupProcessing(1, channels, 1, channels, &plugin, nullptr, nullptr, frames,
                          CLAP_NOTE_DIALECT_CLAP);

  float left[frames] = {};
  float right[frames] = {};
  auto output = makeBufferList(2);
  output->mBuffers[0] = {1, sizeof(left), left};
  output->mBuffers[1] = {1, sizeof(right), right};
  state.expectedOutputPointers = {left, right};
  auto *leftPointer = left;
  auto *rightPointer = right;

  AURenderPullInputBlock pull = ^AUAudioUnitStatus(
      AudioUnitRenderActionFlags *, const AudioTimeStamp *, AUAudioFrameCount, NSInteger,
      AudioBufferList *input) {
    input->mNumberBuffers = 2;
    input->mBuffers[0] = {1, frames * sizeof(float), leftPointer};
    input->mBuffers[1] = {1, frames * sizeof(float), rightPointer};
    return noErr;
  };

  auto time = timestamp(0, 1);
  AudioUnitRenderActionFlags flags = 0;
  const auto copiesBefore = adapter.outputCopyCount();
  auto status = adapter.process(&flags, &time, frames, 0, output.get(), nullptr, pull);
  const auto copiesAfter = adapter.outputCopyCount();

  bool ok = expect(status == noErr, "direct render status") &&
            expect(state.outputPointersMatched, "CLAP output pointers are host pointers") &&
            expect(state.inputWasInPlace, "effect input remains in-place") &&
            expect(copiesAfter == copiesBefore, "direct render performs no post-process copy") &&
            expect(left[3] == 103.0f && right[3] == 113.0f, "direct output samples");

  float repeatedLeft[frames] = {};
  float repeatedRight[frames] = {};
  auto repeated = makeBufferList(2);
  repeated->mBuffers[0] = {1, sizeof(repeatedLeft), repeatedLeft};
  repeated->mBuffers[1] = {1, sizeof(repeatedRight), repeatedRight};
  state.expectedOutputPointers = {repeatedLeft, repeatedRight};
  auto repeatedStatus = adapter.process(&flags, &time, frames, 0, repeated.get(), nullptr, nil);
  return ok && expect(repeatedStatus == noErr && state.processCalls == 2,
                      "single-bus repeated pull processes fresh buffers") &&
         expect(repeatedLeft[3] == 103.0f && repeatedRight[3] == 113.0f,
                "repeated pull copies the direct result");
}

bool testNullBufferFallback()
{
  constexpr uint32_t frames = 8;
  uint32_t channels[] = {2};
  TestState state;
  auto plugin = makePlugin(state);
  Clap::AUv3::ProcessAdapter adapter;
  adapter.setupProcessing(0, nullptr, 1, channels, &plugin, nullptr, nullptr, frames,
                          CLAP_NOTE_DIALECT_CLAP);

  auto output = makeBufferList(2);
  output->mBuffers[0] = {1, frames * sizeof(float), nullptr};
  output->mBuffers[1] = {1, frames * sizeof(float), nullptr};
  auto time = timestamp(10, 2);
  AudioUnitRenderActionFlags flags = 0;
  auto status = adapter.process(&flags, &time, frames, 0, output.get(), nullptr, nil);

  auto *left = (float *)output->mBuffers[0].mData;
  auto *right = (float *)output->mBuffers[1].mData;
  return expect(status == noErr, "null-buffer render status") &&
         expect(left != nullptr && right != nullptr, "fallback supplies null host buffers") &&
         expect(left[4] == 104.0f && right[4] == 114.0f, "fallback output samples");
}

bool testSingleBusSameTimestampIsFresh()
{
  constexpr uint32_t frames = 8;
  uint32_t channels[] = {1};
  TestState state;
  auto plugin = makePlugin(state);
  Clap::AUv3::ProcessAdapter adapter;
  adapter.setupProcessing(1, channels, 1, channels, &plugin, nullptr, nullptr, frames,
                          CLAP_NOTE_DIALECT_CLAP);

  float firstInput[frames] = {1.0f};
  float secondInput[frames] = {2.0f};
  float firstOutput[frames] = {};
  float secondOutput[frames] = {};
  auto *firstInputPointer = firstInput;
  auto *secondInputPointer = secondInput;
  __block uint32_t pullCount = 0;
  AURenderPullInputBlock pull = ^AUAudioUnitStatus(
      AudioUnitRenderActionFlags *, const AudioTimeStamp *, AUAudioFrameCount, NSInteger,
      AudioBufferList *input) {
    input->mNumberBuffers = 1;
    input->mBuffers[0] = {1, frames * sizeof(float),
                          pullCount++ == 0 ? firstInputPointer : secondInputPointer};
    return noErr;
  };
  auto firstEvent = makeNoteOns(1);
  auto secondEvent = makeNoteOns(1);
  firstEvent[0].MIDI.eventSampleTime = 50;
  secondEvent[0].MIDI.eventSampleTime = 52;
  auto first = makeBufferList(1);
  auto second = makeBufferList(1);
  first->mBuffers[0] = {1, sizeof(firstOutput), firstOutput};
  second->mBuffers[0] = {1, sizeof(secondOutput), secondOutput};
  AudioUnitRenderActionFlags flags = 0;
  auto time = timestamp(50, 22);
  state.expectedOutputPointers = {firstOutput};
  const auto firstStatus = adapter.process(&flags, &time, frames, 0, first.get(), &firstEvent.front(), pull);
  state.expectedOutputPointers = {secondOutput};
  const auto secondStatus = adapter.process(&flags, &time, frames, 0, second.get(), &secondEvent.front(), pull);

  return expect(firstStatus == noErr && secondStatus == noErr && state.processCalls == 2,
                "same-timestamp single bus processes twice") &&
         expect(state.observedInputSamples[0] == 1.0f && state.observedInputSamples[1] == 2.0f,
                "same-timestamp single bus pulls fresh input") &&
         expect(state.firstEventTimes[0] == 0 && state.firstEventTimes[1] == 2,
                "same-timestamp single bus receives fresh events") &&
         expect(firstOutput[3] == 103.0f && secondOutput[3] == 103.0f,
                "same-timestamp single bus writes each output buffer");
}

bool testMultiBusCaching()
{
  constexpr uint32_t frames = 8;
  uint32_t channels[] = {1, 2};
  TestState state;
  auto plugin = makePlugin(state);
  Clap::AUv3::ProcessAdapter adapter;
  adapter.setupProcessing(0, nullptr, 2, channels, &plugin, nullptr, nullptr, frames,
                          CLAP_NOTE_DIALECT_CLAP);

  float bus1Left[frames] = {};
  float bus1Right[frames] = {};
  auto bus1 = makeBufferList(2);
  bus1->mBuffers[0] = {1, sizeof(bus1Left), bus1Left};
  bus1->mBuffers[1] = {1, sizeof(bus1Right), bus1Right};

  auto time = timestamp(20, 3);
  AudioUnitRenderActionFlags flags = 0;
  auto status1 = adapter.process(&flags, &time, frames, 1, bus1.get(), nullptr, nil);

  float bus0Data[frames] = {};
  auto bus0 = makeBufferList(1);
  bus0->mBuffers[0] = {1, sizeof(bus0Data), bus0Data};
  auto status0 = adapter.process(&flags, &time, frames, 0, bus0.get(), nullptr, nil);

  return expect(status1 == noErr && status0 == noErr, "multi-bus render status") &&
         expect(state.processCalls == 1, "multi-bus cycle processes CLAP once") &&
         expect(!state.outputPointersMatched, "multi-bus render uses staging") &&
         expect(bus1Left[2] == 202.0f && bus1Right[2] == 212.0f && bus0Data[2] == 102.0f,
                "multi-bus cached output samples");
}

bool testInterleavedFallback()
{
  constexpr uint32_t frames = 8;
  uint32_t channels[] = {2};
  TestState state;
  auto plugin = makePlugin(state);
  Clap::AUv3::ProcessAdapter adapter;
  adapter.setupProcessing(0, nullptr, 1, channels, &plugin, nullptr, nullptr, frames,
                          CLAP_NOTE_DIALECT_CLAP);

  float samples[frames * 2] = {};
  auto output = makeBufferList(1);
  output->mBuffers[0] = {2, sizeof(samples), samples};
  auto time = timestamp(25, 31);
  AudioUnitRenderActionFlags flags = 0;
  auto status = adapter.process(&flags, &time, frames, 0, output.get(), nullptr, nil);

  return expect(status == noErr, "interleaved render status") &&
         expect(!state.outputPointersMatched, "interleaved render uses staging") &&
         expect(samples[6] == 103.0f && samples[7] == 113.0f,
                "interleaved fallback output samples");
}

bool testCapacityBoundaries()
{
  constexpr uint32_t frames = 8;
  uint32_t channels[] = {1};
  TestState state;
  state.pushOutputEvents = true;
  auto plugin = makePlugin(state);
  Clap::AUv3::ProcessAdapter adapter;
  Clap::AUv3::ProcessAdapter::Capacities capacities;
  capacities.inputEvents = 2;
  capacities.eventIndices = 2;
  capacities.outputEvents = 2;
  capacities.activeNotes = 1;
  capacities.reorderScratch = 1;
  adapter.setupProcessing(0, nullptr, 1, channels, &plugin, nullptr, nullptr, frames,
                          CLAP_NOTE_DIALECT_CLAP, capacities);

  auto originalCapacities = adapter.vectorCapacities();
  auto events = makeNoteOns(3);
  float outputData[frames] = {};
  auto output = makeBufferList(1);
  output->mBuffers[0] = {1, sizeof(outputData), outputData};
  auto time = timestamp(30, 4);
  AudioUnitRenderActionFlags flags = 0;
  adapter.process(&flags, &time, frames, 0, output.get(), &events.front(), nil);
  auto overflows = adapter.overflowCounts();
  time = timestamp(31, 41);
  adapter.process(&flags, &time, frames, 0, output.get(), &events.front(), nil);
  auto finalCapacities = adapter.vectorCapacities();

  return expect(state.inputEventCount == 0 && state.inputEventsSorted,
                "bounded sorted input drops unadmitted notes") &&
         expect(overflows.inputEvents == 1, "input event overflow count") &&
         expect(overflows.activeNotes == 1, "active-note overflow count") &&
         expect(overflows.reorderScratch == 1, "reorder scratch overflow count") &&
         expect(state.outputPushResults[0] && state.outputPushResults[1] &&
                    !state.outputPushResults[2],
                "output try_push fails at capacity") &&
         expect(overflows.outputEvents == 1, "output event overflow count") &&
         expect(originalCapacities.inputEvents == finalCapacities.inputEvents &&
                    originalCapacities.eventIndices == finalCapacities.eventIndices &&
                    originalCapacities.outputEvents == finalCapacities.outputEvents &&
                    originalCapacities.activeNotes == finalCapacities.activeNotes &&
                    originalCapacities.reorderScratch == finalCapacities.reorderScratch,
                "vector capacities remain fixed across render");
}

bool testEventIndexOverflow()
{
  constexpr uint32_t frames = 8;
  uint32_t channels[] = {1};
  TestState state;
  auto plugin = makePlugin(state);
  Clap::AUv3::ProcessAdapter adapter;
  Clap::AUv3::ProcessAdapter::Capacities capacities;
  capacities.inputEvents = 3;
  capacities.eventIndices = 2;
  adapter.setupProcessing(0, nullptr, 1, channels, &plugin, nullptr, nullptr, frames,
                          CLAP_NOTE_DIALECT_CLAP, capacities);

  auto events = makeNoteOns(3);
  auto output = makeBufferList(1);
  float samples[frames] = {};
  output->mBuffers[0] = {1, sizeof(samples), samples};
  auto time = timestamp(40, 5);
  AudioUnitRenderActionFlags flags = 0;
  adapter.process(&flags, &time, frames, 0, output.get(), &events.front(), nil);

  return expect(state.inputEventCount == 2, "event-index boundary limits input") &&
         expect(adapter.overflowCounts().eventIndices == 1, "event-index overflow count");
}

bool testAcrossBlockHeldNoteRetrigger()
{
  constexpr uint32_t frames = 8;
  uint32_t channels[] = {1};
  TestState state;
  auto plugin = makePlugin(state);
  Clap::AUv3::ProcessAdapter adapter;
  adapter.setupProcessing(0, nullptr, 1, channels, &plugin, nullptr, nullptr, frames,
                          CLAP_NOTE_DIALECT_CLAP);
  auto output = makeBufferList(1);
  float samples[frames] = {};
  output->mBuffers[0] = {1, sizeof(samples), samples};
  AudioUnitRenderActionFlags flags = 0;

  auto held = makeNoteOns(1);
  held[0].MIDI.eventSampleTime = 0;
  auto time = timestamp(0, 60);
  adapter.process(&flags, &time, frames, 0, output.get(), &held.front(), nil);

  std::vector<AURenderEvent> retrigger(2);
  retrigger[0].MIDI = held[0].MIDI;
  retrigger[0].MIDI.next = &retrigger[1];
  retrigger[0].MIDI.eventSampleTime = 8;
  retrigger[0].MIDI.data[0] = 0x80;
  retrigger[1].MIDI = held[0].MIDI;
  retrigger[1].MIDI.next = nullptr;
  retrigger[1].MIDI.eventSampleTime = 8;
  time = timestamp(8, 61);
  adapter.process(&flags, &time, frames, 0, output.get(), &retrigger.front(), nil);

  return expect(state.capturedEventCount == 3, "held retrigger captures all note events") &&
         expect(state.inputEvents[0].header.type == CLAP_EVENT_NOTE_ON &&
                    state.inputEvents[0].noteId == 0,
                "initial held note has stable id") &&
         expect(state.inputEvents[1].header.type == CLAP_EVENT_NOTE_OFF &&
                    state.inputEvents[1].noteId == 0 &&
                    state.inputEvents[2].header.type == CLAP_EVENT_NOTE_ON &&
                    state.inputEvents[2].noteId == 1,
                "across-block held retrigger keeps OFF old then ON new");
}

bool testActiveNoteAdmissionIsAtomic()
{
  constexpr uint32_t frames = 8;
  uint32_t channels[] = {1};
  TestState state;
  auto plugin = makePlugin(state);
  Clap::AUv3::ProcessAdapter adapter;
  Clap::AUv3::ProcessAdapter::Capacities capacities;
  capacities.activeNotes = 1;
  adapter.setupProcessing(0, nullptr, 1, channels, &plugin, nullptr, nullptr, frames,
                          CLAP_NOTE_DIALECT_CLAP, capacities);
  std::vector<AURenderEvent> events(3);
  for (size_t i = 0; i < events.size(); ++i)
  {
    events[i].MIDI.eventType = AURenderEventMIDI;
    events[i].MIDI.eventSampleTime = 0;
    events[i].MIDI.length = 3;
    events[i].MIDI.next = i + 1 < events.size() ? &events[i + 1] : nullptr;
  }
  events[0].MIDI.data[0] = 0x90;
  events[0].MIDI.data[1] = 60;
  events[0].MIDI.data[2] = 100;
  events[1].MIDI.data[0] = 0x90;
  events[1].MIDI.data[1] = 61;
  events[1].MIDI.data[2] = 100;
  events[2].MIDI.data[0] = 0x80;
  events[2].MIDI.data[1] = 61;
  events[2].MIDI.data[2] = 0;
  auto output = makeBufferList(1);
  float samples[frames] = {};
  output->mBuffers[0] = {1, sizeof(samples), samples};
  AudioUnitRenderActionFlags flags = 0;
  auto time = timestamp(0, 62);
  adapter.process(&flags, &time, frames, 0, output.get(), &events.front(), nil);

  return expect(state.inputEventCount == 2 && state.capturedEventCount == 2,
                "unadmitted NOTE_ON is not emitted") &&
         expect(state.inputEvents[0].header.type == CLAP_EVENT_NOTE_ON &&
                    state.inputEvents[0].noteKey == 60 && state.inputEvents[0].noteId == 0,
                "admitted NOTE_ON receives tracked id") &&
         expect(state.inputEvents[1].header.type == CLAP_EVENT_NOTE_OFF &&
                    state.inputEvents[1].noteKey == 61 && state.inputEvents[1].noteId == -1,
                "unadmitted note's NOTE_OFF is intentionally wildcard") &&
         expect(adapter.overflowCounts().activeNotes == 1, "active-note admission overflow counted");
}

bool testEventRegistry()
{
  Clap::EventRegistry registry;
  uint16_t first = UINT16_MAX;
  uint16_t same = UINT16_MAX;
  uint16_t second = UINT16_MAX;
  uint16_t invalid = 0;
  return expect(registry.query("test.ramp", &first) && first != 0 && first != UINT16_MAX,
                "event registry assigns nonzero id") &&
         expect(registry.query("test.ramp", &same) && same == first,
                "event registry is stable") &&
         expect(registry.query("test.other", &second) && second != first,
                "event registry assigns distinct ids") &&
         expect(!registry.query(nullptr, &invalid) && invalid == UINT16_MAX,
                "event registry rejects invalid names");
}

bool testTranslatedParameterRamp()
{
  constexpr uint32_t frames = 8;
  uint32_t channels[] = {1};
  TestState state;
  const clap_wrapper_plugin_auv3_param_ramp_t extension = {
      CLAP_WRAPPER_AUV3_PARAM_RAMP_ABI_VERSION, translateRamp};
  state.rampExtension = &extension;
  auto plugin = makePlugin(state);
  Clap::AUv3::ProcessAdapter adapter;
  int cookie = 7;
  adapter.setupProcessing(0, nullptr, 1, channels, &plugin, nullptr, nullptr, frames,
                          CLAP_NOTE_DIALECT_CLAP);
  adapter._cookieCache.emplace(42, &cookie);
  auto event = makeParameterRamp(103, 42, 0.75f, 1024);
  auto output = makeBufferList(1);
  float samples[frames] = {};
  output->mBuffers[0] = {1, sizeof(samples), samples};
  auto time = timestamp(100, 6);
  AudioUnitRenderActionFlags flags = 0;
  adapter.process(&flags, &time, frames, 0, output.get(), &event, nil);

  const auto &snapshot = state.inputEvents[0];
  const auto overflows = adapter.overflowCounts();
  return expect(state.inputEventCount == 1 && state.capturedEventCount == 1,
                "translated ramp is exactly one event") &&
         expect(snapshot.header.space_id == 21 && snapshot.header.type == 3,
                "translated ramp is custom rather than a value event") &&
         expect(snapshot.header.time == 3 && snapshot.rampDuration == 1024,
                "translated ramp preserves offset and full duration") &&
         expect(snapshot.rampParameterId == 42 && snapshot.rampParameterAddress == 42 &&
                    snapshot.rampCookie == &cookie,
                "translated ramp receives parameter identity and cookie") &&
         expect(overflows.parameterRampFallbacks == 0 &&
                    overflows.parameterRampTranslationFailures == 0,
                "translated ramp avoids fallback and translator failures");
}

bool testParameterRampFallbackAndCapacity()
{
  constexpr uint32_t frames = 8;
  uint32_t channels[] = {1};
  auto event = makeParameterRamp(0, 42, 0.5f, 32);
  float samples[frames] = {};
  auto output = makeBufferList(1);
  output->mBuffers[0] = {1, sizeof(samples), samples};
  AudioUnitRenderActionFlags flags = 0;

  TestState fallbackState;
  auto fallbackPlugin = makePlugin(fallbackState);
  Clap::AUv3::ProcessAdapter fallbackAdapter;
  fallbackAdapter.setupProcessing(0, nullptr, 1, channels, &fallbackPlugin, nullptr, nullptr, frames,
                                  CLAP_NOTE_DIALECT_CLAP);
  fallbackAdapter._cookieCache.emplace(42, nullptr);
  auto time = timestamp(0, 7);
  fallbackAdapter.process(&flags, &time, frames, 0, output.get(), &event, nil);

  TestState translatedState;
  const clap_wrapper_plugin_auv3_param_ramp_t extension = {
      CLAP_WRAPPER_AUV3_PARAM_RAMP_ABI_VERSION, translateRamp};
  translatedState.rampExtension = &extension;
  auto translatedPlugin = makePlugin(translatedState);
  Clap::AUv3::ProcessAdapter translatedAdapter;
  Clap::AUv3::ProcessAdapter::Capacities capacities;
  capacities.inputEvents = 0;
  capacities.eventIndices = 0;
  translatedAdapter.setupProcessing(0, nullptr, 1, channels, &translatedPlugin, nullptr, nullptr, frames,
                                    CLAP_NOTE_DIALECT_CLAP, capacities);
  translatedAdapter._cookieCache.emplace(42, nullptr);
  const auto capacitiesBefore = translatedAdapter.vectorCapacities();
  time = timestamp(1, 8);
  translatedAdapter.process(&flags, &time, frames, 0, output.get(), &event, nil);
  const auto capacitiesAfter = translatedAdapter.vectorCapacities();

  return expect(fallbackState.inputEventCount == 1 &&
                    fallbackState.inputEvents[0].header.type == CLAP_EVENT_PARAM_VALUE,
                "unextended plugin receives legacy value event") &&
         expect(fallbackAdapter.overflowCounts().parameterRampFallbacks == 1,
                "legacy ramp fallback is observable") &&
         expect(translatedState.inputEventCount == 0 &&
                    translatedAdapter.overflowCounts().inputEvents == 1,
                "custom ramp reports fixed-storage overflow") &&
         expect(capacitiesBefore.inputEvents == capacitiesAfter.inputEvents &&
                    capacitiesBefore.eventIndices == capacitiesAfter.eventIndices,
                "custom ramp overflow does not grow event vectors");
}

bool testMalformedParameterRampTranslation()
{
  constexpr uint32_t frames = 8;
  uint32_t channels[] = {1};
  TestState state;
  const clap_wrapper_plugin_auv3_param_ramp_t extension = {
      CLAP_WRAPPER_AUV3_PARAM_RAMP_ABI_VERSION, translateMalformedRamp};
  state.rampExtension = &extension;
  auto plugin = makePlugin(state);
  Clap::AUv3::ProcessAdapter adapter;
  adapter.setupProcessing(0, nullptr, 1, channels, &plugin, nullptr, nullptr, frames,
                          CLAP_NOTE_DIALECT_CLAP);
  adapter._cookieCache.emplace(42, nullptr);
  auto event = makeParameterRamp(0, 42, 0.5f, 12);
  auto output = makeBufferList(1);
  float samples[frames] = {};
  output->mBuffers[0] = {1, sizeof(samples), samples};
  auto time = timestamp(0, 81);
  AudioUnitRenderActionFlags flags = 0;
  adapter.process(&flags, &time, frames, 0, output.get(), &event, nil);
  return expect(state.inputEventCount == 0, "malformed translated ramp is dropped") &&
         expect(adapter.overflowCounts().parameterRampTranslationFailures == 1,
                "malformed translated ramp increments diagnostics");
}

bool testMIDI2EventLists()
{
  constexpr uint32_t frames = 8;
  uint32_t channels[] = {1};
  TestState state;
  auto plugin = makePlugin(state);
  Clap::AUv3::ProcessAdapter adapter;
  adapter.setupProcessing(0, nullptr, 1, channels, &plugin, nullptr, nullptr, frames,
                          CLAP_NOTE_DIALECT_CLAP);
  auto storage = makeMIDI2Event(103, 9, {{0x20abcdef}, {0x40901234, 0x56789abc},
                                         {0x50abcdef, 2, 3, 4}});
  auto output = makeBufferList(1);
  float samples[frames] = {};
  output->mBuffers[0] = {1, sizeof(samples), samples};
  auto time = timestamp(100, 9);
  AudioUnitRenderActionFlags flags = 0;
  adapter.process(&flags, &time, frames, 0, output.get(),
                  reinterpret_cast<const AURenderEvent *>(storage.data()), nil);

  const auto &one = state.inputEvents[0];
  const auto &two = state.inputEvents[1];
  const auto &four = state.inputEvents[2];
  return expect(state.inputEventCount == 3, "MIDI2 list splits 32/64/128-bit UMPs") &&
         expect(one.header.time == 3 && one.midi2Port == 9 && one.midi2Data[0] == 0x20abcdef &&
                    one.midi2Data[1] == 0 && one.midi2Data[2] == 0 && one.midi2Data[3] == 0,
                "32-bit UMP keeps offset cable and zero fill") &&
         expect(two.midi2Data[0] == 0x40901234 && two.midi2Data[1] == 0x56789abc &&
                    two.midi2Data[2] == 0 && two.midi2Data[3] == 0,
                "64-bit UMP keeps two words and zero fill") &&
         expect(four.midi2Data[0] == 0x50abcdef && four.midi2Data[1] == 2 &&
                    four.midi2Data[2] == 3 && four.midi2Data[3] == 4,
                "128-bit UMP keeps all four words");
}

bool testMalformedMIDI2EventList()
{
  constexpr uint32_t frames = 8;
  uint32_t channels[] = {1};
  TestState state;
  auto plugin = makePlugin(state);
  Clap::AUv3::ProcessAdapter adapter;
  adapter.setupProcessing(0, nullptr, 1, channels, &plugin, nullptr, nullptr, frames,
                          CLAP_NOTE_DIALECT_CLAP);
  auto storage = makeMIDI2Event(0, 0, {{0x40000000}});
  auto output = makeBufferList(1);
  float samples[frames] = {};
  output->mBuffers[0] = {1, sizeof(samples), samples};
  auto time = timestamp(0, 10);
  AudioUnitRenderActionFlags flags = 0;
  adapter.process(&flags, &time, frames, 0, output.get(),
                  reinterpret_cast<const AURenderEvent *>(storage.data()), nil);
  return expect(state.inputEventCount == 0, "truncated MIDI2 UMP is dropped") &&
         expect(adapter.overflowCounts().midi2Malformed == 1,
                "truncated MIDI2 UMP increments diagnostics");
}
}  // namespace

int main()
{
  bool ok = true;
  ok &= testDirectAndInPlace();
  ok &= testSingleBusSameTimestampIsFresh();
  ok &= testNullBufferFallback();
  ok &= testMultiBusCaching();
  ok &= testInterleavedFallback();
  ok &= testCapacityBoundaries();
  ok &= testEventIndexOverflow();
  ok &= testAcrossBlockHeldNoteRetrigger();
  ok &= testActiveNoteAdmissionIsAtomic();
  ok &= testEventRegistry();
  ok &= testTranslatedParameterRamp();
  ok &= testParameterRampFallbackAndCapacity();
  ok &= testMalformedParameterRampTranslation();
  ok &= testMIDI2EventLists();
  ok &= testMalformedMIDI2EventList();
  if (ok) std::cout << "AUv3 process tests passed\n";
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
