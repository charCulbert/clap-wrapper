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
  static_assert(offsetof(clap_wrapper_host_standalone_services_t, dequeue_output_event) >
                    offsetof(clap_wrapper_host_standalone_services_t, get_event_telemetry),
                "output retrieval is appended to extension ABI");
  clap_wrapper_host_standalone_services_t extension{};
  extension.abi_version = CLAP_WRAPPER_STANDALONE_SERVICES_ABI_VERSION;
  extension.struct_size = offsetof(clap_wrapper_host_standalone_services_t,
                                   enqueue_timestamped_event);
  const bool legacyV1IsGuarded =
      !CLAP_WRAPPER_STANDALONE_SERVICES_HAS_MEMBER(&extension, enqueue_timestamped_event) &&
      !CLAP_WRAPPER_STANDALONE_SERVICES_HAS_MEMBER(&extension, dequeue_output_event) &&
      !CLAP_WRAPPER_STANDALONE_SERVICES_HAS_MEMBER(&extension, get_output_event_telemetry);
  extension.struct_size = sizeof(extension);
  const bool appendedV1IsVisible =
      CLAP_WRAPPER_STANDALONE_SERVICES_HAS_MEMBER(&extension, enqueue_timestamped_event) &&
      CLAP_WRAPPER_STANDALONE_SERVICES_HAS_MEMBER(&extension, dequeue_output_event) &&
      CLAP_WRAPPER_STANDALONE_SERVICES_HAS_MEMBER(&extension, get_output_event_telemetry);
  StandaloneServicesCore services;
  services.setAudioDevices({audioDevice(10)}, {audioDevice(20)});
  clap_wrapper_standalone_audio_snapshot_t truncated{};
  truncated.struct_size = offsetof(clap_wrapper_standalone_audio_snapshot_t, selected);
  const bool rejected = !services.getAudioSnapshot(truncated);

  clap_wrapper_standalone_audio_settings_t truncatedSettings{};
  truncatedSettings.struct_size = offsetof(clap_wrapper_standalone_audio_settings_t, flags);
  const bool settingsRejected = !services.applyAudioSettings(truncatedSettings);

  clap_wrapper_standalone_audio_snapshot_t negotiated{};
  negotiated.struct_size = sizeof(negotiated);
  const bool needsStorage = !services.getAudioSnapshot(negotiated) &&
                            negotiated.input_device_count == 1 && negotiated.output_device_count == 1;
  clap_wrapper_standalone_audio_device_t input{}, output{};
  negotiated.input_devices = &input;
  negotiated.input_device_capacity = 1;
  negotiated.output_devices = &output;
  negotiated.output_device_capacity = 1;
  return expect(legacyV1IsGuarded && appendedV1IsVisible,
                "appended v1 service pointers are guarded by struct_size") &&
         expect(rejected, "truncated snapshot ABI is rejected") &&
         expect(settingsRejected, "truncated audio settings ABI is rejected") &&
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
                    telemetry.accepted_events - telemetry.consumed_events == 247 &&
                    telemetry.rejected_events >= 1,
                "continuous producers cannot extend a callback, leave surplus queued, and reject stale events");
}

bool testTimestampedIngressAndOutput()
{
  StandaloneServicesCore services;
  constexpr uint64_t blockAStartNs = 1000000000ull;
  constexpr uint64_t blockBStartNs = blockAStartNs + 3000000ull;
  services.beginAudioBlock(128, 48000, blockAStartNs);

  const auto suppliedOffset = ramp(37, 211, 0.1);
  const auto timestamped = ramp(0, 17, 0.2);
  const auto tiedFirst = ramp(0, 19, 0.3);
  const auto tiedSecond = ramp(0, 23, 0.4);
  const auto late = ramp(99, 29, 0.5);
  const auto noClock = ramp(99, 31, 0.6);
  services.enqueueTimestampedEvent(&timestamped.header, sizeof(timestamped), blockAStartNs + 1000000);
  services.enqueueTimestampedEvent(&tiedFirst.header, sizeof(tiedFirst), blockAStartNs + 2000000);
  services.enqueueTimestampedEvent(&tiedSecond.header, sizeof(tiedSecond), blockAStartNs + 2000000);
  services.enqueueTimestampedEvent(&late.header, sizeof(late), blockAStartNs - 1);
  services.enqueueTimestampedEvent(&noClock.header, sizeof(noClock), 0);
  services.enqueueEvent(&suppliedOffset.header, sizeof(suppliedOffset), 1);
  services.endAudioBlock();
  services.beginAudioBlock(128, 48000, blockBStartNs);

  std::array<StandaloneServicesCore::IngressEvent, 6> input{};
  const auto delivered = services.drainEventsForBlock(128, input.data(), input.size());
  const auto *first = reinterpret_cast<const RampEvent *>(input[0].bytes.data());
  const auto *second = reinterpret_cast<const RampEvent *>(input[1].bytes.data());
  const auto *third = reinterpret_cast<const RampEvent *>(input[2].bytes.data());
  const auto *fourth = reinterpret_cast<const RampEvent *>(input[3].bytes.data());
  const auto *fifth = reinterpret_cast<const RampEvent *>(input[4].bytes.data());
  const auto *sixth = reinterpret_cast<const RampEvent *>(input[5].bytes.data());

  const auto output = ramp(91, 777, 0.9);
  const bool outputAccepted = services.enqueueOutputEvent(&output.header);
  std::array<unsigned char, CLAP_WRAPPER_STANDALONE_EVENT_SIZE_CAPACITY> outputBytes{};
  clap_wrapper_standalone_output_event_info_t truncatedInfo{};
  truncatedInfo.struct_size = offsetof(clap_wrapper_standalone_output_event_info_t,
                                       block_sequence);
  const bool truncatedInfoRejected =
      !services.dequeueOutputEvent(outputBytes.data(), outputBytes.size(), truncatedInfo);
  clap_wrapper_standalone_output_event_info_t outputInfo{};
  outputInfo.struct_size = sizeof(outputInfo);
  const bool outputDequeued = services.dequeueOutputEvent(outputBytes.data(), outputBytes.size(), outputInfo);
  const auto *outputEvent = reinterpret_cast<const RampEvent *>(outputBytes.data());
  for (uint32_t i = 0; i < StandaloneServicesCore::eventCapacity; ++i)
    services.enqueueOutputEvent(&output.header);
  const bool outputBackpressure = !services.enqueueOutputEvent(&output.header);
  clap_wrapper_standalone_event_telemetry_t outputTelemetry{};
  outputTelemetry.struct_size = sizeof(outputTelemetry);
  services.getOutputTelemetry(outputTelemetry);
  services.endAudioBlock();

  StandaloneServicesCore changedClock;
  constexpr uint64_t changedAStartNs = 2000000000ull;
  changedClock.beginAudioBlock(128, 48000, changedAStartNs);
  const auto changed = ramp(0, 41, 0.7);
  changedClock.enqueueTimestampedEvent(&changed.header, sizeof(changed), changedAStartNs + 1000000);
  changedClock.endAudioBlock();
  changedClock.beginAudioBlock(64, 96000, changedAStartNs + 3000000);
  StandaloneServicesCore::IngressEvent changedResult{};
  const auto changedCount = changedClock.drainEventsForBlock(64, &changedResult, 1);
  const auto *changedEvent = reinterpret_cast<const RampEvent *>(changedResult.bytes.data());
  const auto duringChangedB = ramp(0, 43, 0.8);
  changedClock.enqueueTimestampedEvent(&duringChangedB.header, sizeof(duringChangedB),
                                       changedAStartNs + 3500000);
  StandaloneServicesCore::IngressEvent prematureResult{};
  const auto prematureCount = changedClock.drainEventsForBlock(64, &prematureResult, 1);
  changedClock.endAudioBlock();
  changedClock.beginAudioBlock(64, 96000, changedAStartNs + 4000000);
  StandaloneServicesCore::IngressEvent nextResult{};
  const auto nextCount = changedClock.drainEventsForBlock(64, &nextResult, 1);
  const auto *nextEvent = reinterpret_cast<const RampEvent *>(nextResult.bytes.data());

  return expect(delivered == input.size() && first->header.time == 0 && first->durationFrames == 31 &&
                    second->header.time == 0 && second->durationFrames == 29 &&
                    third->header.time == 37 && third->durationFrames == 211 &&
                    fourth->header.time == 48 && fourth->durationFrames == 17 &&
                    fifth->header.time == 96 && fifth->durationFrames == 19 &&
                    sixth->header.time == 96 && sixth->durationFrames == 23,
                "block-A device events reach block B at phase; late/no-clock events use frame zero") &&
         expect(changedCount == 1 && changedEvent->header.time == 24 &&
                    changedEvent->durationFrames == 41 && prematureCount == 0 &&
                    nextCount == 1 && nextEvent->header.time == 48 &&
                    nextEvent->durationFrames == 43,
                "clock changes rescale phase and callback-boundary ingress waits one block") &&
         expect(outputAccepted && truncatedInfoRejected && outputDequeued &&
                    outputInfo.event_size == sizeof(output) &&
                    outputEvent->header.time == 91 && outputEvent->durationFrames == 777 &&
                    outputInfo.flags == CLAP_WRAPPER_STANDALONE_OUTPUT_EVENT_HAS_BLOCK_CONTEXT &&
                    outputInfo.block_sequence == 2 && outputInfo.block_start_time_ns == blockBStartNs &&
                    outputInfo.sample_rate == 48000 && outputInfo.frame_count == 128,
                "output event preserves timestamp with schedulable block identity") &&
         expect(outputBackpressure && outputTelemetry.accepted_events == 257 &&
                    outputTelemetry.consumed_events == 1 && outputTelemetry.dropped_events == 1,
                "output event telemetry exposes acceptance and backpressure");
}

bool testDelayedPublicationAcrossBlockBoundary()
{
  StandaloneServicesCore services;
  constexpr uint64_t blockAStartNs = 3000000000ull;
  constexpr uint64_t blockBStartNs = blockAStartNs + 3000000ull;
  constexpr uint64_t blockCStartNs = blockBStartNs + 3000000ull;
  services.beginAudioBlock(128, 48000, blockAStartNs);

  const auto delayed = ramp(0, 51, 0.1);
  std::atomic_bool reserved{false};
  std::atomic_bool allowPublish{false};
  bool delayedAccepted{};
  std::thread producer(
      [&]
      {
        delayedAccepted = services.enqueueTimestampedEventWithPublishHook(
            &delayed.header, sizeof(delayed), blockAStartNs + 1000000,
            [&]
            {
              reserved.store(true, std::memory_order_release);
              while (!allowPublish.load(std::memory_order_acquire)) std::this_thread::yield();
            });
      });
  while (!reserved.load(std::memory_order_acquire)) std::this_thread::yield();

  services.endAudioBlock();
  services.beginAudioBlock(128, 48000, blockBStartNs);
  const auto behindHead = ramp(0, 53, 0.2);
  const bool behindAccepted = services.enqueueTimestampedEvent(
      &behindHead.header, sizeof(behindHead), blockBStartNs + 1000000);
  std::array<StandaloneServicesCore::IngressEvent, 2> blockB{};
  const auto blockBCount = services.drainEventsForBlock(128, blockB.data(), blockB.size());

  allowPublish.store(true, std::memory_order_release);
  producer.join();
  services.endAudioBlock();
  services.beginAudioBlock(128, 48000, blockCStartNs);
  std::array<StandaloneServicesCore::IngressEvent, 2> blockC{};
  const auto blockCCount = services.drainEventsForBlock(128, blockC.data(), blockC.size());
  const auto *first = reinterpret_cast<const RampEvent *>(blockC[0].bytes.data());
  const auto *second = reinterpret_cast<const RampEvent *>(blockC[1].bytes.data());

  return expect(delayedAccepted && behindAccepted && blockBCount == 0,
                "stalled head reservation makes the bounded FIFO consumer return immediately") &&
         expect(blockCCount == 2 && first->header.time == 48 && first->durationFrames == 51 &&
                    blockC[0].timing.targetBlockSequence == 2 &&
                    second->header.time == 48 && second->durationFrames == 53 &&
                    blockC[1].timing.targetBlockSequence == 3,
                "first later eligible block preserves phase and original target-block identity");
}

bool testConcurrentOutputQueue()
{
  OutputEventQueue<64, 64> output;
  constexpr uint32_t eventCount = 1024;
  std::atomic_bool start{false};
  std::thread producer(
      [&]
      {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (uint32_t i = 0; i < eventCount; ++i)
        {
          const auto event = ramp(i, i + 1, i);
          clap_wrapper_standalone_output_event_info_t info{};
          info.struct_size = sizeof(info);
          info.event_size = sizeof(event);
          while (!output.push(&event.header, info)) std::this_thread::yield();
        }
      });

  start.store(true, std::memory_order_release);
  bool ordered{true};
  for (uint32_t i = 0; i < eventCount; ++i)
  {
    OutputEventQueue<64, 64>::Event event;
    while (!output.pop(event)) std::this_thread::yield();
    const auto *rampEvent = reinterpret_cast<const RampEvent *>(event.bytes.data());
    ordered &= rampEvent->header.time == i && rampEvent->durationFrames == i + 1;
  }
  producer.join();

  clap_wrapper_standalone_event_telemetry_t telemetry{};
  telemetry.struct_size = sizeof(telemetry);
  output.getTelemetry(telemetry);
  return expect(ordered && telemetry.accepted_events == eventCount &&
                    telemetry.consumed_events == eventCount && telemetry.rejected_events == 0,
                "bounded SPSC output push preserves order under concurrent consumption");
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

bool testMonoInputRoute()
{
  StandaloneServicesCore services;
  auto input = audioDevice(10);
  input.input_channels = 1;
  services.setAudioDevices({input}, {audioDevice(20)});

  clap_wrapper_standalone_audio_settings_t settings{};
  settings.struct_size = sizeof(settings);
  settings.input_device_id = input.id;
  settings.input_channels = 1;
  settings.flags = CLAP_WRAPPER_STANDALONE_AUDIO_INPUT_ENABLED;
  return expect(services.applyAudioSettings(settings) &&
                    services.selectedAudioSettings().input_channels == 1,
                "mono input routes are valid service settings");
}

bool testMidiEndpointBindingSeam()
{
  StandaloneServicesCore services;
  services.setMidiPorts({midiPort(1), midiPort(2)});
  services.setMidiPortOpen(2, false);
  const bool failedOpen = services.setBoundMidiPortIds({1});
  uint64_t selected[2]{};
  clap_wrapper_standalone_midi_port_t ports[2]{};
  clap_wrapper_standalone_midi_snapshot_t snapshot{};
  snapshot.struct_size = sizeof(snapshot);
  snapshot.ports = ports;
  snapshot.port_capacity = 2;
  snapshot.selected_port_ids = selected;
  snapshot.selected_port_capacity = 2;
  const bool failureVisible = failedOpen && services.getMidiSnapshot(snapshot) &&
                              snapshot.selected_port_count == 1 && selected[0] == 1;
  services.setMidiPortOpen(2, true);
  const bool successVisible = services.setBoundMidiPortIds({1, 2}) && services.getMidiSnapshot(snapshot) &&
                              snapshot.selected_port_count == 2;
  services.setMidiPortOpen(1, false);
  const bool closeVisible = services.setBoundMidiPortIds({2}) && services.getMidiSnapshot(snapshot) &&
                            snapshot.selected_port_count == 1 && selected[0] == 2;
  return expect(failureVisible && successVisible && closeVisible,
                "endpoint binding seam keeps snapshots coherent across open failure, success and close");
}

bool testMidiRollbackState()
{
  StandaloneServicesCore services;
  services.setAudioDevices({}, {audioDevice(20)});
  services.setMidiPorts({midiPort(1), midiPort(2)});

  clap_wrapper_standalone_audio_settings_t audio{};
  audio.struct_size = sizeof(audio);
  audio.output_device_id = 20;
  audio.output_channels = 2;
  audio.flags = CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT_ENABLED;
  if (!services.applyAudioSettings(audio) || !services.setMidiPortOpen(2, false)) return false;

  const auto previousAudio = services.selectedAudioSettings();
  const auto previousMidi = services.selectedMidiPortIds();
  const auto selectedNewPort = services.setMidiPortOpen(2, true);
  const auto endpointFailure = services.setBoundMidiPortIds({1});
  const auto restored = services.restoreSettings(previousAudio, previousMidi);
  return expect(selectedNewPort && endpointFailure && restored &&
                    services.selectedMidiPortIds() == previousMidi,
                "failed MIDI endpoint change restores prior selection");
}
} // namespace

int main()
{
  return testAbiAndNegotiation() && testIngressOrderCapacityAndRamp() && testBoundedSortedDelivery() &&
                 testTimestampedIngressAndOutput() && testDelayedPublicationAcrossBlockBoundary() &&
                 testConcurrentOutputQueue() && testEnvelopeAndRollback() &&
                 testConcurrentIngress() && testThreadAndLifecycleGating()
                 && testMonoInputRoute() && testMidiEndpointBindingSeam() && testMidiRollbackState()
             ? 0
             : 1;
}
