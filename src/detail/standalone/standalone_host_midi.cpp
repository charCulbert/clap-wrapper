#include "standalone_host.h"

namespace freeaudio::clap_wrapper::standalone
{
bool StandaloneHost::startMIDIThread()
{
  refreshMidiServiceSnapshot();
  return true;
}

void StandaloneHost::stopMIDIThread()
{
}

bool StandaloneHost::rebuildMIDIEndpoints()
{
  midiSelectionConfigured = true;
  return true;
}
} // namespace freeaudio::clap_wrapper::standalone
