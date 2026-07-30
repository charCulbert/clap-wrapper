#include "detail/auv2/auv2_base_classes.h"

#include <cstdio>

namespace
{
bool expect(bool condition, const char *message)
{
  if (condition) return true;

  std::fprintf(stderr, "FAIL: %s\n", message);
  return false;
}

bool testNoteOutputTiming()
{
  clap_note_port_info port{};
  port.id = 42;
  free_audio::auv2_wrapper::MIDIOutput output(3, port);

  clap_event_note_t event{};
  event.header.type = CLAP_EVENT_NOTE_ON;
  event.header.size = sizeof(event);
  event.header.time = 37;
  event.channel = 2;
  event.key = 64;
  event.velocity = 0.75;

  if (!expect(output.addNoteOn(event.channel, event.key, event.velocity * 127.f, event.header.time),
              "CLAP note output is accepted"))
    return false;

  const auto *packet = &output.getMIDIPacketList()->packet[0];
  return expect(packet->timeStamp == event.header.time,
                "AUv2 note packet keeps CLAP sample offset") &&
         expect(packet->length == 3, "AUv2 note packet has three MIDI bytes") &&
         expect(packet->data[0] == 0x92, "AUv2 note packet keeps channel");
}

bool testMIDIOutputTiming()
{
  clap_note_port_info port{};
  port.id = 42;
  free_audio::auv2_wrapper::MIDIOutput output(3, port);

  clap_event_midi_t event{};
  event.header.type = CLAP_EVENT_MIDI;
  event.header.size = sizeof(event);
  event.header.time = 91;
  event.port_index = port.id;
  event.data[0] = 0xb5;
  event.data[1] = 74;
  event.data[2] = 101;

  if (!expect(output.addMIDI3Byte(event.data, event.header.time), "CLAP MIDI output is accepted"))
    return false;

  const auto *packet = &output.getMIDIPacketList()->packet[0];
  return expect(packet->timeStamp == event.header.time,
                "AUv2 MIDI packet keeps CLAP sample offset") &&
         expect(packet->length == 3, "AUv2 MIDI packet has three MIDI bytes") &&
         expect(packet->data[0] == event.data[0], "AUv2 MIDI packet keeps status byte");
}

bool testNoteOffOutputTiming()
{
  clap_note_port_info port{};
  free_audio::auv2_wrapper::MIDIOutput output(3, port);

  if (!expect(output.addNoteOff(11, 36, 64, 73), "CLAP note-off output is accepted")) return false;

  const auto *packet = &output.getMIDIPacketList()->packet[0];
  return expect(packet->timeStamp == 73, "AUv2 note-off keeps CLAP sample offset") &&
         expect(packet->length == 3, "AUv2 note-off has three MIDI bytes") &&
         expect(packet->data[0] == 0x8b, "AUv2 note-off keeps channel");
}

bool testTwoByteMIDIOutputTiming()
{
  clap_note_port_info port{};
  free_audio::auv2_wrapper::MIDIOutput output(3, port);

  const uint8_t programChange[] = {0xc4, 19, 0};
  const uint8_t channelPressure[] = {0xd6, 87, 0};
  if (!expect(output.addMIDI3Byte(programChange, 23), "program change output is accepted") ||
      !expect(output.addMIDI3Byte(channelPressure, 51), "channel pressure output is accepted"))
    return false;

  const auto *first = &output.getMIDIPacketList()->packet[0];
  const auto *second = MIDIPacketNext(first);
  return expect(first->timeStamp == 23 && first->length == 2 && first->data[0] == 0xc4 &&
                    first->data[1] == 19,
                "AUv2 program change preserves timestamp and two-byte length") &&
         expect(second->timeStamp == 51 && second->length == 2 && second->data[0] == 0xd6 &&
                    second->data[1] == 87,
                "AUv2 channel pressure preserves timestamp and two-byte length");
}
}  // namespace

int main()
{
  return testNoteOutputTiming() && testMIDIOutputTiming() && testNoteOffOutputTiming() &&
                 testTwoByteMIDIOutputTiming()
             ? 0
             : 1;
}
