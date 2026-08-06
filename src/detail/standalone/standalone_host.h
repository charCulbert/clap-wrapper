#pragma once

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <mutex>
#include <optional>
#include <string_view>
#include <tuple>
#include <vector>

#include "standalone_details.h"

#include "detail/clap/fsutil.h"

#include "choc/audio/io/choc_AudioMIDIPlayer.h"

#include "clap_proxy.h"
#include "detail/shared/fixedqueue.h"
#include "detail/shared/parameter_flush.h"
#include "detail/standalone/standalone_midi_mapping.h"
#include "detail/standalone/standalone_services_core.h"

#include <clap/ext/param-indication.h>

namespace freeaudio::clap_wrapper::standalone
{
#if LIN && CLAP_WRAPPER_STANDALONE_X11
namespace linux_standalone
{
struct X11Gui;
}
#endif

std::optional<fs::path> getStandaloneSettingsPath();

struct StandaloneAudioDevice
{
  uint64_t id{};
  std::string backendId;
  std::string name;
  uint32_t inputChannels{};
  uint32_t outputChannels{};
};

struct StandaloneHost : Clap::IHost, private choc::audio::io::AudioMIDICallback
{
  StandaloneHost()
  {
    inputEvents.ctx = this;
    inputEvents.size = ie_getsize;
    inputEvents.get = ie_get;
    outputEvents.ctx = this;
    outputEvents.try_push = oe_try_push;
  }
  virtual ~StandaloneHost();

  static bool oe_try_push(const struct clap_output_events *oe, const clap_event_header_t *evt)
  {
    auto *sh = static_cast<StandaloneHost *>(oe->ctx);
    if (sh == nullptr) return false;
    const auto accepted = sh->services.enqueueOutputEvent(evt);
    if (!accepted) return false;
    sh->handlePluginOutputEvent(evt);
    if (sh->currentMidiOutput != nullptr &&
        evt->space_id == CLAP_CORE_EVENT_SPACE_ID &&
        evt->type == CLAP_EVENT_MIDI && evt->size == sizeof(clap_event_midi_t))
    {
      const auto *midi = reinterpret_cast<const clap_event_midi_t *>(evt);
      (*sh->currentMidiOutput)(
          evt->time, choc::midi::MessageView(midi->data, sizeof(midi->data)));
    }
    return true;
  }

  static uint32_t ie_getsize(const struct clap_input_events *ie)
  {
    auto sh = (StandaloneHost *)ie->ctx;
    return sh->inputEventSize();
  }
  static const clap_event_header_t *ie_get(const struct clap_input_events *ie, uint32_t idx)
  {
    auto sh = (StandaloneHost *)ie->ctx;
    return sh->inputEvent(idx);
  }

  // A mapped CC adds one parameter event beside the raw MIDI event.
  static constexpr int maxMidiEventsPerCycle{1024};
  static constexpr int maxEventsPerCycle{maxMidiEventsPerCycle * 2};
  static constexpr int eventSize{1024};
  static constexpr int queueSize{eventSize * maxEventsPerCycle};
  alignas(std::max_align_t) unsigned char eventQueue[queueSize]{};
  std::array<detail::StandaloneServicesCore::IngressEvent, maxEventsPerCycle> ingressStaging{};

  int currInput{0};
  void clearInputEvents()
  {
    currInput = 0;
  }
  bool pushInputEvent(const clap_event_header_t *event)
  {
    if (event->size > eventSize)
    {
      return false;
    }
    if (currInput >= maxEventsPerCycle)
    {
      return false;
    }
    memcpy((void *)(eventQueue + currInput * eventSize), (const void *)event, event->size);
    currInput++;
    return true;
  }
  uint32_t inputEventSize()
  {
    return currInput;
  }
  const clap_event_header_t *inputEvent(uint32_t idx)
  {
    return (const clap_event_header_t *)(eventQueue + idx * eventSize);
  }

  std::shared_ptr<Clap::Plugin> clapPlugin;
  void setPlugin(std::shared_ptr<Clap::Plugin> plugin)
  {
    this->clapPlugin = plugin;
  }

  void mark_dirty() override
  {
    dirtyStateGeneration.fetch_add(1, std::memory_order_release);
  }
  std::atomic<uint64_t> dirtyStateGeneration{0};
  uint64_t observedDirtyStateGeneration{0};
  uint32_t dirtyStateSettlingTicks{0};
  std::function<void()> saveDirtyState;
  std::atomic<bool> restartRequested{false};
  void restartPlugin() override
  {
    restartRequested = true;
  }
  std::atomic<bool> callbackRequested{false};
  void request_callback() override
  {
    callbackRequested = true;
  }
  void setupWrapperSpecifics(const clap_plugin_t *plugin) override;
  const void *getExtension(const char *extension) override;

  bool saveStandaloneAndPluginSettings(const fs::path &intoDir, const fs::path &withName);
  bool tryLoadStandaloneAndPluginSettings(const fs::path &fromDir, const fs::path &withName);

  uint32_t numAudioInputs{0}, numAudioOutputs{0};
  std::vector<uint32_t> inputChannelByBus;
  std::vector<uint32_t> outputChannelByBus;
  uint32_t mainInput{0}, mainOutput{0};
  uint32_t totalInputChannels{0}, totalOutputChannels{0};
  void setupAudioBusses(const clap_plugin_t *plugin,
                        const clap_plugin_audio_ports_t *audioports) override;

  bool hasMIDIInput{false}, hasClapNoteInput{false}, createsMidiOutput{false};
  void setupMIDIBusses(const clap_plugin_t *plugin, const clap_plugin_note_ports_t *noteports) override;

  void setupParameters(const clap_plugin_t *plugin, const clap_plugin_params_t *params) override
  {
    TRACE;
  }
  void param_rescan(clap_param_rescan_flags flags) override
  {
    TRACE;
  }
  void param_clear(clap_id param, clap_param_clear_flags flags) override
  {
    TRACE;
  }
  void param_request_flush() override
  {
    parameterFlushRequested.store(true, std::memory_order_release);
  }
  std::atomic_bool parameterFlushRequested{false};
  void serviceParameterFlushRequestOnMainThread();
  void serviceMainThreadRequests();
  bool receiveWebviewMessage(const void *data, uint32_t size);
  void syncMappingUI();

  bool gui_can_resize() override;

  std::function<bool(uint32_t, uint32_t)> onRequestResize = [](auto, auto) { return false; };
  bool gui_request_resize(uint32_t width, uint32_t height) override;

  bool gui_request_show() override
  {
    TRACE;
    return false;
  }
  bool gui_request_hide() override
  {
    TRACE;
    return false;
  }

  bool supportsWebview() const override
  {
#if MAC
    return true;
#else
    return false;
#endif
  }

  bool webviewSend(const void *buffer, uint32_t size) override
  {
    return sendWebviewMessage && sendWebviewMessage(buffer, size);
  }

  std::function<bool(const void *, uint32_t)> sendWebviewMessage;

#if LIN && CLAP_WRAPPER_STANDALONE_X11
  freeaudio::clap_wrapper::standalone::linux_standalone::X11Gui *x11Gui{nullptr};
#endif

  bool register_timer(uint32_t period_ms, clap_id *timer_id) override;
  bool unregister_timer(clap_id timer_id) override;

  const char *host_get_name() override;

  bool track_info_get(clap_track_info_t *info) override
  {
    return false;
  }

  // context menu extension
  bool supportsContextMenu() const override
  {
    return false;
  }
  bool context_menu_populate(const clap_context_menu_target_t *target,
                             const clap_context_menu_builder_t *builder) override
  {
    return false;
  }
  bool context_menu_perform(const clap_context_menu_target_t *target, clap_id action_id) override
  {
    return false;
  }
  bool context_menu_can_popup() override
  {
    return false;
  }
  bool context_menu_popup(const clap_context_menu_target_t *target, int32_t screen_index, int32_t x,
                          int32_t y) override
  {
    return false;
  }

#if LIN
  bool register_fd(int fd, clap_posix_fd_flags_t flags) override;
  bool modify_fd(int fd, clap_posix_fd_flags_t flags) override;
  bool unregister_fd(int fd) override;

#endif

  void latency_changed() override
  {
    TRACE;
  }
  void tail_changed() override
  {
    TRACE;
  }

  // MIDI is owned by the CHOC AudioMIDIPlayer.
  uint32_t numMidiPorts{0};
  // Kept for the existing Windows UI; the standalone-services extension owns
  // selection for new integrations.
  std::vector<uint32_t> currentMidiPorts;
  bool startMIDIThread();
  void stopMIDIThread();
  bool rebuildMIDIEndpoints();
  std::function<bool(uint32_t)> testMidiPortOpen;

  struct DeviceMIDIEvent
  {
    uint32_t frame{};
    uint8_t size{};
    uint8_t data[3]{};
  };

  void clapProcess(
      choc::buffer::ChannelArrayView<const float> input,
      choc::buffer::ChannelArrayView<float> output,
      const DeviceMIDIEvent *midi, uint32_t midiCount,
      const choc::audio::AudioMIDIBlockDispatcher::HandleMIDIMessageFn
          &midiOutput);

  // Audio and MIDI device ownership lives behind CHOC's backend-neutral layer.
  std::unique_ptr<choc::audio::io::AudioMIDIPlayer> audioPlayer;
  std::function<void(const std::string &)> displayAudioError{nullptr};
  std::string audioApiName;
  std::string audioApiDisplayName;
  uint64_t audioInputDeviceID{0}, audioOutputDeviceID{0};
  bool audioInputUsed{true}, audioOutputUsed{true};
  std::atomic<int32_t> currentSampleRate{0};
  uint32_t currentBufferSize{0};
  uint32_t currentInputChannels{0}, currentOutputChannels{0};
  std::vector<StandaloneAudioDevice> inputAudioDevices;
  std::vector<StandaloneAudioDevice> outputAudioDevices;
  std::vector<std::string> midiInputDevices;
  std::vector<std::string> midiOutputDevices;
  bool midiSelectionConfigured{false};
  void refreshDeviceCaches();
  std::tuple<uint64_t, uint64_t, int32_t> getDefaultAudioInOutSampleRate();
  [[nodiscard]] bool startAudioThread();
  [[nodiscard]] bool startAudioThreadOn(uint64_t inputDeviceID, uint32_t inputChannels,
                                        bool useInput, uint64_t outputDeviceID,
                                        uint32_t outputChannels, bool useOutput, int32_t sampleRate);
  void stopAudioThread();

  bool startupAudioSet{false};
  uint64_t startAudioIn{0}, startAudioOut{0};
  uint32_t startAudioInputChannels{0}, startAudioOutputChannels{0};
  int startSampleRate{0};
  bool startAudioInputUsed{false}, startAudioOutputUsed{false};
  bool followSystemDefaultOutput{true};
  void setStartupAudio(uint64_t in, uint32_t inputChannels, uint64_t out, uint32_t outputChannels,
                       int sr, bool useInput, bool useOutput, bool followDefaultOutput)
  {
    startupAudioSet = true;
    startAudioIn = in;
    startAudioOut = out;
    startAudioInputChannels = inputChannels;
    startAudioOutputChannels = outputChannels;
    startSampleRate = sr;
    startAudioInputUsed = useInput;
    startAudioOutputUsed = useOutput;
    followSystemDefaultOutput = followDefaultOutput;
  }

  bool activatePlugin(int32_t sr, int32_t minBlock, int32_t maxBlock);
  void deactivatePlugin();
  std::atomic_bool isActive{false};
  ClapWrapper::detail::shared::ParameterFlushLifecycle parameterFlushLifecycle;

  detail::StandaloneServicesCore services;
  void refreshAudioServiceSnapshot();
  void refreshMidiServiceSnapshot();
  bool restoreServiceSettings(const clap_wrapper_standalone_audio_settings_t &audio,
                              const std::vector<uint64_t> &midiPorts);

  std::vector<std::string> getCompiledApi();
  std::vector<StandaloneAudioDevice> getInputAudioDevices();
  std::vector<StandaloneAudioDevice> getOutputAudioDevices();
  std::vector<int32_t> getSampleRates();
  std::vector<uint32_t> getBufferSizes();

  clap_input_events inputEvents{};
  clap_output_events outputEvents{};
  const choc::audio::AudioMIDIBlockDispatcher::HandleMIDIMessageFn
      *currentMidiOutput{};

  std::atomic<bool> running{true}, finishedRunning{false};

  static constexpr uint32_t invalidMappingParamId = detail::StandaloneMidiMappingTable::invalidParamId;

  struct MappingRecord
  {
    clap_id parameterId = invalidMappingParamId;
    uint32_t cc = 0;
    int32_t channel = -1;
    double min = 0.0;
    double max = 1.0;
    uint32_t flags = 0;
    std::string name;
  };

  struct ParameterTarget
  {
    clap_id id = invalidMappingParamId;
    double min = 0.0;
    double max = 1.0;
    uint32_t flags = 0;
    std::string name;
  };

  void handlePluginOutputEvent(const clap_event_header_t *event) noexcept;
  void serviceMidiMapping();
  bool handleMappingMessage(std::string_view message);
  bool queryParameter(clap_id id, ParameterTarget& target) const;
  void sendMappingMessage(std::string_view message) const;
  void setMappingMode(bool enabled);
  void clearMapping(clap_id parameterId);
  void applyCapturedMapping();
  void setParamMappingIndication(const MappingRecord& mapping, bool hasMapping);

  const clap_plugin_param_indication_t *paramIndication{};
  detail::StandaloneMidiMappingTable midiMappingTable;
  std::vector<MappingRecord> mappingRecords;
  std::atomic<bool> mappingMode{false};
  std::atomic<clap_id> pendingGestureParamId{invalidMappingParamId};
  std::atomic<uint64_t> pendingGestureSequence{0};
  uint64_t servicedGestureSequence{};
  std::atomic<clap_id> learningParamId{invalidMappingParamId};
  std::atomic<uint64_t> learningMinBits{};
  std::atomic<uint64_t> learningMaxBits{};
  std::atomic<uint32_t> learningFlags{};
  std::atomic<uint32_t> capturedMappingState{};
  std::atomic<clap_id> capturedParamId{invalidMappingParamId};
  std::atomic<uint32_t> capturedCC{};
  std::atomic<uint32_t> capturedChannel{};
  std::atomic<uint64_t> audioBlockSequence{};
  bool mappingUIReady{};

  // We need to have play buffers for the clap. For now lets assume
  // (1) the standalone is never more than 64 total ins and outs and
  // (2) the block size is less that 4096 * 16 and
  // (3) memory in the standalone is pretty cheap
  static constexpr int utilityBufferSize{4096 * 16};
  static constexpr int utilityBufferMaxChannels{64};
  float utilityBuffer[utilityBufferMaxChannels][utilityBufferSize]{};

 private:
  void sampleRateChanged(double sampleRate) override;
  void startBlock() override;
  void processSubBlock(const choc::audio::AudioMIDIBlockDispatcher::Block &block,
                       bool replaceOutput) override;
  void endBlock() override;

  std::array<const float *, utilityBufferMaxChannels> deviceInputPointers{};
  std::array<float *, utilityBufferMaxChannels> deviceOutputPointers{};
  std::array<DeviceMIDIEvent, maxMidiEventsPerCycle> deviceMIDIEvents{};
  uint32_t deviceInputChannels{};
  uint32_t deviceOutputChannels{};
  uint32_t deviceBlockFrames{};
  uint32_t deviceMIDIEventCount{};
  const choc::audio::AudioMIDIBlockDispatcher::HandleMIDIMessageFn
      *deviceMIDIOutput{};
};
}  // namespace freeaudio::clap_wrapper::standalone
