#pragma once

// Optional plugin extension used by the AUv3 wrapper to preserve an
// AURenderEventParameterRamp as one plugin-defined CLAP event.

#include <clap/clap.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLAP_WRAPPER_EXT_AUV3_PARAM_RAMP "com.charculbert.clap-wrapper.auv3-param-ramp/1"
#define CLAP_WRAPPER_AUV3_PARAM_RAMP_ABI_VERSION 1u

typedef struct clap_wrapper_auv3_param_ramp_info {
  uint32_t sample_offset;
  clap_id parameter_id;
  uint64_t parameter_address;
  double target_value;
  void *cookie;
  uint32_t duration_sample_frames;
} clap_wrapper_auv3_param_ramp_info_t;

// Called by the AUv3 render thread. The implementation must write one complete
// CLAP event to event_storage, set event_size to its exact byte size, and return
// true. event_storage belongs to the wrapper and is valid only for this call.
typedef bool(CLAP_ABI *clap_wrapper_auv3_param_ramp_translate_t)(
    const clap_wrapper_auv3_param_ramp_info_t *info, void *event_storage,
    uint32_t event_storage_capacity, uint32_t *event_size);

typedef struct clap_wrapper_plugin_auv3_param_ramp {
  uint32_t abi_version;
  clap_wrapper_auv3_param_ramp_translate_t translate;
} clap_wrapper_plugin_auv3_param_ramp_t;

#ifdef __cplusplus
}
#endif
