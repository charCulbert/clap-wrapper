
#include <cassert>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
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

uint64_t doubleBits(double value)
{
  uint64_t bits{};
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

double bitsDouble(uint64_t bits)
{
  double value{};
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

std::string percentEncode(std::string_view value)
{
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  result.reserve(value.size());
  for (const auto byte : value)
  {
    const auto character = static_cast<unsigned char>(byte);
    if (std::isalnum(character) || character == '-' || character == '_' ||
        character == '.' || character == '~')
      result.push_back(static_cast<char>(character));
    else
    {
      result.push_back('%');
      result.push_back(hex[character >> 4]);
      result.push_back(hex[character & 0x0f]);
    }
  }
  return result;
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
      (settings->flags & CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT_ENABLED) != 0,
      (settings->flags & CLAP_WRAPPER_STANDALONE_AUDIO_FOLLOW_DEFAULT_OUTPUT) != 0);
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
    device.id = info.id;
    device.input_channels = info.inputChannels;
    device.output_channels = info.outputChannels;
    std::strncpy(device.name, info.name.c_str(), sizeof(device.name) - 1);
    inputs.push_back(device);
  }
  for (const auto &info : getOutputAudioDevices())
  {
    clap_wrapper_standalone_audio_device_t device{};
    device.struct_size = sizeof(device);
    device.id = info.id;
    device.input_channels = info.inputChannels;
    device.output_channels = info.outputChannels;
    std::strncpy(device.name, info.name.c_str(), sizeof(device.name) - 1);
    outputs.push_back(device);
  }
  services.setAudioDevices(std::move(inputs), std::move(outputs));
}

void StandaloneHost::refreshMidiServiceSnapshot()
{
  refreshDeviceCaches();
  std::vector<clap_wrapper_standalone_midi_port_t> ports;
  for (std::size_t i = 0; i < midiInputDevices.size(); ++i)
  {
    clap_wrapper_standalone_midi_port_t port{};
    port.struct_size = sizeof(port);
    port.id = i + 1;
    std::strncpy(port.name, midiInputDevices[i].c_str(),
                 sizeof(port.name) - 1);
    ports.push_back(port);
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
  setStartupAudio(audio.input_device_id,
                  audio.input_channels, audio.output_device_id, audio.output_channels,
                  static_cast<int>(audio.sample_rate),
                  (audio.flags & CLAP_WRAPPER_STANDALONE_AUDIO_INPUT_ENABLED) != 0,
                  (audio.flags & CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT_ENABLED) != 0,
                  (audio.flags & CLAP_WRAPPER_STANDALONE_AUDIO_FOLLOW_DEFAULT_OUTPUT) != 0);
  currentBufferSize = audio.buffer_size;
  return true;
}

const void *StandaloneHost::getExtension(const char *extension)
{
  if (extension != nullptr && !std::strcmp(extension, CLAP_WRAPPER_EXT_STANDALONE_SERVICES))
    return &standaloneServicesExtension;
  return nullptr;
}

void StandaloneHost::setupWrapperSpecifics(const clap_plugin_t *plugin)
{
  TRACE;
  paramIndication = plugin != nullptr
                        ? static_cast<const clap_plugin_param_indication_t *>(
                              plugin->get_extension(plugin, CLAP_EXT_PARAM_INDICATION))
                        : nullptr;
  if (paramIndication == nullptr && plugin != nullptr)
    paramIndication = static_cast<const clap_plugin_param_indication_t *>(
        plugin->get_extension(plugin, CLAP_EXT_PARAM_INDICATION_COMPAT));
}

void StandaloneHost::handlePluginOutputEvent(const clap_event_header_t *event) noexcept
{
  if (event == nullptr || !mappingMode.load(std::memory_order_acquire) ||
      event->space_id != CLAP_CORE_EVENT_SPACE_ID ||
      event->type != CLAP_EVENT_PARAM_GESTURE_BEGIN ||
      event->size < sizeof(clap_event_param_gesture_t))
    return;

  const auto &gesture = *reinterpret_cast<const clap_event_param_gesture_t *>(event);
  pendingGestureParamId.store(gesture.param_id, std::memory_order_relaxed);
  pendingGestureSequence.fetch_add(1, std::memory_order_release);
}

bool StandaloneHost::receiveWebviewMessage(const void *data, uint32_t size)
{
  if (data == nullptr || size == 0) return false;
  return handleMappingMessage(std::string_view(static_cast<const char *>(data), size));
}

bool StandaloneHost::handleMappingMessage(std::string_view message)
{
  constexpr std::string_view prefix = "standalone-map:";
  if (message.size() < prefix.size() || message.compare(0, prefix.size(), prefix) != 0)
    return false;

  const auto command = message.substr(prefix.size());
  mappingUIReady = command == "ready" || mappingUIReady;
  if (command == "ready")
  {
    syncMappingUI();
    return true;
  }
  if (command == "begin")
  {
    setMappingMode(true);
    return true;
  }
  if (command == "cancel")
  {
    setMappingMode(false);
    return true;
  }
  if (command == "clear-all")
  {
    const auto mappings = mappingRecords;
    for (const auto &mapping : mappings) clearMapping(mapping.parameterId);
    return true;
  }
  constexpr std::string_view clearPrefix = "clear:";
  if (command.size() >= clearPrefix.size() &&
      command.compare(0, clearPrefix.size(), clearPrefix) == 0)
  {
    const auto idText = command.substr(clearPrefix.size());
    char *end{};
    const auto id = std::strtoul(std::string(idText).c_str(), &end, 10);
    if (end != nullptr && *end == '\0' && id < invalidMappingParamId)
      clearMapping(static_cast<clap_id>(id));
    return true;
  }
  return true;
}

void StandaloneHost::syncMappingUI()
{
  if (!mappingUIReady) return;
  sendMappingMessage(mappingMode.load(std::memory_order_acquire) ? "mode:1" : "mode:0");
  for (const auto &mapping : mappingRecords)
    sendMappingMessage("mapped:" + std::to_string(mapping.parameterId) + ":" +
                       std::to_string(mapping.cc) + ":0:" + percentEncode(mapping.name));
}

void StandaloneHost::setMappingMode(bool enabled)
{
  mappingMode.store(enabled, std::memory_order_release);
  pendingGestureParamId.store(invalidMappingParamId, std::memory_order_release);
  learningParamId.store(invalidMappingParamId, std::memory_order_release);
  if (!enabled && capturedMappingState.exchange(0, std::memory_order_acq_rel) != 0)
    midiMappingTable.clearMapping(-1, capturedCC.load(std::memory_order_relaxed));
  servicedGestureSequence = pendingGestureSequence.load(std::memory_order_acquire);
  sendMappingMessage(enabled ? "mode:1" : "mode:0");
}

void StandaloneHost::sendMappingMessage(std::string_view message) const
{
  if (!mappingUIReady || !sendWebviewMessage) return;
  const auto fullMessage = std::string{"standalone-map:"} + std::string(message);
  sendWebviewMessage(fullMessage.data(), static_cast<uint32_t>(fullMessage.size()));
}

bool StandaloneHost::queryParameter(clap_id id, ParameterTarget &target) const
{
  if (clapPlugin == nullptr || clapPlugin->_ext._params == nullptr) return false;
  auto mainGuard = clapPlugin->AlwaysMainThread();
  clap_param_info_t info{};
  if (!clapPlugin->_ext._params->get_info(clapPlugin->_plugin, id, &info)) return false;
  if ((info.flags & CLAP_PARAM_IS_AUTOMATABLE) == 0 ||
      (info.flags & CLAP_PARAM_IS_READONLY) != 0)
    return false;
  target.id = id;
  target.min = info.min_value;
  target.max = info.max_value;
  target.flags = info.flags;
  const auto *terminator = static_cast<const char *>(std::memchr(info.name, '\0', sizeof(info.name)));
  target.name.assign(info.name,
                     terminator != nullptr ? static_cast<std::size_t>(terminator - info.name)
                                           : sizeof(info.name));
  return true;
}

void StandaloneHost::setParamMappingIndication(const MappingRecord &mapping, bool hasMapping)
{
  if (paramIndication == nullptr || clapPlugin == nullptr) return;
  auto mainGuard = clapPlugin->AlwaysMainThread();
  const auto label = hasMapping
                         ? std::string{"CC "} + std::to_string(mapping.cc)
                         : std::string{};
  const auto description = hasMapping ? std::string{"Mapped to "} + label : std::string{};
  paramIndication->set_mapping(clapPlugin->_plugin, mapping.parameterId, hasMapping, nullptr,
                               hasMapping ? label.c_str() : nullptr,
                               hasMapping ? description.c_str() : nullptr);
}

void StandaloneHost::clearMapping(clap_id parameterId)
{
  const auto found = std::find_if(mappingRecords.begin(), mappingRecords.end(),
                                  [parameterId](const auto &mapping)
                                  { return mapping.parameterId == parameterId; });
  if (found == mappingRecords.end()) return;
  midiMappingTable.clearMapping(found->channel, found->cc);
  setParamMappingIndication(*found, false);
  sendMappingMessage("unmapped:" + std::to_string(found->parameterId));
  mappingRecords.erase(found);
}

void StandaloneHost::applyCapturedMapping()
{
  if (capturedMappingState.exchange(0, std::memory_order_acq_rel) == 0) return;

  const auto parameterId = capturedParamId.load(std::memory_order_relaxed);
  const auto cc = capturedCC.load(std::memory_order_relaxed);
  const auto channel = capturedChannel.load(std::memory_order_relaxed) == 0
                           ? -1
                           : static_cast<int32_t>(capturedChannel.load(std::memory_order_relaxed) - 1);
  ParameterTarget target;
  if (parameterId == invalidMappingParamId || cc >= detail::StandaloneMidiMappingTable::ccCount ||
      !queryParameter(parameterId, target))
  {
    midiMappingTable.clearMapping(channel, cc);
    return;
  }

  for (auto it = mappingRecords.begin(); it != mappingRecords.end();)
  {
    if (it->parameterId == parameterId || (it->cc == cc && it->channel == channel))
    {
      midiMappingTable.clearMapping(it->channel, it->cc);
      setParamMappingIndication(*it, false);
      sendMappingMessage("unmapped:" + std::to_string(it->parameterId));
      it = mappingRecords.erase(it);
    }
    else
      ++it;
  }

  const auto effectiveBlock = audioBlockSequence.load(std::memory_order_acquire) + 1;
  midiMappingTable.setMapping(channel, cc, parameterId, target.min, target.max,
                              target.flags, effectiveBlock);
  MappingRecord mapping{parameterId, cc, channel, target.min, target.max, target.flags, target.name};
  mappingRecords.push_back(mapping);
  setParamMappingIndication(mapping, true);
  sendMappingMessage("mapped:" + std::to_string(parameterId) + ":" +
                    std::to_string(cc) + ":0:" + percentEncode(target.name));
}

void StandaloneHost::serviceMidiMapping()
{
  const auto mode = mappingMode.load(std::memory_order_acquire);
  const auto gestureSequence = pendingGestureSequence.load(std::memory_order_acquire);
  if (mode && gestureSequence != servicedGestureSequence)
  {
    servicedGestureSequence = gestureSequence;
    const auto parameterId = pendingGestureParamId.load(std::memory_order_acquire);
    ParameterTarget target;
    if (parameterId != invalidMappingParamId && queryParameter(parameterId, target))
    {
      learningMinBits.store(doubleBits(target.min), std::memory_order_relaxed);
      learningMaxBits.store(doubleBits(target.max), std::memory_order_relaxed);
      learningFlags.store(target.flags, std::memory_order_relaxed);
      learningParamId.store(parameterId, std::memory_order_release);
      sendMappingMessage("target:" + std::to_string(parameterId) + ":" + percentEncode(target.name));
    }
  }
  applyCapturedMapping();
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

void StandaloneHost::clapProcess(
    choc::buffer::ChannelArrayView<const float> input,
    choc::buffer::ChannelArrayView<float> output,
    const DeviceMIDIEvent *midi, uint32_t midiCount,
    const choc::audio::AudioMIDIBlockDispatcher::HandleMIDIMessageFn
        &midiOutput)
{
  const auto frameCount = output.getNumFrames();
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  services.beginAudioBlock(
      frameCount,
      currentSampleRate.load(std::memory_order_acquire) > 0
          ? static_cast<uint32_t>(
                currentSampleRate.load(std::memory_order_relaxed))
          : 0,
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()));
  const auto blockSequence = audioBlockSequence.fetch_add(1, std::memory_order_acq_rel) + 1;

  if (!running.load(std::memory_order_acquire) || !isActive.load(std::memory_order_acquire))
  {
    output.clear();
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

  float *bufferChanPtr[utilityBufferMaxChannels]{};
  clap_audio_buffer buffers[utilityBufferMaxChannels]{};  // probably twice as large
  size_t ptrIdx{0};
  size_t bufIdx{0};

  process.audio_inputs = &(buffers[0]);
  for (auto inp = 0U; inp < numAudioInputs; ++inp)
  {
    // For now assert sterep
    assert(inputChannelByBus[inp] == 2);
    const auto isMainInput =
        inp == mainInput && input.getNumChannels() != 0;
    if (isMainInput && input.getNumChannels() >= 2)
    {
      bufferChanPtr[ptrIdx++] = const_cast<float *>(
          input.getIterator(0).sample);
      bufferChanPtr[ptrIdx++] = const_cast<float *>(
          input.getIterator(1).sample);
    }
    else if (isMainInput)
    {
      const auto *monoInput = input.getIterator(0).sample;
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
    const auto isMainOutput =
        oup == mainOutput && output.getNumChannels() >= 2;
    bufferChanPtr[ptrIdx] =
        isMainOutput ? output.getIterator(0).sample
                     : &(utilityBuffer[ptrIdx][0]);
    ptrIdx++;
    bufferChanPtr[ptrIdx] =
        isMainOutput ? output.getIterator(1).sample
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

  for (uint32_t i = 0; i < midiCount; ++i)
  {
    const auto midiSize = static_cast<uint32_t>(
        std::min<std::size_t>(midi[i].size, sizeof(midi[i].data)));
    if (midiSize == 0) continue;

    clap_event_midi_t event{};
    event.header.size = sizeof(event);
    event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    event.header.type = CLAP_EVENT_MIDI;
    event.header.time = midi[i].frame;
    event.port_index = 0;
    std::memcpy(event.data, midi[i].data, midiSize);
    if (!pushInputEvent(&event.header))
    {
      services.rejectEvent();
      break;
    }

    const auto learningParam = learningParamId.load(std::memory_order_acquire);
    if (learningParam != invalidMappingParamId && midiSize == 3 &&
        (midi[i].data[0] & 0xf0u) == 0xb0u)
    {
      const auto cc = static_cast<uint32_t>(midi[i].data[1]);
      if (cc < detail::StandaloneMidiMappingTable::ccCount)
      {
        const auto min = bitsDouble(learningMinBits.load(std::memory_order_relaxed));
        const auto max = bitsDouble(learningMaxBits.load(std::memory_order_relaxed));
        const auto flags = learningFlags.load(std::memory_order_relaxed);
        if (midiMappingTable.setMapping(-1, cc, learningParam, min, max, flags,
                                         blockSequence + 1))
        {
          capturedParamId.store(learningParam, std::memory_order_relaxed);
          capturedCC.store(cc, std::memory_order_relaxed);
          capturedChannel.store(0, std::memory_order_relaxed);
          capturedMappingState.store(1, std::memory_order_release);
          learningParamId.store(invalidMappingParamId, std::memory_order_release);
        }
      }
    }

    detail::StandaloneMidiMappingTable::Value mapped;
    if (midiMappingTable.valueFor(midi[i].data, midiSize, blockSequence, mapped))
    {
      clap_event_param_value_t parameter{};
      parameter.header.size = sizeof(parameter);
      parameter.header.time = midi[i].frame;
      parameter.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      parameter.header.type = CLAP_EVENT_PARAM_VALUE;
      parameter.header.flags = CLAP_EVENT_IS_LIVE;
      parameter.param_id = mapped.paramId;
      parameter.note_id = -1;
      parameter.port_index = -1;
      parameter.channel = -1;
      parameter.key = -1;
      parameter.value = mapped.value;
      if (!pushInputEvent(&parameter.header))
      {
        services.rejectEvent();
        break;
      }
    }
  }

  // process() is the active bidirectional parameter transport.
  parameterFlushRequested.store(false, std::memory_order_release);
  currentMidiOutput = &midiOutput;
  clapPlugin->_plugin->process(clapPlugin->_plugin, &process);
  currentMidiOutput = nullptr;
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
  serviceMidiMapping();

  if (callbackRequested.exchange(false) && clapPlugin)
  {
    auto mainThread = clapPlugin->AlwaysMainThread();
    clapPlugin->_plugin->on_main_thread(clapPlugin->_plugin);
  }

  const auto dirtyGeneration = dirtyStateGeneration.load(std::memory_order_acquire);
  if (dirtyGeneration != observedDirtyStateGeneration)
  {
    observedDirtyStateGeneration = dirtyGeneration;
    dirtyStateSettlingTicks = 4;
  }
  else if (dirtyStateSettlingTicks != 0 && --dirtyStateSettlingTicks == 0 && saveDirtyState)
  {
    saveDirtyState();
  }

  if (!restartRequested.exchange(false)) return;

  const bool restartAudio = audioPlayer != nullptr && services.isAudioRunning();
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
  const auto priorFollowSystemDefaultOutput = followSystemDefaultOutput;
  const auto priorBufferSize = currentBufferSize;
  VectorOutputStream priorPluginState;
  clap_ostream_t priorStream{&priorPluginState, VectorOutputStream::write};
  const auto havePriorPluginState = clapPlugin->save(&priorStream);

  const auto restorePriorServices = [&]
  {
    services.restoreSettings(priorAudio, priorMidi);
    startupAudioSet = priorStartupSet;
    startAudioIn = priorStartupIn;
    startAudioOut = priorStartupOut;
    startAudioInputChannels = priorStartupInputChannels;
    startAudioOutputChannels = priorStartupOutputChannels;
    startSampleRate = priorStartupRate;
    startAudioInputUsed = priorStartupInputUsed;
    startAudioOutputUsed = priorStartupOutputUsed;
    followSystemDefaultOutput = priorFollowSystemDefaultOutput;
    currentBufferSize = priorBufferSize;
  };

  // Device restoration and plug-in state restoration are independent. A saved
  // audio or MIDI device that is no longer available must not discard otherwise
  // valid plug-in state, so keep the current devices and carry on.
  if (!restoreServiceSettings(envelope->audio, envelope->midiPortIds)) restorePriorServices();

  if (loadPluginState(clapPlugin, envelope->pluginState)) return restart();

  restorePriorServices();
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
