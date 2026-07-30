#include "../src/detail/auv3/process.h"

#include <cstdlib>
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
};

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
  uint32_t previousTime = 0;
  for (uint32_t i = 0; i < state.inputEventCount; ++i)
  {
    auto *event = process->in_events->get(process->in_events, i);
    state.inputEventsSorted &= event != nullptr && (i == 0 || event->time >= previousTime);
    if (event) previousTime = event->time;
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

clap_plugin_t makePlugin(TestState &state)
{
  clap_plugin_t plugin{};
  plugin.plugin_data = &state;
  plugin.process = processPlugin;
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
  auto repeatedStatus = adapter.process(&flags, &time, frames, 0, repeated.get(), nullptr, nil);
  return ok && expect(repeatedStatus == noErr && state.processCalls == 1,
                      "repeated pull reuses the render") &&
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

  return expect(state.inputEventCount == 2 && state.inputEventsSorted, "bounded sorted input") &&
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
}  // namespace

int main()
{
  bool ok = true;
  ok &= testDirectAndInPlace();
  ok &= testNullBufferFallback();
  ok &= testMultiBusCaching();
  ok &= testInterleavedFallback();
  ok &= testCapacityBoundaries();
  ok &= testEventIndexOverflow();
  if (ok) std::cout << "AUv3 process tests passed\n";
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
