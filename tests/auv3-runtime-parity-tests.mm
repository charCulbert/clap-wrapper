#include <chardio/clap/chardio_CLAPAUv3Metadata.h>

#include "../src/detail/auv3/auv3_audiounit.h"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>

namespace
{
enum class MetadataMode { present, absent, truncated };

struct TestState
{
  MetadataMode mode = MetadataMode::present;
  bool prepareResult = true;
  bool loadedPreset = false;
  int32_t loadedNumber = 0;
  bool supportsMIDI2 = false;
  chardio_auv3_mpe_policy_t mpePolicy = CHARDIO_AUV3_MPE_POLICY_FROM_NOTE_PORTS;
};

constexpr uint32_t fieldEnd(size_t offset, size_t fieldSize)
{
  return static_cast<uint32_t>(offset + fieldSize);
}

bool CLAP_ABI prepare(const clap_plugin_t *plugin)
{
  return static_cast<TestState *>(plugin->plugin_data)->prepareResult;
}

uint32_t CLAP_ABI presetCount(const clap_plugin_t *)
{
  return 2;
}

bool CLAP_ABI getPreset(const clap_plugin_t *, uint32_t index, chardio_auv3_factory_preset_t *preset,
                        uint32_t capacity)
{
  if (!preset || capacity < sizeof(*preset) || index > 1) return false;
  preset->struct_size = sizeof(*preset);
  preset->number = static_cast<int32_t>(index + 3);
  preset->name = index == 0 ? "First" : "Second";
  return true;
}

bool CLAP_ABI loadPreset(const clap_plugin_t *plugin, int32_t number)
{
  auto &state = *static_cast<TestState *>(plugin->plugin_data);
  state.loadedPreset = true;
  state.loadedNumber = number;
  return number == 3;
}

bool CLAP_ABI getView(const clap_plugin_t *, chardio_auv3_view_configuration_t *view,
                      uint32_t capacity)
{
  if (!view || capacity < sizeof(*view)) return false;
  view->struct_size = sizeof(*view);
  view->minimum_width = 480;
  view->minimum_height = 240;
  view->policy = CHARDIO_AUV3_VIEW_POLICY_RESIZABLE;
  return true;
}

chardio_auv3_mpe_policy_t CLAP_ABI getMPEPolicy(const clap_plugin_t *plugin)
{
  return static_cast<TestState *>(plugin->plugin_data)->mpePolicy;
}

const chardio_plugin_auv3_metadata_t metadata{
    sizeof(chardio_plugin_auv3_metadata_t), CHARDIO_PLUGIN_AUV3_METADATA_VERSION,
    nullptr,                                presetCount,
    getPreset,                              loadPreset,
    prepare,                                getView,
    getMPEPolicy,
};

const chardio_plugin_auv3_metadata_t truncatedMetadata{
    fieldEnd(offsetof(chardio_plugin_auv3_metadata_t, get_parameter_metadata),
             sizeof(metadata.get_parameter_metadata)),
    CHARDIO_PLUGIN_AUV3_METADATA_VERSION,
    nullptr,
};

uint32_t CLAP_ABI notePortCount(const clap_plugin_t *, bool isInput)
{
  return isInput ? 2 : 0;
}

bool CLAP_ABI getNotePort(const clap_plugin_t *plugin, uint32_t index, bool isInput,
                          clap_note_port_info_t *info)
{
  if (!isInput || index > 1 || !info) return false;
  *info = {};
  const auto &state = *static_cast<const TestState *>(plugin->plugin_data);
  info->supported_dialects = index == 1 ? CLAP_NOTE_DIALECT_MIDI_MPE : CLAP_NOTE_DIALECT_MIDI;
  if (state.supportsMIDI2 && index == 0) info->supported_dialects |= CLAP_NOTE_DIALECT_MIDI2;
  return true;
}

const clap_plugin_note_ports_t notePorts{notePortCount, getNotePort};

const void *CLAP_ABI getExtension(const clap_plugin_t *plugin, const char *id)
{
  const auto &state = *static_cast<TestState *>(plugin->plugin_data);
  if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) return &notePorts;
  if (std::strcmp(id, CHARDIO_PLUGIN_EXT_AUV3_METADATA) != 0) return nullptr;
  if (state.mode == MetadataMode::absent) return nullptr;
  return state.mode == MetadataMode::truncated ? &truncatedMetadata : &metadata;
}

clap_plugin_t makePlugin(TestState &state)
{
  clap_plugin_t plugin{};
  plugin.plugin_data = &state;
  plugin.get_extension = getExtension;
  return plugin;
}

void testPresentMetadata()
{
  TestState state;
  auto plugin = makePlugin(state);
  const auto metadata = Clap::AUv3::ChardioRuntimeMetadata::find(&plugin);
  assert(metadata.prepareForAUv3(&plugin));
  const auto presets = metadata.factoryPresets(&plugin);
  assert(presets.size() == 2 && presets[0].number == 3 && presets[1].name == "Second");
  assert(metadata.loadFactoryPreset(&plugin, 3));
  assert(state.loadedPreset && state.loadedNumber == 3);
  Clap::AUv3::ChardioViewConfiguration view;
  assert(metadata.getViewConfiguration(&plugin, view));
  assert(view.policy == CHARDIO_AUV3_VIEW_POLICY_RESIZABLE && view.minimumWidth == 480 &&
         view.minimumHeight == 240);
  assert(metadata.supportsMPE(&plugin));
}

void testAbsentAndTruncatedMetadata()
{
  for (const auto mode : {MetadataMode::absent, MetadataMode::truncated})
  {
    TestState state;
    state.mode = mode;
    auto plugin = makePlugin(state);
    const auto metadata = Clap::AUv3::ChardioRuntimeMetadata::find(&plugin);
    assert(metadata.prepareForAUv3(&plugin));
    assert(metadata.factoryPresets(&plugin).empty());
    assert(!metadata.loadFactoryPreset(&plugin, 3));
    Clap::AUv3::ChardioViewConfiguration view;
    assert(!metadata.getViewConfiguration(&plugin, view));
    assert(metadata.supportsMPE(&plugin));
  }
}

void testPreparationAndMPEPolicy()
{
  TestState state;
  state.prepareResult = false;
  auto plugin = makePlugin(state);
  const auto metadata = Clap::AUv3::ChardioRuntimeMetadata::find(&plugin);
  assert(!metadata.prepareForAUv3(&plugin));

  state.mpePolicy = CHARDIO_AUV3_MPE_POLICY_DISABLED;
  assert(!metadata.supportsMPE(&plugin));
  state.mpePolicy = CHARDIO_AUV3_MPE_POLICY_ENABLED;
  assert(metadata.supportsMPE(&plugin));
}

void testMIDI2CapabilityDecision()
{
  TestState state;
  auto plugin = makePlugin(state);
  assert(!Clap::AUv3::inputNotePortsSupportMIDI2(&plugin, &notePorts));
  state.supportsMIDI2 = true;
  assert(Clap::AUv3::inputNotePortsSupportMIDI2(&plugin, &notePorts));
}

void testTailTimeBoundaries()
{
  constexpr double sampleRate = 48000.0;
  constexpr auto maximumFinite = static_cast<uint32_t>(std::numeric_limits<int32_t>::max() - 1);
  assert(Clap::AUv3::tailTimeForSamples(0, sampleRate) == 0);
  assert(Clap::AUv3::tailTimeForSamples(maximumFinite, sampleRate) ==
         static_cast<double>(maximumFinite) / sampleRate);
  assert(std::isinf(Clap::AUv3::tailTimeForSamples(
      static_cast<uint32_t>(std::numeric_limits<int32_t>::max()), sampleRate)));
  assert(std::isinf(Clap::AUv3::tailTimeForSamples(
      std::numeric_limits<uint32_t>::max(), sampleRate)));
}
}  // namespace

int main()
{
  testPresentMetadata();
  testAbsentAndTruncatedMetadata();
  testPreparationAndMPEPolicy();
  testMIDI2CapabilityDecision();
  testTailTimeBoundaries();
}
