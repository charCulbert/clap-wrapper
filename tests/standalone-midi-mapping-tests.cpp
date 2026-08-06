#include "detail/standalone/standalone_midi_mapping.h"

#include <clap/ext/params.h>

#include <array>
#include <cmath>
#include <iostream>

namespace
{
using freeaudio::clap_wrapper::standalone::detail::StandaloneMidiMappingTable;

bool expect(bool condition, const char *message)
{
  if (!condition) std::cerr << "FAILED: " << message << '\n';
  return condition;
}

bool near(double left, double right)
{
  return std::abs(left - right) < 1.0e-12;
}
} // namespace

int main()
{
  StandaloneMidiMappingTable mappings;
  const std::array<uint8_t, 3> omniCC{{0xb0, 10, 64}};
  const std::array<uint8_t, 3> channelCC{{0xb1, 10, 127}};
  const std::array<uint8_t, 3> note{{0x90, 10, 64}};
  StandaloneMidiMappingTable::Value value;

  bool passed = true;
  passed &= expect(mappings.setMapping(-1, 10, 3, -1.0, 1.0, 0, 2),
                   "omni mapping accepts a bounded parameter range");
  passed &= expect(!mappings.valueFor(omniCC.data(), omniCC.size(), 1, value),
                   "mapping stays inactive before its effective block");
  passed &= expect(mappings.valueFor(omniCC.data(), omniCC.size(), 2, value) &&
                       value.paramId == 3 && near(value.value, 1.0 / 127.0),
                   "CC value is linearly mapped after its effective block");
  passed &= expect(mappings.valueFor(channelCC.data(), channelCC.size(), 2, value) &&
                       value.paramId == 3 && near(value.value, 1.0),
                   "omni mapping accepts every MIDI channel");

  passed &= expect(mappings.setMapping(1, 10, 7, 0.0, 10.0, 0, 3),
                   "channel mapping accepts a zero-based channel");
  passed &= expect(mappings.valueFor(channelCC.data(), channelCC.size(), 3, value) &&
                       value.paramId == 7 && near(value.value, 10.0),
                   "channel-specific mapping wins over omni mapping");
  passed &= expect(mappings.clearMapping(1, 10) &&
                       mappings.valueFor(channelCC.data(), channelCC.size(), 3, value) &&
                       value.paramId == 3,
                   "clearing a channel mapping falls back to omni mapping");

  passed &= expect(mappings.setMapping(-1, 11, 9, 0.0, 7.0, CLAP_PARAM_IS_STEPPED, 1),
                   "stepped mapping accepts CLAP parameter flags");
  const std::array<uint8_t, 3> steppedCC{{0xbf, 11, 64}};
  passed &= expect(mappings.valueFor(steppedCC.data(), steppedCC.size(), 1, value) &&
                       value.paramId == 9 && near(value.value, 4.0),
                   "stepped mapping rounds to the parameter step");
  passed &= expect(!mappings.valueFor(note.data(), note.size(), 99, value),
                   "notes never become mapped parameter events");

  return passed ? 0 : 1;
}
