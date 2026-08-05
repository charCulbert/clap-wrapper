#include "standalone_host.h"
#include "entry.h"

#include "choc/audio/io/choc_RtAudioPlayer.h"

#include <algorithm>
#include <chrono>
#include <limits>

namespace freeaudio::clap_wrapper::standalone
{
namespace
{
constexpr uint32_t minimumSampleRate = 22050;
constexpr uint32_t maximumSampleRate = 192000;

struct SampleAccurateRtAudioMIDIPlayer final
    : choc::audio::io::RtAudioMIDIPlayer
{
  SampleAccurateRtAudioMIDIPlayer(
      const choc::audio::io::AudioDeviceOptions &options,
      std::function<void(const std::string &)> log)
      : RtAudioMIDIPlayer(options, std::move(log))
  {
    dispatcher.midiTimingGranularityFrames = 1;
  }
};

uint64_t deviceId(const std::string &backendId)
{
  uint64_t hash = 1469598103934665603ull;
  for (const auto byte : backendId)
  {
    hash ^= static_cast<unsigned char>(byte);
    hash *= 1099511628211ull;
  }
  return hash == 0 ? 1 : hash;
}

const StandaloneAudioDevice *findDevice(
    const std::vector<StandaloneAudioDevice> &devices, uint64_t id)
{
  const auto found = std::find_if(
      devices.begin(), devices.end(),
      [id](const auto &device) { return device.id == id; });
  return found == devices.end() ? nullptr : &*found;
}

std::vector<StandaloneAudioDevice> makeDevices(
    const std::vector<choc::audio::io::AudioDeviceInfo> &devices,
    uint32_t inputChannels, uint32_t outputChannels)
{
  std::vector<StandaloneAudioDevice> result;
  result.reserve(devices.size());
  for (const auto &device : devices)
    result.push_back(
        {deviceId(device.deviceID), device.deviceID, device.name,
         inputChannels, outputChannels});
  return result;
}
} // namespace

void StandaloneHost::refreshDeviceCaches()
{
  if (!audioPlayer) return;

  inputAudioDevices = makeDevices(
      audioPlayer->getAvailableInputDevices(),
      std::min<uint32_t>(2, std::max<uint32_t>(1, totalInputChannels)), 0);
  outputAudioDevices = makeDevices(
      audioPlayer->getAvailableOutputDevices(), 0,
      std::min<uint32_t>(2, std::max<uint32_t>(1, totalOutputChannels)));
  midiInputDevices = audioPlayer->getAvailableMIDIInputDevices();
  midiOutputDevices = audioPlayer->getAvailableMIDIOutputDevices();
  numMidiPorts = static_cast<uint32_t>(midiInputDevices.size());
}

std::tuple<uint64_t, uint64_t, int32_t>
StandaloneHost::getDefaultAudioInOutSampleRate()
{
  refreshDeviceCaches();
  const auto sampleRate = currentSampleRate.load(std::memory_order_acquire);
  return {audioInputDeviceID, audioOutputDeviceID,
          sampleRate > 0 ? sampleRate : 48000};
}

bool StandaloneHost::startAudioThread()
{
  // An all-zero saved route is the unconfigured default, not a usable RtAudio
  // stream. Fall back to the plugin's normal audio route in that case.
  const bool hasConfiguredAudio = startupAudioSet &&
                                  (startAudioInputUsed || startAudioOutputUsed);
  const auto input = hasConfiguredAudio ? startAudioIn : uint64_t{};
  const auto output = hasConfiguredAudio ? startAudioOut : uint64_t{};
  const auto sampleRate = hasConfiguredAudio ? startSampleRate : 48000;
  return startAudioThreadOn(
      input, hasConfiguredAudio ? startAudioInputChannels : 2,
      (hasConfiguredAudio ? startAudioInputUsed : true) && numAudioInputs > 0,
      output, hasConfiguredAudio ? startAudioOutputChannels : 2,
      (hasConfiguredAudio ? startAudioOutputUsed : true) && numAudioOutputs > 0,
      sampleRate);
}

std::vector<std::string> StandaloneHost::getCompiledApi()
{
  return audioPlayer ? audioPlayer->getAvailableAudioAPIs()
                     : std::vector<std::string>{};
}

std::vector<StandaloneAudioDevice> StandaloneHost::getInputAudioDevices()
{
  refreshDeviceCaches();
  return inputAudioDevices;
}

std::vector<StandaloneAudioDevice> StandaloneHost::getOutputAudioDevices()
{
  refreshDeviceCaches();
  return outputAudioDevices;
}

std::vector<int32_t> StandaloneHost::getSampleRates()
{
  std::vector<int32_t> result;
  if (!audioPlayer) return result;

  for (const auto sampleRate : audioPlayer->getAvailableSampleRates())
    if (sampleRate >= minimumSampleRate && sampleRate <= maximumSampleRate)
      result.push_back(static_cast<int32_t>(sampleRate));
  return result;
}

std::vector<uint32_t> StandaloneHost::getBufferSizes()
{
  return audioPlayer ? audioPlayer->getAvailableBlockSizes()
                     : std::vector<uint32_t>{16, 32, 48, 64, 96, 128, 196,
                                             224, 256, 320, 480, 512, 768,
                                             1024, 1536, 2048};
}

bool StandaloneHost::startAudioThreadOn(
    uint64_t inputDeviceID, uint32_t inputChannels, bool useInput,
    uint64_t outputDeviceID, uint32_t outputChannels, bool useOutput,
    int32_t requestedSampleRate)
{
  stopAudioThread();
  running.store(false, std::memory_order_release);

  const auto *input = findDevice(inputAudioDevices, inputDeviceID);
  const auto *output = findDevice(outputAudioDevices, outputDeviceID);

  choc::audio::io::AudioDeviceOptions options;
  options.sampleRate = static_cast<uint32_t>(
      std::clamp<int32_t>(requestedSampleRate, minimumSampleRate,
                          maximumSampleRate));
  options.blockSize = currentBufferSize == 0 ? 256 : currentBufferSize;
  options.inputChannelCount = useInput ? inputChannels : 0;
  options.outputChannelCount = useOutput ? outputChannels : 0;
  options.audioAPI = audioApiDisplayName;
  options.inputDeviceID = input != nullptr ? input->backendId : std::string{};
  options.outputDeviceID =
      followSystemDefaultOutput || output == nullptr
          ? std::string{}
          : output->backendId;
  options.midiClientName = "CLAP Wrapper";

  const auto selectedMidi = services.selectedMidiPortIds();
  const auto selectedMidiNames = [this, &selectedMidi]
  {
    std::vector<std::string> names;
    for (const auto id : selectedMidi)
      if (id > 0 && id <= midiInputDevices.size())
        names.push_back(midiInputDevices[static_cast<std::size_t>(id - 1)]);
    return names;
  }();
  options.shouldOpenMIDIInput =
      [all = !midiSelectionConfigured, selectedMidiNames](
          const std::string &name)
  {
    return all || std::find(selectedMidiNames.begin(), selectedMidiNames.end(),
                            name) != selectedMidiNames.end();
  };
  options.shouldOpenMIDIOutput = [](const std::string &) { return false; };

  auto candidate = std::make_unique<SampleAccurateRtAudioMIDIPlayer>(
      options, [](const std::string &message)
      {
        LOGDETAIL("CHOC audio: {}", message);
      });
  if (const auto error = candidate->getLastError(); !error.empty())
  {
    LOGINFO("[ERROR] CHOC audio reports '{}'", error);
    if (displayAudioError) displayAudioError(error);
    return false;
  }

  audioPlayer = std::move(candidate);
  refreshDeviceCaches();

  const auto &actual = audioPlayer->options;
  audioApiName = actual.audioAPI;
  audioApiDisplayName = actual.audioAPI;
  currentBufferSize = actual.blockSize;
  currentInputChannels = actual.inputChannelCount;
  currentOutputChannels = actual.outputChannelCount;
  audioInputUsed = actual.inputChannelCount != 0;
  audioOutputUsed = actual.outputChannelCount != 0;

  if (const auto device = std::find_if(
          inputAudioDevices.begin(), inputAudioDevices.end(),
          [&actual](const auto &candidateDevice)
          {
            return candidateDevice.backendId == actual.inputDeviceID;
          });
      device != inputAudioDevices.end())
    audioInputDeviceID = device->id;
  else
    audioInputDeviceID = 0;

  if (const auto device = std::find_if(
          outputAudioDevices.begin(), outputAudioDevices.end(),
          [&actual](const auto &candidateDevice)
          {
            return candidateDevice.backendId == actual.outputDeviceID;
          });
      device != outputAudioDevices.end())
    audioOutputDeviceID = device->id;
  else
    audioOutputDeviceID = 0;

  clap_wrapper_standalone_audio_settings_t currentSettings{};
  currentSettings.struct_size = sizeof(currentSettings);
  currentSettings.input_device_id = audioInputDeviceID;
  currentSettings.output_device_id = audioOutputDeviceID;
  currentSettings.input_channels = currentInputChannels;
  currentSettings.output_channels = currentOutputChannels;
  currentSettings.sample_rate = actual.sampleRate;
  currentSettings.buffer_size = actual.blockSize;
  if (audioInputUsed)
    currentSettings.flags |= CLAP_WRAPPER_STANDALONE_AUDIO_INPUT_ENABLED;
  if (audioOutputUsed)
    currentSettings.flags |= CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT_ENABLED;
  if (audioOutputUsed && followSystemDefaultOutput)
    currentSettings.flags |=
        CLAP_WRAPPER_STANDALONE_AUDIO_FOLLOW_DEFAULT_OUTPUT;
  services.recordAudioSettings(currentSettings);

  audioPlayer->addCallback(*this);
  if (!isActive.load(std::memory_order_acquire))
  {
    audioPlayer->removeCallback(*this);
    audioPlayer.reset();
    return false;
  }

  running.store(true, std::memory_order_release);
  services.setAudioRunning(true);
  finishedRunning.store(false, std::memory_order_release);
  return true;
}

void StandaloneHost::stopAudioThread()
{
  services.setAudioRunning(false);
  running.store(false, std::memory_order_release);
  if (audioPlayer)
  {
    audioPlayer->removeCallback(*this);
    audioPlayer.reset();
  }
  deactivatePlugin();
  services.resetAudioClock();
  finishedRunning.store(true, std::memory_order_release);
}

void StandaloneHost::sampleRateChanged(double sampleRate)
{
  if (sampleRate < minimumSampleRate || sampleRate > maximumSampleRate)
    return;

  currentSampleRate.store(static_cast<int32_t>(sampleRate),
                          std::memory_order_release);
  const auto maximumFrames =
      std::max<uint32_t>(1, currentBufferSize) * 2;
  if (!activatePlugin(static_cast<int32_t>(sampleRate), 1, maximumFrames))
    running.store(false, std::memory_order_release);
}

void StandaloneHost::startBlock()
{
  deviceInputChannels = 0;
  deviceOutputChannels = 0;
  deviceBlockFrames = 0;
  deviceMIDIEventCount = 0;
  deviceMIDIOutput = nullptr;
}

void StandaloneHost::processSubBlock(
    const choc::audio::AudioMIDIBlockDispatcher::Block &block,
    bool replaceOutput)
{
  (void)replaceOutput;

  if (deviceBlockFrames == 0)
  {
    deviceInputChannels = std::min<uint32_t>(
        block.audioInput.getNumChannels(),
        static_cast<uint32_t>(deviceInputPointers.size()));
    deviceOutputChannels = std::min<uint32_t>(
        block.audioOutput.getNumChannels(),
        static_cast<uint32_t>(deviceOutputPointers.size()));
    for (uint32_t channel = 0; channel < deviceInputChannels; ++channel)
      deviceInputPointers[channel] =
          block.audioInput.getIterator(channel).sample;
    for (uint32_t channel = 0; channel < deviceOutputChannels; ++channel)
      deviceOutputPointers[channel] =
          block.audioOutput.getIterator(channel).sample;
    deviceMIDIOutput = &block.onMidiOutputMessage;
  }

  for (const auto &message : block.midiMessages)
  {
    if (message.message.size() == 0 || message.message.size() > 3 ||
        deviceMIDIEventCount >= deviceMIDIEvents.size())
      continue;

    auto &event = deviceMIDIEvents[deviceMIDIEventCount++];
    event.frame = deviceBlockFrames;
    event.size = static_cast<uint8_t>(message.message.size());
    std::memcpy(event.data, message.message.data(), event.size);
  }
  deviceBlockFrames += block.audioOutput.getNumFrames();
}

void StandaloneHost::endBlock()
{
  if (deviceMIDIOutput == nullptr || deviceBlockFrames == 0) return;

  clapProcess(
      choc::buffer::createChannelArrayView(
          deviceInputPointers.data(), deviceInputChannels, deviceBlockFrames),
      choc::buffer::createChannelArrayView(
          deviceOutputPointers.data(), deviceOutputChannels,
          deviceBlockFrames),
      deviceMIDIEvents.data(), deviceMIDIEventCount, *deviceMIDIOutput);
}
} // namespace freeaudio::clap_wrapper::standalone
