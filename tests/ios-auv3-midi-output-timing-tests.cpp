#include "../src/detail/standalone/ios/auv3/ios_midi_out_timing.h"

#include <cassert>
#include <cstdint>
#include <limits>

namespace
{
using namespace Clap::Standalone::IOSMidiOut;

constexpr uint64_t ticksPerSample(uint32_t ticks)
{
  return static_cast<uint64_t>(ticks) << 32;
}

void testHostTimeAnchorConversion()
{
  RenderAnchor anchor;
  updateRenderAnchor(anchor, 48000.0, 100000, true, true, 512, ticksPerSample(100));

  assert(hostTimeForSample(48037, anchor, 9) == 103700);
  assert(hostTimeForSample(48512, anchor, 9) == 151200);
}

void testHostTimeFallbackAndClamping()
{
  RenderAnchor anchor;
  updateRenderAnchor(anchor, 48000.0, 100000, true, true, 64, ticksPerSample(100));

  assert(hostTimeForSample(immediateSampleTime, anchor, 0) == 1);
  assert(hostTimeForSample(47999, anchor, 7) == 7);
  assert(hostTimeForSample(48065, anchor, 7) == 7);

  updateRenderAnchor(anchor, 48000.0, 100000, false, true, 64, ticksPerSample(100));
  assert(hostTimeForSample(48001, anchor, 7) == 7);

  updateRenderAnchor(anchor, 0.0, std::numeric_limits<uint64_t>::max() - 2, true, true, 64,
                     ticksPerSample(100));
  assert(hostTimeForSample(1, anchor, 7) == 7);

  assert(hostTimeForDrain(100, 200) == 200);
  assert(hostTimeForDrain(300, 200) == 300);
}

void testReentrantTeardownFencesInsteadOfSynchronouslyDraining()
{
  assert(teardownAction(false) == TeardownAction::synchronouslyDrain);
  assert(teardownAction(true) == TeardownAction::asynchronouslyFence);
}
}  // namespace

int main()
{
  testHostTimeAnchorConversion();
  testHostTimeFallbackAndClamping();
  testReentrantTeardownFencesInsteadOfSynchronouslyDraining();
}
