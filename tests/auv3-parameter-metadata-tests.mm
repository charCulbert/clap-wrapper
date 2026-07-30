#include "../src/detail/auv3/auv3_parameters.h"

#include <clapwrapper/chardio-auv3-metadata.h>

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
enum class MetadataMode
{
  present,
  partial,
  absent,
  unsupportedVersion,
  truncatedTable,
};

struct TestState
{
  MetadataMode metadataMode = MetadataMode::absent;
  const char *identifier = nullptr;
  uint32_t metadataCapacity = 0;
  uint32_t metadataStructSize = 0;
  uint32_t metadataCalls = 0;
};

constexpr uint32_t fieldEnd(size_t offset, size_t fieldSize)
{
  return static_cast<uint32_t>(offset + fieldSize);
}

bool CLAP_ABI getParameterMetadata(const clap_plugin_t *plugin, clap_id parameterId,
                                   chardio_auv3_parameter_metadata_t *metadata,
                                   uint32_t metadataCapacity)
{
  auto &state = *static_cast<TestState *>(plugin->plugin_data);
  ++state.metadataCalls;
  state.metadataCapacity = metadataCapacity;
  state.metadataStructSize = metadata ? metadata->struct_size : 0;
  if (!metadata || parameterId != 17) return false;

  if (state.metadataMode == MetadataMode::partial)
  {
    metadata->struct_size =
        fieldEnd(offsetof(chardio_auv3_parameter_metadata_t, identifier), sizeof(metadata->identifier));
    metadata->identifier = state.identifier;
    return true;
  }

  if (state.metadataMode != MetadataMode::present) return false;

  static constexpr const char *valueStrings[] = {"Slow", "Fast"};
  metadata->struct_size = sizeof(*metadata);
  metadata->identifier = state.identifier;
  metadata->unit = kAudioUnitParameterUnit_Indexed;
  metadata->unit_name = "Mode";
  metadata->value_strings = valueStrings;
  metadata->value_string_count = 2;
  return true;
}

chardio_plugin_auv3_metadata_t makeMetadataTable(
    uint32_t structSize, uint32_t version,
    bool(CLAP_ABI *getParameterMetadataCallback)(const clap_plugin_t *, clap_id,
                                                 chardio_auv3_parameter_metadata_t *, uint32_t))
{
  chardio_plugin_auv3_metadata_t metadata{};
  metadata.struct_size = structSize;
  metadata.version = version;
  metadata.get_parameter_metadata = getParameterMetadataCallback;
  return metadata;
}

const auto validMetadata = makeMetadataTable(sizeof(chardio_plugin_auv3_metadata_t),
                                             CHARDIO_PLUGIN_AUV3_METADATA_VERSION, getParameterMetadata);

const auto unsupportedVersionMetadata =
    makeMetadataTable(sizeof(chardio_plugin_auv3_metadata_t), CHARDIO_PLUGIN_AUV3_METADATA_VERSION + 1,
                      getParameterMetadata);

const auto truncatedMetadata =
    makeMetadataTable(fieldEnd(offsetof(chardio_plugin_auv3_metadata_t, get_parameter_metadata), 0),
                      CHARDIO_PLUGIN_AUV3_METADATA_VERSION, nullptr);

const void *CLAP_ABI getExtension(const clap_plugin_t *plugin, const char *id)
{
  if (std::strcmp(id, CHARDIO_PLUGIN_EXT_AUV3_METADATA) != 0) return nullptr;

  const auto mode = static_cast<TestState *>(plugin->plugin_data)->metadataMode;
  switch (mode)
  {
    case MetadataMode::present:
    case MetadataMode::partial:
      return &validMetadata;
    case MetadataMode::unsupportedVersion:
      return &unsupportedVersionMetadata;
    case MetadataMode::truncatedTable:
      return &truncatedMetadata;
    case MetadataMode::absent:
      return nullptr;
  }
}

uint32_t CLAP_ABI parameterCount(const clap_plugin_t *)
{
  return 1;
}

bool CLAP_ABI getParameterInfo(const clap_plugin_t *, uint32_t index, clap_param_info_t *info)
{
  if (index != 0 || !info) return false;
  *info = {};
  info->id = 17;
  info->flags = CLAP_PARAM_IS_AUTOMATABLE;
  std::strcpy(info->name, "Speed");
  info->min_value = 0.0;
  info->max_value = 1.0;
  info->default_value = 0.0;
  return true;
}

bool CLAP_ABI getParameterValue(const clap_plugin_t *, clap_id parameterId, double *value)
{
  if (parameterId != 17 || !value) return false;
  *value = 0.0;
  return true;
}

bool CLAP_ABI parameterValueToText(const clap_plugin_t *, clap_id, double value, char *text,
                                   uint32_t textCapacity)
{
  if (!text || textCapacity < 2) return false;
  std::snprintf(text, textCapacity, "%.0f", value);
  return true;
}

bool CLAP_ABI parameterTextToValue(const clap_plugin_t *, clap_id, const char *text, double *value)
{
  if (!text || !value) return false;
  *value = std::strtod(text, nullptr);
  return true;
}

void CLAP_ABI flushParameters(const clap_plugin_t *, const clap_input_events_t *,
                              const clap_output_events_t *)
{
}

const clap_plugin_params_t parameters{
    parameterCount,       getParameterInfo,     getParameterValue,
    parameterValueToText, parameterTextToValue, flushParameters,
};

clap_plugin_t makePlugin(TestState &state)
{
  clap_plugin_t plugin{};
  plugin.plugin_data = &state;
  plugin.get_extension = getExtension;
  return plugin;
}

AUParameter *firstParameter(AUParameterTree *tree)
{
  auto *parameter = [tree parameterWithAddress:17];
  assert(parameter != nil);
  return parameter;
}

void expectFallback(const clap_plugin_t *plugin)
{
  auto result = Clap::AUv3::createParameterTree(plugin, &parameters);
  auto *parameter = firstParameter(result.tree);
  assert([parameter.identifier isEqualToString:@"clap_17"]);
  assert(parameter.unit == kAudioUnitParameterUnit_Generic);
  assert(parameter.unitName.length == 0);
  assert(parameter.valueStrings.count == 0);
}

void testPresentMetadataAndInstances()
{
  TestState firstState{MetadataMode::present, "com_example_first_speed"};
  TestState secondState{MetadataMode::present, "com_example_second_speed"};
  auto firstPlugin = makePlugin(firstState);
  auto secondPlugin = makePlugin(secondState);

  auto firstResult = Clap::AUv3::createParameterTree(&firstPlugin, &parameters);
  auto secondResult = Clap::AUv3::createParameterTree(&secondPlugin, &parameters);
  auto *first = firstParameter(firstResult.tree);
  auto *second = firstParameter(secondResult.tree);

  assert([first.identifier isEqualToString:@"com_example_first_speed"]);
  assert([second.identifier isEqualToString:@"com_example_second_speed"]);
  assert(first.unit == kAudioUnitParameterUnit_Indexed);
  assert([first.unitName isEqualToString:@"Mode"]);
  assert(first.valueStrings.count == 2);
  assert([first.valueStrings[0] isEqualToString:@"Slow"]);
  assert([first.valueStrings[1] isEqualToString:@"Fast"]);
  assert(firstState.metadataCalls == 1 && secondState.metadataCalls == 1);
  assert(firstState.metadataCapacity == sizeof(chardio_auv3_parameter_metadata_t));
  assert(firstState.metadataStructSize == sizeof(chardio_auv3_parameter_metadata_t));
}

void testPartialAndUnavailableMetadata()
{
  TestState partialState{MetadataMode::partial, "com_example_partial_speed"};
  auto partialPlugin = makePlugin(partialState);
  auto partialResult = Clap::AUv3::createParameterTree(&partialPlugin, &parameters);
  auto *partial = firstParameter(partialResult.tree);
  assert([partial.identifier isEqualToString:@"com_example_partial_speed"]);
  assert(partial.unit == kAudioUnitParameterUnit_Generic);
  assert(partial.unitName.length == 0 && partial.valueStrings.count == 0);

  for (const auto mode :
       {MetadataMode::absent, MetadataMode::unsupportedVersion, MetadataMode::truncatedTable})
  {
    TestState state{mode, "ignored"};
    auto plugin = makePlugin(state);
    expectFallback(&plugin);
    assert(state.metadataCalls == 0);
  }
}

void testReservedIdentifierFallsBack()
{
  TestState state{MetadataMode::present, "com.example.invalid"};
  auto plugin = makePlugin(state);
  auto result = Clap::AUv3::createParameterTree(&plugin, &parameters);
  auto *parameter = firstParameter(result.tree);
  assert([parameter.identifier isEqualToString:@"clap_17"]);
  assert(parameter.unit == kAudioUnitParameterUnit_Indexed);
  assert(state.metadataCalls == 1);
}
}  // namespace

int main()
{
  @autoreleasepool
  {
    testPresentMetadataAndInstances();
    testPartialAndUnavailableMetadata();
    testReservedIdentifierFallsBack();
  }
}
