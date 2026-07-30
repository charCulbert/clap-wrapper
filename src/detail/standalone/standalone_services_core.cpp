#include "standalone_services_core.h"

#include <algorithm>
#include <cstddef>

namespace freeaudio::clap_wrapper::standalone::detail
{
namespace
{
template <typename T>
bool hasSize(const T &value) { return value.struct_size >= sizeof(T); }

template <typename T>
bool copySnapshot(const std::vector<T> &source, T *destination, uint32_t capacity, uint32_t &count)
{
  count = static_cast<uint32_t>(source.size());
  if (capacity < count || (count != 0 && destination == nullptr)) return false;
  if (count != 0) std::memcpy(destination, source.data(), count * sizeof(T));
  return true;
}
} // namespace

bool StandaloneServicesCore::hasInputDevice(uint64_t id) const
{
  return std::any_of(inputDevices.begin(), inputDevices.end(), [id](const auto &device) { return device.id == id; });
}

bool StandaloneServicesCore::hasOutputDevice(uint64_t id) const
{
  return std::any_of(outputDevices.begin(), outputDevices.end(), [id](const auto &device) { return device.id == id; });
}

bool StandaloneServicesCore::hasMidiPort(uint64_t id) const
{
  return std::any_of(midiPorts.begin(), midiPorts.end(), [id](const auto &port) { return port.id == id; });
}

bool StandaloneServicesCore::validateAudioSettings(
    const clap_wrapper_standalone_audio_settings_t &settings) const
{
  constexpr auto validFlags = CLAP_WRAPPER_STANDALONE_AUDIO_INPUT_ENABLED |
                              CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT_ENABLED;
  if ((settings.flags & ~validFlags) != 0) return false;

  const auto validateRoute = [](const auto &devices, uint64_t id, uint32_t channels, bool enabled,
                                auto availableChannels)
  {
    if (!enabled) return id == 0 && channels == 0;
    const auto found = std::find_if(devices.begin(), devices.end(),
                                   [id](const auto &device) { return device.id == id; });
    return found != devices.end() && channels != 0 && channels <= availableChannels(*found);
  };

  const auto inputEnabled = (settings.flags & CLAP_WRAPPER_STANDALONE_AUDIO_INPUT_ENABLED) != 0;
  const auto outputEnabled = (settings.flags & CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT_ENABLED) != 0;
  if (!validateRoute(inputDevices, settings.input_device_id, settings.input_channels, inputEnabled,
                     [](const auto &device) { return device.input_channels; }) ||
      !validateRoute(outputDevices, settings.output_device_id, settings.output_channels, outputEnabled,
                     [](const auto &device) { return device.output_channels; }))
    return false;
  return settings.flags != 0 || (settings.input_device_id == 0 && settings.output_device_id == 0 &&
                                 settings.input_channels == 0 && settings.output_channels == 0);
}

bool StandaloneServicesCore::getAudioSnapshot(clap_wrapper_standalone_audio_snapshot_t &snapshot) const
{
  if (!isMainThread() || !hasSize(snapshot)) return false;
  snapshot.selected = audioSettings;
  snapshot.input_device_count = static_cast<uint32_t>(inputDevices.size());
  snapshot.output_device_count = static_cast<uint32_t>(outputDevices.size());
  if (snapshot.input_device_capacity < snapshot.input_device_count ||
      snapshot.output_device_capacity < snapshot.output_device_count ||
      (snapshot.input_device_count != 0 && snapshot.input_devices == nullptr) ||
      (snapshot.output_device_count != 0 && snapshot.output_devices == nullptr))
    return false;
  return copySnapshot(inputDevices, snapshot.input_devices, snapshot.input_device_capacity,
                      snapshot.input_device_count) &&
         copySnapshot(outputDevices, snapshot.output_devices, snapshot.output_device_capacity,
                      snapshot.output_device_count);
}

bool StandaloneServicesCore::applyAudioSettings(const clap_wrapper_standalone_audio_settings_t &settings)
{
  if (!isMainThread() || isAudioRunning() || !hasSize(settings)) return false;
  if (!validateAudioSettings(settings)) return false;
  audioSettings = settings;
  audioSettings.struct_size = sizeof(audioSettings);
  return true;
}

bool StandaloneServicesCore::getMidiSnapshot(clap_wrapper_standalone_midi_snapshot_t &snapshot) const
{
  if (!isMainThread() || !hasSize(snapshot)) return false;
  snapshot.port_count = static_cast<uint32_t>(midiPorts.size());
  snapshot.selected_port_count = static_cast<uint32_t>(selectedMidiPorts.size());
  if (snapshot.port_capacity < snapshot.port_count || snapshot.selected_port_capacity < snapshot.selected_port_count ||
      (snapshot.port_count != 0 && snapshot.ports == nullptr) ||
      (snapshot.selected_port_count != 0 && snapshot.selected_port_ids == nullptr))
    return false;
  copySnapshot(midiPorts, snapshot.ports, snapshot.port_capacity, snapshot.port_count);
  if (snapshot.selected_port_count != 0)
    std::memcpy(snapshot.selected_port_ids, selectedMidiPorts.data(),
                snapshot.selected_port_count * sizeof(uint64_t));
  return true;
}

bool StandaloneServicesCore::setMidiPortOpen(uint64_t id, bool shouldOpen)
{
  if (!isMainThread() || isAudioRunning() || !hasMidiPort(id)) return false;
  const auto found = std::find(selectedMidiPorts.begin(), selectedMidiPorts.end(), id);
  midiSelectionConfigured = true;
  if (shouldOpen)
  {
    if (found != selectedMidiPorts.end()) return true;
    if (selectedMidiPorts.size() >= CLAP_WRAPPER_STANDALONE_MAX_SELECTED_MIDI_PORTS) return false;
    selectedMidiPorts.push_back(id);
  }
  else if (found != selectedMidiPorts.end())
  {
    selectedMidiPorts.erase(found);
  }
  return true;
}

uint32_t StandaloneServicesCore::drainEventsForBlock(uint32_t frameCount, IngressEvent *destination,
                                                      uint32_t capacity)
{
  if (destination == nullptr || capacity == 0) return 0;
  uint32_t count{};
  IngressEvent event;
  while (count < capacity &&
         ingress.popIf(event,
                       [this](const auto &candidate)
                       {
                         return candidate.timing.targetBlockSequence == 0 ||
                                candidate.timing.targetBlockSequence <= currentAudioBlock.sequence;
                       }))
  {
    if (event.timing.deviceTimestamp) mapTimestampToAudioBlock(event, frameCount);
    const auto *header = reinterpret_cast<const clap_event_header_t *>(event.bytes.data());
    if (header->time >= frameCount)
    {
      ingress.rejectDequeued();
      continue;
    }
    destination[count++] = event;
  }

  for (uint32_t i = 1; i < count; ++i)
  {
    auto value = destination[i];
    const auto *valueHeader = reinterpret_cast<const clap_event_header_t *>(value.bytes.data());
    auto j = i;
    while (j != 0)
    {
      const auto *previous = reinterpret_cast<const clap_event_header_t *>(destination[j - 1].bytes.data());
      if (previous->time < valueHeader->time ||
          (previous->time == valueHeader->time && destination[j - 1].timestampNs <= value.timestampNs))
        break;
      destination[j] = destination[j - 1];
      --j;
    }
    destination[j] = value;
  }
  return count;
}

void StandaloneServicesCore::mapTimestampToAudioBlock(IngressEvent &event, uint32_t frameCount) const
{
  auto *header = reinterpret_cast<clap_event_header_t *>(event.bytes.data());
  if (!event.timing.phaseValid || event.timing.sourceFrameCount == 0 || frameCount == 0)
  {
    header->time = 0;
    return;
  }
  const auto currentFrame = (static_cast<uint64_t>(event.timing.sourceFrame) * frameCount) /
                            event.timing.sourceFrameCount;
  header->time = static_cast<uint32_t>(std::min<uint64_t>(currentFrame, frameCount - 1));
}

IngressTiming StandaloneServicesCore::captureIngressTiming(uint64_t timestampNs,
                                                            bool useTimestamp) const
{
  const auto clock = readAudioClock();
  IngressTiming timing{};
  timing.deviceTimestamp = useTimestamp;
  const auto currentSequence =
      clock.current.sequence != 0
          ? clock.current.sequence
          : publishedCurrentSequence.load(std::memory_order_acquire);
  if (currentSequence != 0) timing.targetBlockSequence = currentSequence + 1;
  if (!useTimestamp || timestampNs == 0) return timing;

  const AudioBlockAnchor *source{};
  if (clock.current.startTimeNs != 0 && timestampNs >= clock.current.startTimeNs)
  {
    source = &clock.current;
  }
  else if (clock.previous.startTimeNs != 0 &&
           clock.current.startTimeNs > clock.previous.startTimeNs &&
           timestampNs >= clock.previous.startTimeNs &&
           timestampNs < clock.current.startTimeNs)
  {
    source = &clock.previous;
    timing.targetBlockSequence = clock.current.sequence;
  }
  if (source == nullptr || source->sampleRate == 0 || source->frameCount == 0) return timing;

  const auto elapsedNs = timestampNs - source->startTimeNs;
  const auto finalFrame = static_cast<uint64_t>(source->frameCount - 1);
  const auto finalFrameTimeNs = (finalFrame * 1000000000ull) / source->sampleRate;
  timing.sourceFrame = static_cast<uint32_t>(
      elapsedNs >= finalFrameTimeNs ? finalFrame
                                   : (elapsedNs * source->sampleRate) / 1000000000ull);
  timing.sourceFrameCount = source->frameCount;
  timing.phaseValid = true;
  return timing;
}

AudioClockSnapshot StandaloneServicesCore::readAudioClock() const
{
  for (int attempt = 0; attempt < 2; ++attempt)
  {
    const auto before = audioClockRevision.load(std::memory_order_acquire);
    if ((before & 1u) != 0) continue;
    AudioClockSnapshot result;
    result.previous.sequence = publishedPreviousSequence.load(std::memory_order_relaxed);
    result.previous.startTimeNs = publishedPreviousStartTimeNs.load(std::memory_order_relaxed);
    result.previous.sampleRate = publishedPreviousSampleRate.load(std::memory_order_relaxed);
    result.previous.frameCount = publishedPreviousFrameCount.load(std::memory_order_relaxed);
    result.current.sequence = publishedCurrentSequence.load(std::memory_order_relaxed);
    result.current.startTimeNs = publishedCurrentStartTimeNs.load(std::memory_order_relaxed);
    result.current.sampleRate = publishedCurrentSampleRate.load(std::memory_order_relaxed);
    result.current.frameCount = publishedCurrentFrameCount.load(std::memory_order_relaxed);
    if (audioClockRevision.load(std::memory_order_acquire) == before) return result;
  }
  return {};
}

void StandaloneServicesCore::publishAudioClock()
{
  audioClockRevision.fetch_add(1, std::memory_order_acq_rel);
  publishedPreviousSequence.store(previousAudioBlock.sequence, std::memory_order_relaxed);
  publishedPreviousStartTimeNs.store(previousAudioBlock.startTimeNs, std::memory_order_relaxed);
  publishedPreviousSampleRate.store(previousAudioBlock.sampleRate, std::memory_order_relaxed);
  publishedPreviousFrameCount.store(previousAudioBlock.frameCount, std::memory_order_relaxed);
  publishedCurrentSequence.store(currentAudioBlock.sequence, std::memory_order_relaxed);
  publishedCurrentStartTimeNs.store(currentAudioBlock.startTimeNs, std::memory_order_relaxed);
  publishedCurrentSampleRate.store(currentAudioBlock.sampleRate, std::memory_order_relaxed);
  publishedCurrentFrameCount.store(currentAudioBlock.frameCount, std::memory_order_relaxed);
  audioClockRevision.fetch_add(1, std::memory_order_release);
}

bool StandaloneServicesCore::restoreSettings(const clap_wrapper_standalone_audio_settings_t &audio,
                                             const std::vector<uint64_t> &midiPorts)
{
  if (!isMainThread() || isAudioRunning() || !hasSize(audio)) return false;
  if (!validateAudioSettings(audio) || midiPorts.size() > CLAP_WRAPPER_STANDALONE_MAX_SELECTED_MIDI_PORTS)
    return false;
  for (const auto id : midiPorts)
    if (!hasMidiPort(id)) return false;
  audioSettings = audio;
  audioSettings.struct_size = sizeof(audioSettings);
  selectedMidiPorts = midiPorts;
  midiSelectionConfigured = true;
  return true;
}
} // namespace freeaudio::clap_wrapper::standalone::detail
