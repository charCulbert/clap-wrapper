#include "detail/vst3/process.h"
#include "detail/vst3/parameter.h"

#include <public.sdk/source/vst/hosting/eventlist.h>
#include <public.sdk/source/vst/hosting/parameterchanges.h>

#include <cstdio>
#include <vector>

void utf8_to_utf16l(const char *text, uint16_t *target, size_t targetSize)
{
  for (size_t i = 0; i < targetSize; ++i)
  {
    target[i] = text[i] == '\0' ? 0 : static_cast<uint8_t>(text[i]);
    if (text[i] == '\0') return;
  }
}

namespace
{
bool expect(bool condition, const char *message)
{
  if (condition) return true;

  std::fprintf(stderr, "FAIL: %s\n", message);
  return false;
}

struct TestPlugin
{
  clap_plugin_t plugin{};
  std::vector<clap_event_header_t> inputEvents;
  std::vector<uint32_t> inputTimes;
  bool emittedOutput = false;

  TestPlugin()
  {
    plugin.plugin_data = this;
    plugin.process = process;
  }

  static clap_process_status process(const clap_plugin_t *plugin, const clap_process_t *process)
  {
    auto &self = *static_cast<TestPlugin *>(plugin->plugin_data);
    const auto count = process->in_events->size(process->in_events);
    for (uint32_t i = 0; i < count; ++i)
    {
      const auto *event = process->in_events->get(process->in_events, i);
      self.inputEvents.push_back(*event);
      self.inputTimes.push_back(event->time);
    }

    clap_event_note_t note{};
    note.header.size = sizeof(note);
    note.header.time = 43;
    note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    note.header.type = CLAP_EVENT_NOTE_ON;
    note.channel = 2;
    note.key = 61;
    note.note_id = 9;
    note.velocity = 0.5;

    clap_event_param_value_t parameter{};
    parameter.header.size = sizeof(parameter);
    parameter.header.time = 47;
    parameter.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    parameter.header.type = CLAP_EVENT_PARAM_VALUE;
    parameter.param_id = 17;
    parameter.value = 0.75;

    self.emittedOutput = process->out_events->try_push(process->out_events, &note.header) &&
                         process->out_events->try_push(process->out_events, &parameter.header);
    return CLAP_PROCESS_CONTINUE;
  }
};

bool testSampleOffsets()
{
  Steinberg::Vst::BusList inputs(Steinberg::Vst::kAudio, Steinberg::Vst::kInput);
  Steinberg::Vst::BusList outputs(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput);
  Steinberg::Vst::ParameterContainer parameters;
  std::vector<clap_id> gesturedParameters;
  TestPlugin plugin;

  clap_param_info_t parameterInfo{};
  parameterInfo.id = 17;
  parameterInfo.min_value = 0;
  parameterInfo.max_value = 1;
  parameterInfo.default_value = 0;
  parameters.addParameter(Vst3Parameter::create(&parameterInfo, {}));
  parameters.addParameter(Vst3Parameter::create(0, 3, 74, 18));

  Clap::ProcessAdapter adapter;
  adapter.setupProcessing(&plugin.plugin, nullptr, inputs, outputs, 64, 0, 0, parameters, nullptr,
                          nullptr, gesturedParameters, false, false);

  Steinberg::Vst::EventList inputEvents;
  Steinberg::Vst::Event note{};
  note.type = Steinberg::Vst::Event::kNoteOnEvent;
  note.sampleOffset = 23;
  note.noteOn.channel = 2;
  note.noteOn.pitch = 60;
  note.noteOn.noteId = 8;
  note.noteOn.velocity = 0.5f;
  inputEvents.addEvent(note);

  Steinberg::Vst::ParameterChanges inputParameters;
  Steinberg::int32 queueIndex = 0;
  auto *parameterQueue = inputParameters.addParameterData(17, queueIndex);
  Steinberg::int32 pointIndex = 0;
  parameterQueue->addPoint(11, 0.25, pointIndex);
  auto *midiQueue = inputParameters.addParameterData(18, queueIndex);
  midiQueue->addPoint(31, 0.5, pointIndex);

  Steinberg::Vst::EventList outputEvents;
  Steinberg::Vst::ParameterChanges outputParameters;
  Steinberg::Vst::ProcessData data{};
  data.numSamples = 64;
  data.inputEvents = &inputEvents;
  data.inputParameterChanges = &inputParameters;
  data.outputEvents = &outputEvents;
  data.outputParameterChanges = &outputParameters;

  adapter.process(data);

  bool result = expect(plugin.emittedOutput, "CLAP output events are accepted") &&
                expect(plugin.inputEvents.size() == 3, "three VST3 inputs reach CLAP") &&
                expect(plugin.inputEvents[0].type == CLAP_EVENT_PARAM_VALUE &&
                           plugin.inputTimes[0] == 11,
                       "VST3 parameter point keeps its nonzero sample offset") &&
                expect(plugin.inputEvents[1].type == CLAP_EVENT_NOTE_ON && plugin.inputTimes[1] == 23,
                       "VST3 note keeps its nonzero sample offset") &&
                expect(plugin.inputEvents[2].type == CLAP_EVENT_MIDI && plugin.inputTimes[2] == 31,
                       "VST3 MIDI parameter keeps its nonzero sample offset");

  Steinberg::Vst::Event outputNote{};
  result = expect(outputEvents.getEventCount() == 1, "one CLAP note reaches VST3") && result;
  if (outputEvents.getEventCount() == 1) outputEvents.getEvent(0, outputNote);
  result = expect(outputNote.sampleOffset == 43, "CLAP note output keeps sample offset") && result;

  auto *outputQueue = outputParameters.getParameterData(0);
  Steinberg::int32 outputOffset = 0;
  Steinberg::Vst::ParamValue outputValue = 0;
  result = expect(outputQueue != nullptr && outputQueue->getPoint(0, outputOffset, outputValue) == Steinberg::kResultOk &&
                      outputOffset == 47,
                  "CLAP parameter output keeps sample offset") && result;
  return result;
}

bool testSameFrameOrdering()
{
  Steinberg::Vst::BusList inputs(Steinberg::Vst::kAudio, Steinberg::Vst::kInput);
  Steinberg::Vst::BusList outputs(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput);
  Steinberg::Vst::ParameterContainer parameters;
  std::vector<clap_id> gesturedParameters;
  TestPlugin plugin;

  clap_param_info_t parameterInfo{};
  parameterInfo.id = 17;
  parameterInfo.max_value = 1;
  parameters.addParameter(Vst3Parameter::create(&parameterInfo, {}));

  Clap::ProcessAdapter adapter;
  adapter.setupProcessing(&plugin.plugin, nullptr, inputs, outputs, 64, 0, 0, parameters, nullptr,
                          nullptr, gesturedParameters, false, false);

  Steinberg::Vst::EventList inputEvents;
  Steinberg::Vst::Event note{};
  note.type = Steinberg::Vst::Event::kNoteOnEvent;
  note.sampleOffset = 19;
  inputEvents.addEvent(note);

  Steinberg::Vst::ParameterChanges inputParameters;
  Steinberg::int32 queueIndex = 0;
  auto *queue = inputParameters.addParameterData(17, queueIndex);
  Steinberg::int32 pointIndex = 0;
  queue->addPoint(19, 0.5, pointIndex);

  Steinberg::Vst::ProcessData data{};
  data.numSamples = 64;
  data.inputEvents = &inputEvents;
  data.inputParameterChanges = &inputParameters;
  adapter.process(data);

  return expect(plugin.inputEvents.size() == 2, "same-frame inputs reach CLAP") &&
         expect(plugin.inputEvents[0].type == CLAP_EVENT_NOTE_ON &&
                    plugin.inputEvents[1].type == CLAP_EVENT_PARAM_VALUE,
                "same-frame ordering is stable by insertion order");
}
}  // namespace

int main()
{
  return testSampleOffsets() && testSameFrameOrdering() ? 0 : 1;
}
