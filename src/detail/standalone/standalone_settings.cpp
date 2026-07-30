#include "standalone_settings.h"

#include <array>
#include <cstring>
#include <limits>

namespace freeaudio::clap_wrapper::standalone::detail
{
namespace
{
constexpr std::array<uint8_t, 4> magic{{'C', 'W', 'S', 'V'}};
constexpr uint32_t version = 1;
constexpr size_t headerSize = 24;

uint32_t checksum(const uint8_t *data, size_t size)
{
  uint32_t value = 2166136261u;
  for (size_t i = 0; i < size; ++i)
    value = (value ^ data[i]) * 16777619u;
  return value;
}

void appendU32(std::vector<uint8_t> &output, uint32_t value)
{
  for (auto shift = 0u; shift < 32; shift += 8) output.push_back(static_cast<uint8_t>(value >> shift));
}

void appendU64(std::vector<uint8_t> &output, uint64_t value)
{
  for (auto shift = 0u; shift < 64; shift += 8) output.push_back(static_cast<uint8_t>(value >> shift));
}

bool readU32(const std::vector<uint8_t> &bytes, size_t &offset, uint32_t &value)
{
  if (offset > bytes.size() || bytes.size() - offset < 4) return false;
  value = 0;
  for (auto shift = 0u; shift < 32; shift += 8) value |= uint32_t{bytes[offset++]} << shift;
  return true;
}

bool readU64(const std::vector<uint8_t> &bytes, size_t &offset, uint64_t &value)
{
  if (offset > bytes.size() || bytes.size() - offset < 8) return false;
  value = 0;
  for (auto shift = 0u; shift < 64; shift += 8) value |= uint64_t{bytes[offset++]} << shift;
  return true;
}
} // namespace

bool isSettingsEnvelope(const std::vector<uint8_t> &bytes)
{
  if (bytes.size() < headerSize || !std::equal(magic.begin(), magic.end(), bytes.begin())) return false;
  size_t offset = magic.size();
  uint32_t fileVersion{}, audioSize{}, midiSize{}, pluginSize{}, ignoredChecksum{};
  if (!readU32(bytes, offset, fileVersion) || !readU32(bytes, offset, audioSize) ||
      !readU32(bytes, offset, midiSize) || !readU32(bytes, offset, pluginSize) ||
      !readU32(bytes, offset, ignoredChecksum))
    return false;
  const auto payloadSize = static_cast<uint64_t>(audioSize) + midiSize + pluginSize;
  return fileVersion == version && audioSize == sizeof(clap_wrapper_standalone_audio_settings_t) &&
         midiSize % sizeof(uint64_t) == 0 && payloadSize == bytes.size() - headerSize;
}

std::vector<uint8_t> writeSettingsEnvelope(const StandaloneSettingsEnvelope &envelope)
{
  const auto midiSize = envelope.midiPortIds.size() * sizeof(uint64_t);
  const auto audioSize = sizeof(clap_wrapper_standalone_audio_settings_t);
  if (midiSize > std::numeric_limits<uint32_t>::max() ||
      envelope.pluginState.size() > std::numeric_limits<uint32_t>::max())
    return {};

  std::vector<uint8_t> payload;
  payload.reserve(audioSize + midiSize + envelope.pluginState.size());
  const auto *audio = reinterpret_cast<const uint8_t *>(&envelope.audio);
  payload.insert(payload.end(), audio, audio + audioSize);
  for (const auto id : envelope.midiPortIds) appendU64(payload, id);
  payload.insert(payload.end(), envelope.pluginState.begin(), envelope.pluginState.end());

  std::vector<uint8_t> result;
  result.reserve(headerSize + payload.size());
  result.insert(result.end(), magic.begin(), magic.end());
  appendU32(result, version);
  appendU32(result, static_cast<uint32_t>(audioSize));
  appendU32(result, static_cast<uint32_t>(midiSize));
  appendU32(result, static_cast<uint32_t>(envelope.pluginState.size()));
  appendU32(result, checksum(payload.data(), payload.size()));
  result.insert(result.end(), payload.begin(), payload.end());
  return result;
}

std::optional<StandaloneSettingsEnvelope> readSettingsEnvelope(const std::vector<uint8_t> &bytes)
{
  if (!isSettingsEnvelope(bytes) || bytes.size() < headerSize) return std::nullopt;
  size_t offset = magic.size();
  uint32_t fileVersion{}, audioSize{}, midiSize{}, pluginSize{}, expectedChecksum{};
  if (!readU32(bytes, offset, fileVersion) || !readU32(bytes, offset, audioSize) ||
      !readU32(bytes, offset, midiSize) || !readU32(bytes, offset, pluginSize) ||
      !readU32(bytes, offset, expectedChecksum) || fileVersion != version ||
      audioSize != sizeof(clap_wrapper_standalone_audio_settings_t) || midiSize % sizeof(uint64_t) != 0)
    return std::nullopt;

  const auto payloadSize = static_cast<uint64_t>(audioSize) + midiSize + pluginSize;
  if (payloadSize > bytes.size() - headerSize || payloadSize != bytes.size() - headerSize ||
      checksum(bytes.data() + headerSize, static_cast<size_t>(payloadSize)) != expectedChecksum)
    return std::nullopt;

  StandaloneSettingsEnvelope result;
  std::memcpy(&result.audio, bytes.data() + offset, audioSize);
  if (result.audio.struct_size < sizeof(result.audio)) return std::nullopt;
  offset += audioSize;
  result.midiPortIds.resize(midiSize / sizeof(uint64_t));
  for (auto &id : result.midiPortIds)
    if (!readU64(bytes, offset, id)) return std::nullopt;
  result.pluginState.assign(bytes.begin() + static_cast<ptrdiff_t>(offset), bytes.end());
  return result;
}
} // namespace freeaudio::clap_wrapper::standalone::detail
