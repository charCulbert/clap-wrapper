#pragma once

/*
    AUv3 Process Adapter

    Copyright (c) 2024 Timo Kaluza (defiantnerd)

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.

    The AUv3 process adapter translates between the AUv3 render block model
    (AURenderEvent linked list, AURenderPullInputBlock) and the CLAP process model
    (clap_process_t with event lists and audio buffers).
*/

#include <clap/clap.h>
#include <clapwrapper/auv3-param-ramp.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"

#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#include <atomic>
#include <vector>
#include <map>
#include <unordered_map>
#include <limits>
#include "../clap/automation.h"
#include "../shared/fixedqueue.h"

namespace Clap::AUv3
{

// Consume one coalesced host flush request using an empty input event list.
// The caller supplies the appropriate output sink and thread context.
bool serviceParameterFlushRequest(const clap_plugin_t *plugin, const clap_plugin_params_t *params,
                                  std::atomic_bool *requestFlag, const clap_output_events_t *output);

typedef union clap_multi_event
{
  clap_event_header_t header;
  clap_event_note_t note;
  clap_event_midi_t midi;
  clap_event_midi2_t midi2;
  clap_event_midi_sysex_t sysex;
  clap_event_param_value_t param;
  clap_event_note_expression_t noteexpression;
  uint8_t custom[64];
} clap_multi_event_t;

class ProcessAdapter
{
 public:
  struct Capacities
  {
    static constexpr size_t defaultInputEvents = 8192;
    static constexpr size_t defaultEventIndices = 8192;
    static constexpr size_t defaultOutputEvents = 8192;
    static constexpr size_t defaultActiveNotes = 256;
    static constexpr size_t defaultReorderScratch = 8192;

    size_t inputEvents = defaultInputEvents;
    size_t eventIndices = defaultEventIndices;
    size_t outputEvents = defaultOutputEvents;
    size_t activeNotes = defaultActiveNotes;
    size_t reorderScratch = defaultReorderScratch;
  };

  struct OverflowCounts
  {
    uint64_t inputEvents = 0;
    uint64_t eventIndices = 0;
    uint64_t outputEvents = 0;
    uint64_t activeNotes = 0;
    uint64_t reorderScratch = 0;
    uint64_t parameterRampFallbacks = 0;
    uint64_t parameterRampTranslationFailures = 0;
    uint64_t midi2Malformed = 0;
    uint64_t inputBufferFallbacks = 0;
    uint64_t outputBufferFailures = 0;
  };

  struct VectorCapacities
  {
    size_t inputEvents = 0;
    size_t eventIndices = 0;
    size_t outputEvents = 0;
    size_t activeNotes = 0;
    size_t reorderScratch = 0;
  };

  ProcessAdapter() = default;
  ~ProcessAdapter();

  // Set up the processing state for the given bus configuration.
  // Called once when render resources are allocated.
  void setupProcessing(uint32_t numInputBusses, const uint32_t *inputChannelCounts,
                       uint32_t numOutputBusses, const uint32_t *outputChannelCounts,
                       const clap_plugin_t *plugin, const clap_plugin_params_t *ext_params,
                       Clap::IAutomation *automation, uint32_t numMaxSamples,
                       uint32_t preferredMIDIDialect);

  void setupProcessing(uint32_t numInputBusses, const uint32_t *inputChannelCounts,
                       uint32_t numOutputBusses, const uint32_t *outputChannelCounts,
                       const clap_plugin_t *plugin, const clap_plugin_params_t *ext_params,
                       Clap::IAutomation *automation, uint32_t numMaxSamples,
                       uint32_t preferredMIDIDialect, const Capacities &capacities);

  // Main render call - invoked from the AUv3 internalRenderBlock.
  // Translates AUv3 events, pulls input, calls CLAP process, and writes output.
  AUAudioUnitStatus process(AudioUnitRenderActionFlags *actionFlags, const AudioTimeStamp *timestamp,
                            AVAudioFrameCount frameCount, NSInteger outputBusNumber,
                            AudioBufferList *outputData, const AURenderEvent *realtimeEventListHead,
                            AURenderPullInputBlock __unsafe_unretained pullInputBlock);

  // Provide transport/musical context from the host
  void setTransportStateBlock(AUHostTransportStateBlock __nullable block);
  void setMusicalContextBlock(AUHostMusicalContextBlock __nullable block);

  // Append a parameter event to the current cycle's input events.
  // Render-thread only (mutates the event vectors process() reads).
  void addParameterEvent(clap_id paramId, double value, uint32_t sampleOffset);

  // Queue a host-side parameter change (AUParameter.setValue via
  // implementorValueObserver) for delivery in the next render cycle.
  // flush() is forbidden while the plugin is processing and the event
  // vectors are render-thread-owned, so this lock-free queue is the only
  // legal path while rendering. The fixedqueue is single-producer /
  // single-consumer: callers must serialize (the wrapper uses its
  // parameter-cache mutex); process() is the sole consumer.
  struct QueuedParamChange
  {
    clap_id id;
    double value;
  };
  void queueParameterChange(clap_id paramId, double value);

  // Drain the host-change queue outside rendering (the wrapper calls this
  // after stop_processing to flush changes that were parked while the last
  // cycles ran). Only legal when the render thread is quiesced.
  bool dequeueParameterChange(QueuedParamChange &out);

  // request_flush() coalesces notifications in this wrapper-owned flag. The
  // render thread consumes it and calls params.flush() before process().
  // Output uses the normal bounded staging, so parameter events do not coalesce.
  void setParameterFlushRequestFlag(std::atomic_bool *requestFlag);

  OverflowCounts overflowCounts() const;
  VectorCapacities vectorCapacities() const;
  uint64_t outputCopyCount() const;

  // MIDI output event block (set by the AU host)
  AUMIDIOutputEventBlock __nullable midiOutputEventBlock;

 private:
  static uint32_t input_events_size(const struct clap_input_events *list);
  static const clap_event_header_t *input_events_get(const struct clap_input_events *list,
                                                     uint32_t index);
  static bool output_events_try_push(const struct clap_output_events *list,
                                     const clap_event_header_t *event);

  void sortEventIndices();
  void serviceParameterFlushRequest();
  bool appendInputEvent(const clap_multi_event_t &event);
  bool enqueueOutputEvent(const clap_event_header_t *event);
  void translateAUv3Events(const AURenderEvent *head, AUEventSampleTime bufferStartTime,
                           AVAudioFrameCount frameCount);
  bool appendParameterRamp(const AUParameterEvent &event, clap_id parameterId, void *cookie,
                           uint32_t sampleOffset, AVAudioFrameCount frameCount);
  void appendMIDI2Events(const AUMIDIEventList &event, uint32_t sampleOffset);

  // Post-sort pass that guards against the AU scheduler reordering
  // same-block NOTE_ON/NOTE_OFF pairs. See process.mm for the full
  // rationale and behaviour.
  void reorderSameSampleOrphanOffs(AVAudioFrameCount frameCount);
  void finalizeNoteIdsAndActiveNotes();

 public:
  // Snapshot of the parameter-id → cookie map, copied at allocate time
  // (setupProcessing lifetime). The render thread only ever reads this
  // private copy, so main-thread cache rebuilds (param_rescan) can never
  // race the render path.
  std::unordered_map<clap_id, void *> _cookieCache;

 private:
  const clap_plugin_t *_plugin = nullptr;
  const clap_plugin_params_t *_ext_params = nullptr;
  std::atomic_bool *_parameterFlushRequestFlag = nullptr;
  const clap_wrapper_plugin_auv3_param_ramp_t *_paramRampExtension = nullptr;
  clap_wrapper_auv3_param_ramp_translate_t _paramRampTranslator = nullptr;
  Clap::IAutomation *_automation = nullptr;

  uint32_t _numInputs = 0;
  uint32_t _numOutputs = 0;

  clap_audio_buffer_t *_input_ports = nullptr;
  clap_audio_buffer_t *_output_ports = nullptr;
  clap_event_transport_t _transport = {};
  clap_input_events_t _in_events = {};
  clap_output_events_t _out_events = {};

  float *_silent_input = nullptr;
  float *_silent_output = nullptr;

  clap_process_t _processData = {-1, 0, &_transport, nullptr, nullptr, 0, 0, &_in_events, &_out_events};

  std::vector<clap_multi_event_t> _events;
  std::vector<size_t> _eventindices;
  std::vector<clap_multi_event_t> _outevents;
  Capacities _capacities;

  std::atomic<uint64_t> _inputEventOverflows{0};
  std::atomic<uint64_t> _eventIndexOverflows{0};
  std::atomic<uint64_t> _outputEventOverflows{0};
  std::atomic<uint64_t> _activeNoteOverflows{0};
  std::atomic<uint64_t> _reorderScratchOverflows{0};
  std::atomic<uint64_t> _parameterRampFallbacks{0};
  std::atomic<uint64_t> _parameterRampTranslationFailures{0};
  std::atomic<uint64_t> _midi2Malformed{0};
  std::atomic<uint64_t> _inputBufferFallbacks{0};
  std::atomic<uint64_t> _outputBufferFailures{0};
  std::atomic<uint64_t> _outputCopyCount{0};

  uint32_t _preferred_midi_dialect = CLAP_NOTE_DIALECT_CLAP;

  // Active note tracking for note expression targeting
  struct ActiveNote
  {
    bool used = false;
    int32_t note_id;
    int16_t port_index;
    int16_t channel;
    int16_t key;
  };
  std::vector<ActiveNote> _activeNotes;

  // Scratch for reorderSameSampleOrphanOffs — member so the render thread
  // never constructs/destroys a vector per cycle.
  std::vector<uint32_t> _reorderScratch;

  bool addToActiveNotes(const clap_event_note_t *note);
  void removeFromActiveNotes(const clap_event_note_t *note);

  // AUv3 MIDI events carry no per-note identifier, so the wrapper synthesizes
  // monotonically increasing note_ids at NOTE_ON time and pairs them back
  // with the matching NOTE_OFF / per-key expressions via the active-notes
  // shadow. Plugins that key voice tracking off note_id (rather than the
  // (channel, key) pair) get a usable identity instead of -1 everywhere.
  int32_t _nextNoteId = 0;
  int32_t synthesizeNoteId();
  int32_t lookupNoteId(int16_t port_index, int16_t channel, int16_t key) const;

  AUHostTransportStateBlock __nullable _transportStateBlock = nil;
  AUHostMusicalContextBlock __nullable _musicalContextBlock = nil;

  // Host parameter changes queued while rendering (see queueParameterChange).
  // The ring never wraps: producers count pending entries and drop the
  // newest change when full (the wrapper's value cache already holds it),
  // because fixedqueue silently discards everything on wrap-around.
  static constexpr uint32_t kHostParamQueueSize = 4096;
  ClapWrapper::detail::shared::fixedqueue<QueuedParamChange, kHostParamQueueSize> _hostParamChanges;
  std::atomic<uint32_t> _hostParamChangesCount{0};

  // Temporary storage for input pulling
  AudioBufferList *_inputBufferList = nullptr;
  uint32_t _inputBufferListChannels = 0;

  // Multi-bus render tracking: AUv3 calls the render block once per output bus,
  // but CLAP processes all buses in a single process() call. We run the CLAP
  // process on the first bus pulled in a render cycle (all pulls of one cycle
  // share the same AudioTimeStamp) and just copy stored output for the others.
  // A single-bus render never reuses a timestamp: its host buffers are only
  // borrowed for one invocation and matching timestamps can carry new events.
  // The key is (mSampleTime, mHostTime) so an engine restart that reuses a
  // sample time (new host time) is still processed. NaN sentinel: the first
  // comparison is always unequal.
  double _lastProcessedSampleTime = std::numeric_limits<double>::quiet_NaN();
  uint64_t _lastProcessedHostTime = 0;
  uint32_t _numMaxSamples = 0;
  std::vector<std::vector<float>> _inputStorage;
  std::vector<uint32_t> _inputStorageOffset;
  std::vector<std::vector<float>> _outputStorage;  // [_outputStorageOffset[bus] + ch][samples]
  std::vector<uint32_t> _outputStorageOffset;      // per-bus start index into _outputStorage
  std::vector<float> _interleavedOutputStorage;
  std::vector<float *> _directOutputPointers;
  bool _lastRenderUsedDirectOutput = false;
};

}  // namespace Clap::AUv3

#pragma clang diagnostic pop
