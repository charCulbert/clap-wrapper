
#include <cassert>
#include <chrono>
#include "standalone_host.h"
#include "standalone_settings.h"
#include <fstream>
#include <iterator>

// Standalone source lists are intentionally fixed by the format CMake module.
// Keep this core linked into that target without changing its build-format API.
#include "standalone_services_core.cpp"
#include "standalone_settings.cpp"

#if LIN && CLAP_WRAPPER_STANDALONE_X11
#include "detail/standalone/linux/x11_gui.h"
#endif

#if WIN
#if CLAP_WRAPPER_HAS_WIN32
#include <Windows.h>
#include <ShlObj.h>
#include <string>
#endif
#endif

namespace freeaudio::clap_wrapper::standalone
{
namespace
{
StandaloneHost *standaloneHostFor(const clap_host_t *host)
{
  if (host == nullptr || host->host_data == nullptr) return nullptr;
  auto *plugin = static_cast<Clap::Plugin *>(host->host_data);
  return static_cast<StandaloneHost *>(plugin->hostImplementation());
}

bool CLAP_ABI getAudioSnapshot(const clap_host_t *host, clap_wrapper_standalone_audio_snapshot_t *snapshot)
{
  auto *standalone = standaloneHostFor(host);
  if (standalone == nullptr || snapshot == nullptr || !standalone->services.isMainThread()) return false;
  standalone->refreshAudioServiceSnapshot();
  return standalone->services.getAudioSnapshot(*snapshot);
}

bool CLAP_ABI applyAudioSettings(const clap_host_t *host,
                                 const clap_wrapper_standalone_audio_settings_t *settings)
{
  auto *standalone = standaloneHostFor(host);
  if (standalone == nullptr || settings == nullptr || !standalone->services.isMainThread())
    return false;
  if (settings->struct_size < sizeof(*settings)) return false;
  if (((settings->flags & CLAP_WRAPPER_STANDALONE_AUDIO_INPUT_ENABLED) != 0 &&
       (settings->input_channels == 0 || settings->input_channels > 2)) ||
      ((settings->flags & CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT_ENABLED) != 0 &&
       settings->output_channels != 2))
    return false;
  if (!standalone->services.applyAudioSettings(*settings)) return false;
  standalone->setStartupAudio(
      static_cast<unsigned int>(settings->input_device_id),
      settings->input_channels, static_cast<unsigned int>(settings->output_device_id), settings->output_channels,
      static_cast<int>(settings->sample_rate),
      (settings->flags & CLAP_WRAPPER_STANDALONE_AUDIO_INPUT_ENABLED) != 0,
      (settings->flags & CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT_ENABLED) != 0);
  standalone->currentBufferSize = settings->buffer_size;
  return true;
}

bool CLAP_ABI getMidiSnapshot(const clap_host_t *host, clap_wrapper_standalone_midi_snapshot_t *snapshot)
{
  auto *standalone = standaloneHostFor(host);
  if (standalone == nullptr || snapshot == nullptr || !standalone->services.isMainThread()) return false;
  standalone->refreshMidiServiceSnapshot();
  return standalone->services.getMidiSnapshot(*snapshot);
}

bool CLAP_ABI setMidiPortOpen(const clap_host_t *host, uint64_t portId, bool shouldOpen)
{
  auto *standalone = standaloneHostFor(host);
  if (standalone == nullptr || !standalone->services.isMainThread()) return false;
  const auto restartAudio = standalone->services.isAudioRunning();
  const auto previousAudio = standalone->services.selectedAudioSettings();
  const auto previousMidi = standalone->services.selectedMidiPortIds();
  if (restartAudio)
  {
    standalone->stopAudioThread();
    standalone->deactivatePlugin();
  }
  standalone->refreshMidiServiceSnapshot();
  const auto changed = standalone->services.setMidiPortOpen(portId, shouldOpen);
  const auto rebound = changed && standalone->rebuildMIDIEndpoints();
  const auto restarted = rebound && (!restartAudio || standalone->startAudioThread());
  if (restarted) return true;

  const auto restored = standalone->services.restoreSettings(previousAudio, previousMidi);
  const auto restoredMidi = restored && standalone->rebuildMIDIEndpoints();
  const auto restoredAudio = !restartAudio || (restoredMidi && standalone->startAudioThread());
  if (!restoredMidi || !restoredAudio)
    LOGINFO("[ERROR] MIDI service change failed and prior configuration could not be restored");
  return false;
}

bool CLAP_ABI enqueueEvent(const clap_host_t *host, const clap_event_header_t *event, uint32_t eventSize,
                           uint64_t timestampNs)
{
  auto *standalone = standaloneHostFor(host);
  return standalone != nullptr && standalone->services.enqueueEvent(event, eventSize, timestampNs);
}

bool CLAP_ABI enqueueTimestampedEvent(const clap_host_t *host, const clap_event_header_t *event,
                                      uint32_t eventSize, uint64_t timestampNs)
{
  auto *standalone = standaloneHostFor(host);
  return standalone != nullptr &&
         standalone->services.enqueueTimestampedEvent(event, eventSize, timestampNs);
}

bool CLAP_ABI dequeueOutputEvent(const clap_host_t *host, void *event, uint32_t eventCapacity,
                                 clap_wrapper_standalone_output_event_info_t *info)
{
  auto *standalone = standaloneHostFor(host);
  return standalone != nullptr && info != nullptr &&
         standalone->services.dequeueOutputEvent(event, eventCapacity, *info);
}

bool CLAP_ABI getEventTelemetry(const clap_host_t *host,
                                clap_wrapper_standalone_event_telemetry_t *telemetry)
{
  auto *standalone = standaloneHostFor(host);
  if (standalone == nullptr || telemetry == nullptr ||
      telemetry->struct_size < sizeof(clap_wrapper_standalone_event_telemetry_t))
    return false;
  standalone->services.getTelemetry(*telemetry);
  return true;
}

bool CLAP_ABI getOutputEventTelemetry(const clap_host_t *host,
                                      clap_wrapper_standalone_event_telemetry_t *telemetry)
{
  auto *standalone = standaloneHostFor(host);
  if (standalone == nullptr || telemetry == nullptr ||
      telemetry->struct_size < sizeof(clap_wrapper_standalone_event_telemetry_t))
    return false;
  standalone->services.getOutputTelemetry(*telemetry);
  return true;
}

const clap_wrapper_host_standalone_services_t standaloneServicesExtension{
    CLAP_WRAPPER_STANDALONE_SERVICES_ABI_VERSION,
    sizeof(clap_wrapper_host_standalone_services_t),
    getAudioSnapshot,
    applyAudioSettings,
    getMidiSnapshot,
    setMidiPortOpen,
    enqueueEvent,
    getEventTelemetry,
    enqueueTimestampedEvent,
    dequeueOutputEvent,
    getOutputEventTelemetry};

struct VectorOutputStream
{
  std::vector<uint8_t> bytes;
  static int64_t write(const clap_ostream_t *stream, const void *buffer, uint64_t size)
  {
    auto *output = static_cast<VectorOutputStream *>(stream->ctx);
    if (size > static_cast<uint64_t>(std::numeric_limits<size_t>::max() - output->bytes.size())) return -1;
    const auto *first = static_cast<const uint8_t *>(buffer);
    output->bytes.insert(output->bytes.end(), first, first + static_cast<size_t>(size));
    return static_cast<int64_t>(size);
  }
};

struct VectorInputStream
{
  const std::vector<uint8_t> &bytes;
  size_t offset{};
  static int64_t read(const clap_istream_t *stream, void *buffer, uint64_t size)
  {
    auto *input = static_cast<VectorInputStream *>(stream->ctx);
    const auto available = input->bytes.size() - input->offset;
    const auto count = std::min<uint64_t>(size, available);
    if (count != 0)
    {
      std::memcpy(buffer, input->bytes.data() + input->offset, static_cast<size_t>(count));
      input->offset += static_cast<size_t>(count);
    }
    return static_cast<int64_t>(count);
  }
};

bool loadPluginState(const std::shared_ptr<Clap::Plugin> &plugin, const std::vector<uint8_t> &state)
{
  VectorInputStream input{state};
  clap_istream_t stream{&input, VectorInputStream::read};
  return plugin->load(&stream);
}

bool replaceSettingsFile(const fs::path &temporary, const fs::path &destination)
{
#if WIN && CLAP_WRAPPER_HAS_WIN32
  return MoveFileExW(temporary.wstring().c_str(), destination.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  std::error_code error;
  fs::rename(temporary, destination, error);
  return !error;
#endif
}
} // namespace

#if WIN && CLAP_WRAPPER_HAS_WIN32
std::optional<fs::path> getStandaloneSettingsPath()
{
  wchar_t *buffer{nullptr};

  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &buffer)))
  {
    fs::path data{std::wstring(buffer) + fs::path::preferred_separator + L"clap-wrapper-standalone"};
    CoTaskMemFree(buffer);

    if (!fs::exists(data)) fs::create_directory(data);

    return data;
  }

  return std::nullopt;
}
#elif !MAC
std::optional<fs::path> getStandaloneSettingsPath()
{
  TRACE;
  return std::nullopt;
}
#endif

StandaloneHost::~StandaloneHost()
{
}

void StandaloneHost::refreshAudioServiceSnapshot()
{
  std::vector<clap_wrapper_standalone_audio_device_t> inputs;
  std::vector<clap_wrapper_standalone_audio_device_t> outputs;
  for (const auto &info : getInputAudioDevices())
  {
    clap_wrapper_standalone_audio_device_t device{};
    device.struct_size = sizeof(device);
    device.id = info.ID;
    device.input_channels = info.inputChannels;
    device.output_channels = info.outputChannels;
    std::strncpy(device.name, info.name.c_str(), sizeof(device.name) - 1);
    inputs.push_back(device);
  }
  for (const auto &info : getOutputAudioDevices())
  {
    clap_wrapper_standalone_audio_device_t device{};
    device.struct_size = sizeof(device);
    device.id = info.ID;
    device.input_channels = info.inputChannels;
    device.output_channels = info.outputChannels;
    std::strncpy(device.name, info.name.c_str(), sizeof(device.name) - 1);
    outputs.push_back(device);
  }
  services.setAudioDevices(std::move(inputs), std::move(outputs));
}

void StandaloneHost::refreshMidiServiceSnapshot()
{
  std::vector<clap_wrapper_standalone_midi_port_t> ports;
  try
  {
    RtMidiIn midi;
    const auto count = midi.getPortCount();
    for (unsigned int i = 0; i < count; ++i)
    {
      clap_wrapper_standalone_midi_port_t port{};
      port.struct_size = sizeof(port);
      port.id = i + 1;
      std::strncpy(port.name, midi.getPortName(i).c_str(), sizeof(port.name) - 1);
      ports.push_back(port);
    }
  }
  catch (const RtMidiError &)
  {
  }
  services.setMidiPorts(std::move(ports));
}

bool StandaloneHost::restoreServiceSettings(const clap_wrapper_standalone_audio_settings_t &audio,
                                            const std::vector<uint64_t> &midiPorts)
{
  refreshAudioServiceSnapshot();
  refreshMidiServiceSnapshot();
  if (((audio.flags & CLAP_WRAPPER_STANDALONE_AUDIO_INPUT_ENABLED) != 0 &&
       (audio.input_channels == 0 || audio.input_channels > 2)) ||
      ((audio.flags & CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT_ENABLED) != 0 && audio.output_channels != 2))
    return false;
  if (!services.restoreSettings(audio, midiPorts)) return false;
  setStartupAudio(static_cast<unsigned int>(audio.input_device_id),
                  audio.input_channels, static_cast<unsigned int>(audio.output_device_id), audio.output_channels,
                  static_cast<int>(audio.sample_rate),
                  (audio.flags & CLAP_WRAPPER_STANDALONE_AUDIO_INPUT_ENABLED) != 0,
                  (audio.flags & CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT_ENABLED) != 0);
  currentBufferSize = audio.buffer_size;
  return true;
}

const void *StandaloneHost::getExtension(const char *extension)
{
  if (extension != nullptr && !std::strcmp(extension, CLAP_WRAPPER_EXT_STANDALONE_SERVICES))
    return &standaloneServicesExtension;
  return nullptr;
}

void StandaloneHost::setupAudioBusses(const clap_plugin_t *plugin,
                                      const clap_plugin_audio_ports_t *audioports)
{
  if (!audioports) return;
  numAudioInputs = audioports->count(plugin, true);
  numAudioOutputs = audioports->count(plugin, false);
  LOGDETAIL("inputs/outputs : {}/{}", numAudioInputs, numAudioOutputs);

  clap_audio_port_info_t info;
  for (auto i = 0U; i < numAudioInputs; ++i)
  {
    audioports->get(plugin, i, true, &info);
    inputChannelByBus.push_back(info.channel_count);
    totalInputChannels += info.channel_count;
    if (info.flags & CLAP_AUDIO_PORT_IS_MAIN) mainInput = i;
  }
  for (auto i = 0U; i < numAudioOutputs; ++i)
  {
    audioports->get(plugin, i, false, &info);
    outputChannelByBus.push_back(info.channel_count);
    totalOutputChannels += info.channel_count;
    if (info.flags & CLAP_AUDIO_PORT_IS_MAIN) mainOutput = i;
  }

  assert(totalOutputChannels + totalInputChannels < utilityBufferMaxChannels);
}

void StandaloneHost::setupMIDIBusses(const clap_plugin_t *plugin,
                                     const clap_plugin_note_ports_t *noteports)
{
  auto numMIDIInPorts = noteports->count(plugin, true);
  if (numMIDIInPorts > 0)
  {
    clap_note_port_info_t info;
    noteports->get(plugin, 0, true, &info);
    if (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI)
    {
      hasMIDIInput = true;
    }
    if (info.supported_dialects & CLAP_NOTE_DIALECT_CLAP)
    {
      hasClapNoteInput = true;
    }
  }
  auto numMIDIOutPorts = noteports->count(plugin, false);
  if (numMIDIOutPorts > 0)
  {
    createsMidiOutput = true;
    LOGINFO("[WARNING] Midi output is queued for standalone-services; no device forwarder is configured");
  }
}

void StandaloneHost::clapProcess(void *pOutput, const void *pInput, uint32_t frameCount)
{
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  services.beginAudioBlock(
      frameCount, currentSampleRate > 0 ? static_cast<uint32_t>(currentSampleRate) : 0,
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()));
  auto f = (float *)pOutput;

  if (!running.load(std::memory_order_acquire) || !isActive.load(std::memory_order_acquire))
  {
    if (f != nullptr && currentOutputChannels != 0)
      memset(f, 0, frameCount * currentOutputChannels * sizeof(float));
    finishedRunning = true;
    services.endAudioBlock();
    return;
  }

  clap_process process;
  process.transport = nullptr;  // this is a freefloating host
  process.in_events = &inputEvents;
  process.out_events = &outputEvents;
  process.frames_count = frameCount;
  process.audio_inputs_count = numAudioInputs;
  process.audio_outputs_count = numAudioOutputs;

  assert(frameCount < utilityBufferSize);
  if (frameCount >= utilityBufferSize)
  {
    LOGINFO("frameCount {} is beyond utility buffer size {}", frameCount, utilityBufferSize);
    std::terminate();
  }

  process.audio_outputs_count = 1;

  float *bufferChanPtr[utilityBufferMaxChannels]{};
  clap_audio_buffer buffers[utilityBufferMaxChannels]{};  // probably twice as large
  size_t ptrIdx{0};
  size_t bufIdx{0};

  process.audio_inputs = &(buffers[0]);
  for (auto inp = 0U; inp < numAudioInputs; ++inp)
  {
    // For now assert sterep
    assert(inputChannelByBus[inp] == 2);
    const auto isMainInput = inp == mainInput && pInput != nullptr;
    if (isMainInput && currentInputChannels >= 2)
    {
      bufferChanPtr[ptrIdx++] = static_cast<float *>(const_cast<void *>(pInput));
      bufferChanPtr[ptrIdx++] = static_cast<float *>(const_cast<void *>(pInput)) + frameCount;
    }
    else if (isMainInput && currentInputChannels == 1)
    {
      const auto *monoInput = static_cast<const float *>(pInput);
      bufferChanPtr[ptrIdx] = &(utilityBuffer[ptrIdx][0]);
      std::memcpy(bufferChanPtr[ptrIdx], monoInput, frameCount * sizeof(float));
      ptrIdx++;
      bufferChanPtr[ptrIdx] = &(utilityBuffer[ptrIdx][0]);
      std::memcpy(bufferChanPtr[ptrIdx], monoInput, frameCount * sizeof(float));
      ptrIdx++;
    }
    else
    {
      bufferChanPtr[ptrIdx] = &(utilityBuffer[ptrIdx][0]);
      memset(bufferChanPtr[ptrIdx++], 0, frameCount * sizeof(float));
      bufferChanPtr[ptrIdx] = &(utilityBuffer[ptrIdx][0]);
      memset(bufferChanPtr[ptrIdx++], 0, frameCount * sizeof(float));
    }

    buffers[bufIdx].channel_count = 2;
    buffers[bufIdx].data32 = &(bufferChanPtr[ptrIdx - 2]);

    bufIdx++;
  }

  process.audio_outputs = &(buffers[bufIdx]);
  for (auto oup = 0U; oup < numAudioOutputs; ++oup)
  {
    // For now assert sterep
    assert(outputChannelByBus[oup] == 2);
    const auto isMainOutput = oup == mainOutput && pOutput != nullptr && currentOutputChannels >= 2;
    bufferChanPtr[ptrIdx] = isMainOutput ? static_cast<float *>(pOutput) : &(utilityBuffer[ptrIdx][0]);
    ptrIdx++;
    bufferChanPtr[ptrIdx] = isMainOutput ? static_cast<float *>(pOutput) + frameCount
                                          : &(utilityBuffer[ptrIdx][0]);
    ptrIdx++;

    buffers[bufIdx].channel_count = 2;
    buffers[bufIdx].data32 = &(bufferChanPtr[ptrIdx - 2]);

    bufIdx++;
  }

  clearInputEvents();
  const auto staged = services.drainEventsForBlock(
      frameCount, ingressStaging.data(), static_cast<uint32_t>(maxEventsPerCycle - currInput));
  for (uint32_t i = 0; i < staged; ++i)
  {
    const auto &event = ingressStaging[i];
    if (!pushInputEvent(reinterpret_cast<const clap_event_header_t *>(event.bytes.data())))
    {
      services.rejectEvent();
      break;
    }
  }

  // process() is the active bidirectional parameter transport.
  parameterFlushRequested.store(false, std::memory_order_release);
  clapPlugin->_plugin->process(clapPlugin->_plugin, &process);
  services.endAudioBlock();

}

bool StandaloneHost::gui_can_resize()
{
  if (!clapPlugin) return false;

  auto g = clapPlugin->_ext._gui;
  if (!g) return false;

  auto res = g->can_resize(clapPlugin->_plugin);
  return res;
}

bool StandaloneHost::gui_request_resize(uint32_t width, uint32_t height)
{
  if (onRequestResize)
  {
    return onRequestResize(width, height);
  }
  return false;
}

const char *StandaloneHost::host_get_name()
{
  return "CLAP-Wrapper-As-Standalone";
}

void StandaloneHost::serviceParameterFlushRequestOnMainThread()
{
  if (!clapPlugin || !clapPlugin->_ext._params) return;

  parameterFlushLifecycle.serviceIfInactive(
      [&]
      {
        auto mainThread = clapPlugin->AlwaysMainThread();
        ClapWrapper::detail::shared::serviceParameterFlushRequest(
            clapPlugin->_plugin, clapPlugin->_ext._params, &parameterFlushRequested, &outputEvents);
      });
}

void StandaloneHost::serviceMainThreadRequests()
{
  serviceParameterFlushRequestOnMainThread();

  if (callbackRequested.exchange(false) && clapPlugin)
  {
    auto mainThread = clapPlugin->AlwaysMainThread();
    clapPlugin->_plugin->on_main_thread(clapPlugin->_plugin);
  }

  if (!restartRequested.exchange(false)) return;

  const bool restartAudio = rtaDac && rtaDac->isStreamRunning();
  if (restartAudio)
  {
    stopAudioThread();
    const auto restarted = startAudioThread();
    if (!restarted) running.store(false, std::memory_order_release);
    return;
  }

  const bool activated = activatePlugin(currentSampleRate, 1, currentBufferSize * 2);
  running.store(activated, std::memory_order_release);
  if (activated) finishedRunning.store(false, std::memory_order_release);
}

#if LIN

bool StandaloneHost::register_timer(uint32_t period_ms, clap_id *timer_id)
{
#if LIN && CLAP_WRAPPER_STANDALONE_X11
  assert(x11Gui);
  return x11Gui->register_timer(period_ms, timer_id);
#else
  return false;
#endif
}
bool StandaloneHost::unregister_timer(clap_id timer_id)
{
#if LIN && CLAP_WRAPPER_STANDALONE_X11
  assert(x11Gui);
  return x11Gui->unregister_timer(timer_id);
#else
  return false;
#endif
}

bool StandaloneHost::register_fd(int fd, clap_posix_fd_flags_t flags)
{
#if LIN && CLAP_WRAPPER_STANDALONE_X11
  return x11Gui->register_fd(fd, flags);
#else
  return false;
#endif
}
bool StandaloneHost::modify_fd(int fd, clap_posix_fd_flags_t flags)
{
  return true;
}
bool StandaloneHost::unregister_fd(int fd)
{
#if LIN && CLAP_WRAPPER_STANDALONE_X11
  return x11Gui->unregister_fd(fd);
#else
  return false;
#endif
}

#else
bool StandaloneHost::register_timer(uint32_t period_ms, clap_id *timer_id)
{
  return false;
}
bool StandaloneHost::unregister_timer(clap_id timer_id)
{
  return false;
}
#endif

bool StandaloneHost::saveStandaloneAndPluginSettings(const fs::path &intoDir, const fs::path &withName)
{
  if (!clapPlugin || !clapPlugin->_ext._state) return false;
  refreshAudioServiceSnapshot();
  refreshMidiServiceSnapshot();

  VectorOutputStream output;
  clap_ostream cos{};
  cos.ctx = &output;
  cos.write = VectorOutputStream::write;
  if (!clapPlugin->save(&cos)) return false;

  detail::StandaloneSettingsEnvelope envelope;
  envelope.audio = services.selectedAudioSettings();
  envelope.midiPortIds = services.selectedMidiPortIds();
  envelope.pluginState = std::move(output.bytes);
  const auto bytes = detail::writeSettingsEnvelope(envelope);
  if (bytes.empty()) return false;

  const auto destination = intoDir / withName;
  const auto temporary = destination.string() + ".tmp";
  std::ofstream ofs(temporary, std::ios::out | std::ios::binary | std::ios::trunc);
  if (!ofs.is_open()) return false;
  ofs.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  ofs.close();
  if (!ofs) return false;

  if (!replaceSettingsFile(temporary, destination))
  {
    LOGINFO("[ERROR] Unable to replace settings '{}'; temporary file retained", destination.u8string());
    return false;
  }

  return true;
}

bool StandaloneHost::tryLoadStandaloneAndPluginSettings(const fs::path &fromDir,
                                                        const fs::path &withName)
{
  auto fsp = fromDir / withName;
  std::ifstream ifs(fsp, std::ios::in | std::ios::binary);
  if (!ifs.is_open())
  {
    return false;
  }
  if (!clapPlugin || !clapPlugin->_ext._state)
  {
    return false;
  }
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(ifs), {}};
  ifs.close();
  const auto restartAudio = services.isAudioRunning();
  if (restartAudio)
  {
    stopAudioThread();
    stopMIDIThread();
    deactivatePlugin();
  }
  const auto restart = [this, restartAudio]
  {
    if (!restartAudio) return true;
    return rebuildMIDIEndpoints() && startAudioThread();
  };
  if (!detail::isSettingsEnvelope(bytes))
  {
    const auto loaded = loadPluginState(clapPlugin, bytes);
    return restart() && loaded;
  }

  const auto envelope = detail::readSettingsEnvelope(bytes);
  if (!envelope)
  {
    restart();
    return false;
  }
  const auto priorAudio = services.selectedAudioSettings();
  const auto priorMidi = services.selectedMidiPortIds();
  const auto priorStartupSet = startupAudioSet;
  const auto priorStartupIn = startAudioIn;
  const auto priorStartupOut = startAudioOut;
  const auto priorStartupInputChannels = startAudioInputChannels;
  const auto priorStartupOutputChannels = startAudioOutputChannels;
  const auto priorStartupRate = startSampleRate;
  const auto priorStartupInputUsed = startAudioInputUsed;
  const auto priorStartupOutputUsed = startAudioOutputUsed;
  const auto priorBufferSize = currentBufferSize;
  VectorOutputStream priorPluginState;
  clap_ostream_t priorStream{&priorPluginState, VectorOutputStream::write};
  const auto havePriorPluginState = clapPlugin->save(&priorStream);
  if (!restoreServiceSettings(envelope->audio, envelope->midiPortIds))
  {
    restart();
    return false;
  }
  const auto loadedNewState = loadPluginState(clapPlugin, envelope->pluginState);
  const auto restartedNewState = loadedNewState && restart();
  if (restartedNewState) return true;
  services.restoreSettings(priorAudio, priorMidi);
  startupAudioSet = priorStartupSet;
  startAudioIn = priorStartupIn;
  startAudioOut = priorStartupOut;
  startAudioInputChannels = priorStartupInputChannels;
  startAudioOutputChannels = priorStartupOutputChannels;
  startSampleRate = priorStartupRate;
  startAudioInputUsed = priorStartupInputUsed;
  startAudioOutputUsed = priorStartupOutputUsed;
  currentBufferSize = priorBufferSize;
  if (havePriorPluginState) loadPluginState(clapPlugin, priorPluginState.bytes);
  const auto restartedPriorState = restart();
  if (!restartedPriorState)
    LOGINFO("[ERROR] Settings load failed and prior audio configuration could not be restarted");
  return false;
}

bool StandaloneHost::activatePlugin(int32_t sr, int32_t minBlock, int32_t maxBlock)
{
  if (!clapPlugin) return false;

  if (isActive.load(std::memory_order_acquire))
    deactivatePlugin();

  const bool started = parameterFlushLifecycle.activateAndStart(
      [&]
      {
        LOGINFO("Activating plugin : sampleRate={} blockBounds={} to {}", sr, minBlock, maxBlock);
        clapPlugin->setSampleRate(sr);
        clapPlugin->setBlockSizes(minBlock, maxBlock);
        return clapPlugin->activate();
      },
      [&] { return clapPlugin->start_processing(); },
      [&] { clapPlugin->deactivate(); });

  isActive.store(started, std::memory_order_release);
  return started;
}

void StandaloneHost::deactivatePlugin()
{
  if (!clapPlugin || !isActive.load(std::memory_order_acquire)) return;

  parameterFlushLifecycle.deactivate(
      [&]
      {
        isActive.store(false, std::memory_order_release);
        clapPlugin->stop_processing();
        clapPlugin->deactivate();
      });
}

}  // namespace freeaudio::clap_wrapper::standalone
