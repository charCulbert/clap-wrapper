#pragma once

// The Chardio header is preferred when clap-wrapper is built as part of
// Chardio. Keep this compatibility declaration local so standalone
// clap-wrapper builds can safely ignore an unavailable optional extension.
#if defined(__has_include)
#if __has_include(<chardio/clap/chardio_CLAPAUv3Metadata.h>)
#define CLAP_WRAPPER_HAS_CHARDIO_AUV3_METADATA 1
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <chardio/clap/chardio_CLAPAUv3Metadata.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif
#endif

#if !defined(CLAP_WRAPPER_HAS_CHARDIO_AUV3_METADATA)

#include <clap/clap.h>

#include <stdbool.h>
#include <stdint.h>

#define CHARDIO_PLUGIN_EXT_AUV3_METADATA "com.charculbert.chardio.auv3-metadata/1"
#define CHARDIO_PLUGIN_AUV3_METADATA_VERSION 1

typedef struct chardio_auv3_parameter_metadata
{
  uint32_t struct_size;
  const char *identifier;
  uint32_t unit;
  const char *unit_name;
  const char *const *value_strings;
  uint32_t value_string_count;
} chardio_auv3_parameter_metadata_t;

typedef struct chardio_plugin_auv3_metadata
{
  uint32_t struct_size;
  uint32_t version;
  bool(CLAP_ABI *get_parameter_metadata)(const clap_plugin_t *plugin, clap_id parameter_id,
                                         chardio_auv3_parameter_metadata_t *metadata,
                                         uint32_t metadata_capacity);
} chardio_plugin_auv3_metadata_t;

#endif
