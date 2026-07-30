#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <clapwrapper/standalone-services.h>

namespace freeaudio::clap_wrapper::standalone::detail
{
struct StandaloneSettingsEnvelope
{
  clap_wrapper_standalone_audio_settings_t audio{};
  std::vector<uint64_t> midiPortIds;
  std::vector<uint8_t> pluginState;
};

std::vector<uint8_t> writeSettingsEnvelope(const StandaloneSettingsEnvelope &envelope);
std::optional<StandaloneSettingsEnvelope> readSettingsEnvelope(const std::vector<uint8_t> &bytes);
bool isSettingsEnvelope(const std::vector<uint8_t> &bytes);
} // namespace freeaudio::clap_wrapper::standalone::detail
