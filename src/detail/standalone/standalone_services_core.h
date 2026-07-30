#pragma once

#include <array>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include <clapwrapper/standalone-services.h>

namespace freeaudio::clap_wrapper::standalone::detail
{
struct AudioBlockAnchor
{
  uint64_t sequence{};
  uint64_t startTimeNs{};
  uint32_t sampleRate{};
  uint32_t frameCount{};
};

struct AudioClockSnapshot
{
  AudioBlockAnchor previous{};
  AudioBlockAnchor current{};
};

struct IngressTiming
{
  uint64_t targetBlockSequence{};
  uint32_t sourceFrame{};
  uint32_t sourceFrameCount{};
  bool deviceTimestamp{};
  bool phaseValid{};
};

template <size_t Capacity, size_t EventSize>
class EventIngress
{
 public:
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

  struct Event
  {
    uint64_t timestampNs{};
    uint32_t size{};
    IngressTiming timing{};
    clap_wrapper_standalone_output_event_info_t outputInfo{};
    alignas(std::max_align_t) std::array<unsigned char, EventSize> bytes{};
  };

  EventIngress()
  {
    for (size_t i = 0; i < Capacity; ++i)
      cells[i].sequence.store(i, std::memory_order_relaxed);
  }

  bool push(const clap_event_header_t *event, uint32_t size, uint64_t timestampNs,
            const IngressTiming &timing = {},
            const clap_wrapper_standalone_output_event_info_t *outputInfo = nullptr)
  {
    return pushWithPublishHook(event, size, timestampNs, timing, outputInfo, [] {});
  }

  template <typename BeforePublish>
  bool pushWithPublishHook(const clap_event_header_t *event, uint32_t size, uint64_t timestampNs,
                           const IngressTiming &timing,
                           const clap_wrapper_standalone_output_event_info_t *outputInfo,
                           BeforePublish &&beforePublish)
  {
    if (event == nullptr || size < sizeof(clap_event_header_t) || size > EventSize ||
        event->size != size)
    {
      rejected.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    size_t position = enqueuePosition.load(std::memory_order_relaxed);
    for (;;)
    {
      auto &cell = cells[position & (Capacity - 1)];
      const auto sequence = cell.sequence.load(std::memory_order_acquire);
      const auto difference = static_cast<intptr_t>(sequence) - static_cast<intptr_t>(position);
      if (difference == 0)
      {
        if (enqueuePosition.compare_exchange_weak(position, position + 1, std::memory_order_relaxed))
        {
          cell.event.timestampNs = timestampNs;
          cell.event.size = size;
          cell.event.timing = timing;
          cell.event.outputInfo = outputInfo != nullptr
                                      ? *outputInfo
                                      : clap_wrapper_standalone_output_event_info_t{};
          std::memcpy(cell.event.bytes.data(), event, size);
          std::forward<BeforePublish>(beforePublish)();
          cell.sequence.store(position + 1, std::memory_order_release);
          accepted.fetch_add(1, std::memory_order_relaxed);
          return true;
        }
      }
      else if (difference < 0)
      {
        dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      else
      {
        position = enqueuePosition.load(std::memory_order_relaxed);
      }
    }
  }

  bool pop(Event &result)
  {
    return popIf(result, [](const Event &) { return true; });
  }

  template <typename Predicate>
  bool popIf(Event &result, Predicate &&predicate)
  {
    const auto position = dequeuePosition;
    auto &cell = cells[position & (Capacity - 1)];
    if (cell.sequence.load(std::memory_order_acquire) != position + 1)
      return false;
    if (!std::forward<Predicate>(predicate)(cell.event)) return false;

    result = cell.event;
    cell.sequence.store(position + Capacity, std::memory_order_release);
    dequeuePosition = position + 1;
    consumed.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  void rejectDequeued() { rejected.fetch_add(1, std::memory_order_relaxed); }

  void getTelemetry(clap_wrapper_standalone_event_telemetry_t &telemetry) const
  {
    telemetry.event_capacity = static_cast<uint32_t>(Capacity);
    telemetry.event_size_capacity = static_cast<uint32_t>(EventSize);
    telemetry.accepted_events = accepted.load(std::memory_order_relaxed);
    telemetry.dropped_events = dropped.load(std::memory_order_relaxed);
    telemetry.rejected_events = rejected.load(std::memory_order_relaxed);
    telemetry.consumed_events = consumed.load(std::memory_order_relaxed);
  }

 private:
  struct Cell
  {
    std::atomic_size_t sequence{};
    Event event{};
  };

  std::array<Cell, Capacity> cells{};
  std::atomic_size_t enqueuePosition{};
  size_t dequeuePosition{}; // one audio consumer
  std::atomic_uint64_t accepted{};
  std::atomic_uint64_t dropped{};
  std::atomic_uint64_t rejected{};
  std::atomic_uint64_t consumed{};
};

template <size_t Capacity, size_t EventSize>
class OutputEventQueue
{
 public:
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

  struct Event
  {
    uint32_t size{};
    clap_wrapper_standalone_output_event_info_t info{};
    alignas(std::max_align_t) std::array<unsigned char, EventSize> bytes{};
  };

  bool push(const clap_event_header_t *event,
            const clap_wrapper_standalone_output_event_info_t &info)
  {
    if (event == nullptr || event->size < sizeof(clap_event_header_t) || event->size > EventSize)
    {
      rejected.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    const auto write = writePosition.load(std::memory_order_relaxed);
    if (write - readPosition.load(std::memory_order_acquire) >= Capacity)
    {
      dropped.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    auto &destination = events[write & (Capacity - 1)];
    destination.size = event->size;
    destination.info = info;
    std::memcpy(destination.bytes.data(), event, event->size);
    writePosition.store(write + 1, std::memory_order_release);
    accepted.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  bool pop(Event &result)
  {
    const auto read = readPosition.load(std::memory_order_relaxed);
    if (read == writePosition.load(std::memory_order_acquire)) return false;
    result = events[read & (Capacity - 1)];
    readPosition.store(read + 1, std::memory_order_release);
    consumed.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  void getTelemetry(clap_wrapper_standalone_event_telemetry_t &telemetry) const
  {
    telemetry.event_capacity = static_cast<uint32_t>(Capacity);
    telemetry.event_size_capacity = static_cast<uint32_t>(EventSize);
    telemetry.accepted_events = accepted.load(std::memory_order_relaxed);
    telemetry.dropped_events = dropped.load(std::memory_order_relaxed);
    telemetry.rejected_events = rejected.load(std::memory_order_relaxed);
    telemetry.consumed_events = consumed.load(std::memory_order_relaxed);
  }

 private:
  std::array<Event, Capacity> events{};
  std::atomic_size_t writePosition{};
  std::atomic_size_t readPosition{};
  std::atomic_uint64_t accepted{};
  std::atomic_uint64_t dropped{};
  std::atomic_uint64_t rejected{};
  std::atomic_uint64_t consumed{};
};

class StandaloneServicesCore
{
 public:
  static constexpr uint32_t eventCapacity = 256;
  static constexpr uint32_t eventSizeCapacity = 1024;

  StandaloneServicesCore() : mainThread(std::this_thread::get_id())
  {
    audioSettings.struct_size = sizeof(audioSettings);
  }

  bool isMainThread() const { return std::this_thread::get_id() == mainThread; }
  bool isAudioRunning() const { return audioRunning.load(std::memory_order_acquire); }
  void setAudioRunning(bool running) { audioRunning.store(running, std::memory_order_release); }

  bool setAudioDevices(std::vector<clap_wrapper_standalone_audio_device_t> inputs,
                       std::vector<clap_wrapper_standalone_audio_device_t> outputs)
  {
    if (!isMainThread()) return false;
    inputDevices = std::move(inputs);
    outputDevices = std::move(outputs);
    return true;
  }

  bool setMidiPorts(std::vector<clap_wrapper_standalone_midi_port_t> ports)
  {
    if (!isMainThread()) return false;
    midiPorts = std::move(ports);
    selectedMidiPorts.erase(std::remove_if(selectedMidiPorts.begin(), selectedMidiPorts.end(),
                                           [this](uint64_t id) { return !hasMidiPort(id); }),
                            selectedMidiPorts.end());
    if (!midiSelectionConfigured)
      for (const auto &port : midiPorts)
        if (selectedMidiPorts.size() < CLAP_WRAPPER_STANDALONE_MAX_SELECTED_MIDI_PORTS)
          selectedMidiPorts.push_back(port.id);
    return true;
  }

  bool getAudioSnapshot(clap_wrapper_standalone_audio_snapshot_t &snapshot) const;
  bool applyAudioSettings(const clap_wrapper_standalone_audio_settings_t &settings);
  void recordAudioSettings(const clap_wrapper_standalone_audio_settings_t &settings)
  {
    if (isMainThread())
    {
      audioSettings = settings;
      audioSettings.struct_size = sizeof(audioSettings);
    }
  }
  bool getMidiSnapshot(clap_wrapper_standalone_midi_snapshot_t &snapshot) const;
  bool setMidiPortOpen(uint64_t id, bool shouldOpen);
  bool setBoundMidiPortIds(std::vector<uint64_t> ids)
  {
    if (!isMainThread()) return false;
    for (const auto id : ids)
      if (!hasMidiPort(id)) return false;
    selectedMidiPorts = std::move(ids);
    midiSelectionConfigured = true;
    return true;
  }
  bool enqueueEvent(const clap_event_header_t *event, uint32_t size, uint64_t timestampNs)
  {
    return ingress.push(event, size, timestampNs, captureIngressTiming(timestampNs, false));
  }
  bool enqueueTimestampedEvent(const clap_event_header_t *event, uint32_t size, uint64_t timestampNs)
  {
    return ingress.push(event, size, timestampNs, captureIngressTiming(timestampNs, true));
  }
  template <typename BeforePublish>
  bool enqueueTimestampedEventWithPublishHook(const clap_event_header_t *event, uint32_t size,
                                              uint64_t timestampNs,
                                              BeforePublish &&beforePublish)
  {
    return ingress.pushWithPublishHook(
        event, size, timestampNs, captureIngressTiming(timestampNs, true), nullptr,
        std::forward<BeforePublish>(beforePublish));
  }
  bool enqueueOutputEvent(const clap_event_header_t *event)
  {
    clap_wrapper_standalone_output_event_info_t info{};
    info.struct_size = sizeof(info);
    info.event_size = event != nullptr ? event->size : 0;
    if (audioBlockActive)
    {
      info.flags = CLAP_WRAPPER_STANDALONE_OUTPUT_EVENT_HAS_BLOCK_CONTEXT;
      info.sample_rate = currentAudioBlock.sampleRate;
      info.frame_count = currentAudioBlock.frameCount;
      info.block_sequence = currentAudioBlock.sequence;
      info.block_start_time_ns = currentAudioBlock.startTimeNs;
    }
    return output.push(event, info);
  }
  bool dequeueOutputEvent(void *destination, uint32_t capacity,
                          clap_wrapper_standalone_output_event_info_t &info)
  {
    if (destination == nullptr || capacity < eventSizeCapacity || info.struct_size < sizeof(info))
      return false;
    OutputEvent event;
    if (!output.pop(event)) return false;
    std::memcpy(destination, event.bytes.data(), event.size);
    info = event.info;
    return true;
  }
  void beginAudioBlock(uint32_t frameCount, uint32_t sampleRate, uint64_t startTimeNs)
  {
    previousAudioBlock = currentAudioBlock;
    currentAudioBlock = {++audioBlockSequence, startTimeNs, sampleRate, frameCount};
    publishAudioClock();
    audioBlockActive = true;
  }
  void endAudioBlock() { audioBlockActive = false; }
  void resetAudioClock()
  {
    previousAudioBlock = {};
    currentAudioBlock = {};
    publishAudioClock();
    audioBlockActive = false;
  }
  using IngressEvent = EventIngress<eventCapacity, eventSizeCapacity>::Event;
  using OutputEvent = OutputEventQueue<eventCapacity, eventSizeCapacity>::Event;
  uint32_t drainEventsForBlock(uint32_t frameCount, IngressEvent *destination, uint32_t capacity);
  void rejectEvent() { ingress.rejectDequeued(); }
  void getTelemetry(clap_wrapper_standalone_event_telemetry_t &telemetry) const
  {
    ingress.getTelemetry(telemetry);
  }
  void getOutputTelemetry(clap_wrapper_standalone_event_telemetry_t &telemetry) const
  {
    output.getTelemetry(telemetry);
  }

  const clap_wrapper_standalone_audio_settings_t &selectedAudioSettings() const { return audioSettings; }
  const std::vector<uint64_t> &selectedMidiPortIds() const { return selectedMidiPorts; }
  bool restoreSettings(const clap_wrapper_standalone_audio_settings_t &audio,
                       const std::vector<uint64_t> &midiPorts);

 private:
  bool hasInputDevice(uint64_t id) const;
  bool hasOutputDevice(uint64_t id) const;
  bool hasMidiPort(uint64_t id) const;
  bool validateAudioSettings(const clap_wrapper_standalone_audio_settings_t &settings) const;
  void mapTimestampToAudioBlock(IngressEvent &event, uint32_t frameCount) const;
  IngressTiming captureIngressTiming(uint64_t timestampNs, bool useTimestamp) const;
  AudioClockSnapshot readAudioClock() const;
  void publishAudioClock();

  std::thread::id mainThread;
  std::atomic_bool audioRunning{false};
  std::vector<clap_wrapper_standalone_audio_device_t> inputDevices;
  std::vector<clap_wrapper_standalone_audio_device_t> outputDevices;
  std::vector<clap_wrapper_standalone_midi_port_t> midiPorts;
  std::vector<uint64_t> selectedMidiPorts;
  bool midiSelectionConfigured{};
  clap_wrapper_standalone_audio_settings_t audioSettings{};
  EventIngress<eventCapacity, eventSizeCapacity> ingress;
  OutputEventQueue<eventCapacity, eventSizeCapacity> output;
  AudioBlockAnchor previousAudioBlock{};
  AudioBlockAnchor currentAudioBlock{};
  uint64_t audioBlockSequence{};
  bool audioBlockActive{};
  std::atomic_uint64_t audioClockRevision{};
  std::atomic_uint64_t publishedPreviousSequence{};
  std::atomic_uint64_t publishedPreviousStartTimeNs{};
  std::atomic_uint32_t publishedPreviousSampleRate{};
  std::atomic_uint32_t publishedPreviousFrameCount{};
  std::atomic_uint64_t publishedCurrentSequence{};
  std::atomic_uint64_t publishedCurrentStartTimeNs{};
  std::atomic_uint32_t publishedCurrentSampleRate{};
  std::atomic_uint32_t publishedCurrentFrameCount{};
};
} // namespace freeaudio::clap_wrapper::standalone::detail
