#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Clap::Standalone::IOSMidiOut
{
using SampleTime = int64_t;
using HostTime = uint64_t;

// Keep this in sync with AUEventSampleTimeImmediate without importing AUAudioUnit here.
constexpr SampleTime immediateSampleTime = -4294967296LL;

struct RenderAnchor
{
  std::atomic<uint32_t> valid{0};
  std::atomic<SampleTime> sampleTime{0};
  std::atomic<HostTime> hostTime{0};
  std::atomic<uint64_t> hostTicksPerSampleQ32{0};
  std::atomic<uint32_t> frameCount{0};
};

static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "MIDI render state must use lock-free atomics");
static_assert(std::atomic<SampleTime>::is_always_lock_free,
              "MIDI render state must use lock-free atomics");
static_assert(std::atomic<HostTime>::is_always_lock_free,
              "MIDI render state must use lock-free atomics");

inline void updateRenderAnchor(RenderAnchor &anchor, double sampleTime, HostTime hostTime,
                               bool hasSampleTime, bool hasHostTime, uint32_t frameCount,
                               uint64_t hostTicksPerSampleQ32)
{
  anchor.valid.store(0, std::memory_order_release);
  if (hostTicksPerSampleQ32 == 0 || hostTime == 0 || !hasSampleTime || !hasHostTime ||
      !std::isfinite(sampleTime))
    return;

  anchor.sampleTime.store(static_cast<SampleTime>(sampleTime), std::memory_order_relaxed);
  anchor.hostTime.store(hostTime, std::memory_order_relaxed);
  anchor.hostTicksPerSampleQ32.store(hostTicksPerSampleQ32, std::memory_order_relaxed);
  anchor.frameCount.store(frameCount, std::memory_order_relaxed);
  anchor.valid.store(1, std::memory_order_release);
}

inline HostTime hostTimeForSample(SampleTime sampleTime, const RenderAnchor &anchor,
                                  HostTime fallbackHostTime)
{
  if (fallbackHostTime == 0) fallbackHostTime = 1;
  if (sampleTime == immediateSampleTime || anchor.valid.load(std::memory_order_acquire) == 0)
    return fallbackHostTime;

  const auto anchorSampleTime = anchor.sampleTime.load(std::memory_order_relaxed);
  if (sampleTime < anchorSampleTime) return fallbackHostTime;

  const auto frameOffset = static_cast<uint64_t>(sampleTime - anchorSampleTime);
  if (frameOffset > anchor.frameCount.load(std::memory_order_relaxed)) return fallbackHostTime;

  const auto ticksPerSample = anchor.hostTicksPerSampleQ32.load(std::memory_order_relaxed);
  if (ticksPerSample == 0 || frameOffset > std::numeric_limits<uint64_t>::max() / ticksPerSample)
    return fallbackHostTime;

  const auto offset = (frameOffset * ticksPerSample) >> 32;
  const auto hostTime = anchor.hostTime.load(std::memory_order_relaxed);
  if (offset > std::numeric_limits<HostTime>::max() - hostTime) return fallbackHostTime;
  return hostTime + offset;
}

inline HostTime hostTimeForDrain(HostTime scheduledHostTime, HostTime currentHostTime)
{
  return scheduledHostTime < currentHostTime ? currentHostTime : scheduledHostTime;
}

enum class TeardownAction { synchronouslyDrain, asynchronouslyFence };

constexpr TeardownAction teardownAction(bool isOnDrainQueue)
{
  return isOnDrainQueue ? TeardownAction::asynchronouslyFence : TeardownAction::synchronouslyDrain;
}
}  // namespace Clap::Standalone::IOSMidiOut
