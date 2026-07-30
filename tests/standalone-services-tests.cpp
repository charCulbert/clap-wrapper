#include "detail/standalone/standalone_services_core.h"
#include "detail/standalone/standalone_settings.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <thread>

namespace
{
using namespace freeaudio::clap_wrapper::standalone::detail;

bool expect(bool condition, const char *message)
{
  if (!condition) std::cerr << "FAILED: " << message << '\n';
  return condition;
}

clap_wrapper_standalone_audio_device_t audioDevice(uint64_t id)
{
  clap_wrapper_standalone_audio_device_t result{};
  result.struct_size = sizeof(result);
  result.id = id;
  result.input_channels = 2;
  result.output_channels = 2;
  return result;
}

clap_wrapper_standalone_midi_port_t midiPort(uint64_t id)
{
  clap_wrapper_standalone_midi_port_t result{};
  result.struct_size = sizeof(result);
  result.id = id;
  return result;
}

struct RampEvent
{
  clap_event_header_t header{};
  uint32_t durationFrames{};
  double target{};
};

RampEvent ramp(uint32_t time, uint32_t duration, double target)
{
  RampEvent result{};
  result.header.size = sizeof(result);
  result.header.time = time;
  result.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
  result.header.type = CLAP_EVENT_PARAM_VALUE;
  result.durationFrames = duration;
  result.target = target;
  return result;
}

bool testAbiAndNegotiation()
{
  static_assert(offsetof(clap_wrapper_host_standalone_services_t, enqueue_event) >
                    offsetof(clap_wrapper_host_standalone_services_t, get_midi_snapshot),
                "extension ABI must remain append-only");
  StandaloneServicesCore services;
  services.setAudioDevices({audioDevice(10)}, {audioDevice(20)});
  clap_wrapper_standalone_audio_snapshot_t truncated{};
  truncated.struct_size = offsetof(clap_wrapper_standalone_audio_snapshot_t, selected);
  const bool rejected = !services.getAudioSnapshot(truncated);

  clap_wrapper_standalone_audio_snapshot_t negotiated{};
  negotiated.struct_size = sizeof(negotiated);
  const bool needsStorage = !services.getAudioSnapshot(negotiated) &&
                            negotiated.input_device_count == 1 && negotiated.output_device_count == 1;
  clap_wrapper_standalone_audio_device_t input{}, output{};
  negotiated.input_devices = &input;
  negotiated.input_device_capacity = 1;
  negotiated.output_devices = &output;
  negotiated.output_device_capacity = 1;
  return expect(rejected, "truncated snapshot ABI is rejected") &&
         expect(needsStorage, "snapshot reports required storage without partial output") &&
         expect(services.getAudioSnapshot(negotiated) && input.id == 10 && output.id == 20,
                "snapshot succeeds after size negotiation");
}

bool testIngressOrderCapacityAndRamp()
{
  EventIngress<4, 64> ingress;
  const auto first = ramp(7, 19, 0.2);
  const auto second = ramp(9, 23, 0.7);
  const bool pushed = ingress.push(&first.header, sizeof(first), 100) &&
                      ingress.push(&second.header, sizeof(second), 200);
  EventIngress<4, 64>::Event result{};
  const bool firstOk = ingress.pop(result) && result.timestampNs == 100 && result.size == sizeof(first) &&
                       std::memcmp(result.bytes.data(), &first, sizeof(first)) == 0;
  const bool secondOk = ingress.pop(result) && result.timestampNs == 200 &&
                        std::memcmp(result.bytes.data(), &second, sizeof(second)) == 0;

  for (uint32_t i = 0; i < 4; ++i) ingress.push(&first.header, sizeof(first), i);
  const bool fullIsDropped = !ingress.push(&first.header, sizeof(first), 99);
  clap_wrapper_standalone_event_telemetry_t telemetry{};
  telemetry.struct_size = sizeof(telemetry);
  ingress.getTelemetry(telemetry);
  return expect(pushed && firstOk && secondOk, "ingress preserves event bytes, order, timestamps and duration") &&
         expect(fullIsDropped && telemetry.dropped_events == 1 && telemetry.event_capacity == 4,
                "ingress reports bounded capacity drops");
}

bool testBoundedSortedDelivery()
{
  StandaloneServicesCore services;
  const auto late = ramp(5, 31, 0.5);
  const auto earlySecond = ramp(2, 17, 0.2);
  const auto earlyFirst = ramp(2, 13, 0.1);
  const auto outOfBlock = ramp(64, 99, 0.9);
  services.enqueueEvent(&late.header, sizeof(late), 30);
  services.enqueueEvent(&earlySecond.header, sizeof(earlySecond), 20);
  services.enqueueEvent(&earlyFirst.header, sizeof(earlyFirst), 10);
  services.enqueueEvent(&outOfBlock.header, sizeof(outOfBlock), 40);

  std::array<StandaloneServicesCore::IngressEvent, 3> staging{};
  const auto delivered = services.drainEventsForBlock(64, staging.data(), staging.size());
  const auto *first = reinterpret_cast<const RampEvent *>(staging[0].bytes.data());
  const auto *second = reinterpret_cast<const RampEvent *>(staging[1].bytes.data());
  const auto *third = reinterpret_cast<const RampEvent *>(staging[2].bytes.data());

  for (uint32_t i = 0; i < StandaloneServicesCore::eventCapacity; ++i)
    services.enqueueEvent(&late.header, sizeof(late), i + 100);
  std::array<StandaloneServicesCore::IngressEvent, 8> bounded{};
  const auto boundedCount = services.drainEventsForBlock(64, bounded.data(), bounded.size());
  clap_wrapper_standalone_event_telemetry_t telemetry{};
  telemetry.struct_size = sizeof(telemetry);
  services.getTelemetry(telemetry);
  return expect(delivered == 3 && first->durationFrames == 13 && second->durationFrames == 17 &&
                    third->durationFrames == 31,
                "delivery sorts by sample time then ingestion timestamp and preserves ramp duration") &&
         expect(boundedCount == bounded.size() && telemetry.consumed_events == 12 &&
                    telemetry.rejected_events >= 1,
                "continuous producers cannot extend a callback and out-of-block events are rejected");
}

bool testEnvelopeAndRollback()
{
  StandaloneSettingsEnvelope source;
  source.audio.struct_size = sizeof(source.audio);
  source.audio.input_device_id = 10;
  source.audio.output_device_id = 20;
  source.audio.sample_rate = 48000;
  source.audio.input_channels = 2;
  source.audio.output_channels = 2;
  source.audio.flags = CLAP_WRAPPER_STANDALONE_AUDIO_INPUT_ENABLED |
                       CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT_ENABLED;
  source.midiPortIds = {1};
  source.pluginState = {1, 2, 3, 4};
  const auto bytes = writeSettingsEnvelope(source);
  const auto decoded = readSettingsEnvelope(bytes);
  auto corrupted = bytes;
  corrupted.back() ^= 0x7f;
  std::vector<uint8_t> legacyWithMagic(24, 0);
  std::memcpy(legacyWithMagic.data(), "CWSV", 4);

  StandaloneServicesCore services;
  services.setAudioDevices({audioDevice(10)}, {audioDevice(20)});
  services.setMidiPorts({midiPort(1)});
  const auto baseline = services.selectedAudioSettings();
  auto invalid = source.audio;
  invalid.output_device_id = 99;
  const bool rejected = !services.restoreSettings(invalid, {1});
  return expect(decoded && decoded->pluginState == source.pluginState && decoded->midiPortIds == source.midiPortIds,
                "settings envelope round trips length-delimited state") &&
         expect(!readSettingsEnvelope(corrupted), "corrupt envelope is rejected before applying settings") &&
         expect(!isSettingsEnvelope(legacyWithMagic), "raw state beginning with CWSV remains identifiable as legacy") &&
         expect(rejected && services.selectedAudioSettings().output_device_id == baseline.output_device_id,
                "invalid settings restore leaves prior host state unchanged");
}

bool testConcurrentIngress()
{
  EventIngress<256, 64> ingress;
  constexpr uint32_t eventsPerProducer = 96;
  std::atomic_bool start{false};
  std::atomic_uint32_t finished{0};
  auto producer = [&](uint32_t base)
  {
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    for (uint32_t i = 0; i < eventsPerProducer; ++i)
    {
      const auto event = ramp(base + i, i + 1, base + i);
      if (!ingress.push(&event.header, sizeof(event), base + i)) return;
    }
    finished.fetch_add(1, std::memory_order_release);
  };
  std::thread first(producer, 0);
  std::thread second(producer, eventsPerProducer);
  start.store(true, std::memory_order_release);

  std::array<bool, eventsPerProducer * 2> seen{};
  uint32_t received{};
  EventIngress<256, 64>::Event result{};
  while (finished.load(std::memory_order_acquire) != 2 || received != seen.size())
  {
    if (!ingress.pop(result))
    {
      std::this_thread::yield();
      continue;
    }
    const auto *event = reinterpret_cast<const RampEvent *>(result.bytes.data());
    if (event->header.time < seen.size() && !seen[event->header.time])
    {
      seen[event->header.time] = true;
      ++received;
    }
  }
  first.join();
  second.join();
  clap_wrapper_standalone_event_telemetry_t telemetry{};
  telemetry.struct_size = sizeof(telemetry);
  ingress.getTelemetry(telemetry);
  return expect(received == seen.size() && telemetry.dropped_events == 0,
                "multi-producer ingress remains lossless below capacity");
}

bool testThreadAndLifecycleGating()
{
  StandaloneServicesCore services;
  services.setAudioDevices({audioDevice(10)}, {audioDevice(20)});
  services.setMidiPorts({midiPort(1)});
  clap_wrapper_standalone_audio_settings_t settings{};
  settings.struct_size = sizeof(settings);
  settings.input_device_id = 10;
  settings.output_device_id = 20;
  settings.input_channels = 2;
  settings.output_channels = 2;
  settings.flags = CLAP_WRAPPER_STANDALONE_AUDIO_INPUT_ENABLED |
                   CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT_ENABLED;
  std::atomic_bool workerRejected{false};
  std::thread worker([&] { workerRejected = !services.applyAudioSettings(settings); });
  worker.join();
  clap_wrapper_standalone_audio_settings_t disabled{};
  disabled.struct_size = sizeof(disabled);
  const bool routesCleared = services.applyAudioSettings(disabled) &&
                             services.selectedAudioSettings().flags == 0;
  const bool midiClosed = services.setMidiPortOpen(1, false);
  clap_wrapper_standalone_midi_snapshot_t midiSnapshot{};
  midiSnapshot.struct_size = sizeof(midiSnapshot);
  const bool selectionUpdated = midiClosed && !services.getMidiSnapshot(midiSnapshot) &&
                                midiSnapshot.selected_port_count == 0;
  services.setMidiPortOpen(1, true);
  services.setAudioRunning(true);
  const bool activeRejected = !services.applyAudioSettings(settings) && !services.setMidiPortOpen(1, false);
  return expect(workerRejected, "main-thread services reject worker calls") &&
         expect(routesCleared && selectionUpdated, "disabled routes and MIDI selection are reflected in snapshots") &&
         expect(activeRejected, "device and MIDI lifecycle changes are gated while audio runs");
}
} // namespace

int main()
{
  return testAbiAndNegotiation() && testIngressOrderCapacityAndRamp() && testBoundedSortedDelivery() &&
                 testEnvelopeAndRollback() && testConcurrentIngress() && testThreadAndLifecycleGating()
             ? 0
             : 1;
}
