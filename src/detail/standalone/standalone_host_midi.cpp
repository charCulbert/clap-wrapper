#include "standalone_host.h"
#include "standalone_details.h"

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"  // other peoples errors are outside my scope
#endif

#include "RtMidi.h"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

namespace freeaudio::clap_wrapper::standalone
{
bool StandaloneHost::startMIDIThread()
{
  stopMIDIThread();
  refreshMidiServiceSnapshot();
  std::vector<uint64_t> boundPorts;
  bool allOpened{true};
  try
  {
    LOGINFO("Initializing Midi");
    auto midiIn = std::make_unique<RtMidiIn>();
    numMidiPorts = midiIn->getPortCount();
  }
  catch (RtMidiError &error)
  {
    error.printMessage();
    return false;
  }

  LOGDETAIL("MIDI: There are {} MIDI input sources available.", numMidiPorts);
  for (unsigned int i = 0; i < numMidiPorts; i++)
  {
    const auto portId = static_cast<uint64_t>(i) + 1;
    const auto &selected = services.selectedMidiPortIds();
    const bool openedByServices = std::find(selected.begin(), selected.end(), portId) != selected.end();
    const bool openedByWindowsUi = !currentMidiPorts.empty() &&
                                   std::find(currentMidiPorts.begin(), currentMidiPorts.end(), i) != currentMidiPorts.end();
    if (!openedByServices && !openedByWindowsUi) continue;
    if (testMidiPortOpen)
    {
      if (!testMidiPortOpen(i)) allOpened = false;
      else boundPorts.push_back(portId);
      continue;
    }
    try
    {
      auto midiIn = std::make_unique<RtMidiIn>();
      LOGDETAIL("  - '{}'", midiIn->getPortName(i));
      midiIn->openPort(i);
      midiIn->setCallback(midiCallback, this);
      midiIns.push_back(std::move(midiIn));
      boundPorts.push_back(portId);
    }
    catch (RtMidiError &error)
    {
      error.printMessage();
      allOpened = false;
    }
  }
  services.setBoundMidiPortIds(std::move(boundPorts));
  return allOpened;
}

void StandaloneHost::processMIDIEvents(double deltatime, std::vector<unsigned char> *message)
{
  auto nBytes = message->size();

  if (nBytes > 0 && nBytes <= 3)
  {
    clap_event_midi event{};
    event.header.size = sizeof(event);
    event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    event.header.type = CLAP_EVENT_MIDI;
    event.port_index = 0;
    std::memcpy(event.data, message->data(), nBytes);
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    services.enqueueEvent(&event.header, event.header.size,
                          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()));
  }
}

void StandaloneHost::midiCallback(double deltatime, std::vector<unsigned char> *message, void *userData)
{
  auto sh = (StandaloneHost *)userData;
  sh->processMIDIEvents(deltatime, message);
}

void StandaloneHost::stopMIDIThread()
{
  midiIns.clear();
}

bool StandaloneHost::rebuildMIDIEndpoints()
{
  stopMIDIThread();
  return startMIDIThread();
}

}  // namespace freeaudio::clap_wrapper::standalone
