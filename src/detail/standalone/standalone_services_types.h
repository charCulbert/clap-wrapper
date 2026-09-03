#pragma once

// Plain data types shared by the standalone host's services core, settings
// persistence and native device UI. Internal to the wrapper.

#include <clap/clap.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLAP_WRAPPER_STANDALONE_DEVICE_NAME_CAPACITY 128u
#define CLAP_WRAPPER_STANDALONE_MAX_SELECTED_MIDI_PORTS 32u
#define CLAP_WRAPPER_STANDALONE_EVENT_SIZE_CAPACITY 1024u
#define CLAP_WRAPPER_STANDALONE_AUDIO_INPUT_ENABLED (1u << 0)
#define CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT_ENABLED (1u << 1)
#define CLAP_WRAPPER_STANDALONE_AUDIO_FOLLOW_DEFAULT_OUTPUT (1u << 2)
#define CLAP_WRAPPER_STANDALONE_OUTPUT_EVENT_HAS_BLOCK_CONTEXT (1u << 0)

typedef struct clap_wrapper_standalone_audio_device {
  uint32_t struct_size;
  uint64_t id;
  uint32_t input_channels;
  uint32_t output_channels;
  uint32_t flags;
  char name[CLAP_WRAPPER_STANDALONE_DEVICE_NAME_CAPACITY];
} clap_wrapper_standalone_audio_device_t;

typedef struct clap_wrapper_standalone_midi_port {
  uint32_t struct_size;
  uint64_t id;
  uint32_t flags;
  char name[CLAP_WRAPPER_STANDALONE_DEVICE_NAME_CAPACITY];
} clap_wrapper_standalone_midi_port_t;

typedef struct clap_wrapper_standalone_audio_settings {
  uint32_t struct_size;
  uint64_t input_device_id;
  uint64_t output_device_id;
  uint32_t input_channels;
  uint32_t output_channels;
  uint32_t sample_rate;
  uint32_t buffer_size;
  uint32_t flags;
} clap_wrapper_standalone_audio_settings_t;

typedef struct clap_wrapper_standalone_audio_snapshot {
  uint32_t struct_size;
  clap_wrapper_standalone_audio_device_t *input_devices;
  uint32_t input_device_capacity;
  uint32_t input_device_count;
  clap_wrapper_standalone_audio_device_t *output_devices;
  uint32_t output_device_capacity;
  uint32_t output_device_count;
  clap_wrapper_standalone_audio_settings_t selected;
} clap_wrapper_standalone_audio_snapshot_t;

typedef struct clap_wrapper_standalone_midi_snapshot {
  uint32_t struct_size;
  clap_wrapper_standalone_midi_port_t *ports;
  uint32_t port_capacity;
  uint32_t port_count;
  uint64_t *selected_port_ids;
  uint32_t selected_port_capacity;
  uint32_t selected_port_count;
} clap_wrapper_standalone_midi_snapshot_t;

typedef struct clap_wrapper_standalone_event_telemetry {
  uint32_t struct_size;
  uint32_t event_capacity;
  uint32_t event_size_capacity;
  uint64_t accepted_events;
  uint64_t dropped_events;
  uint64_t rejected_events;
  uint64_t consumed_events;
} clap_wrapper_standalone_event_telemetry_t;

typedef struct clap_wrapper_standalone_output_event_info {
  uint32_t struct_size;
  uint32_t event_size;
  uint32_t flags;
  uint32_t sample_rate;
  uint32_t frame_count;
  uint32_t reserved;
  uint64_t block_sequence;
  uint64_t block_start_time_ns;
} clap_wrapper_standalone_output_event_info_t;

#ifdef __cplusplus
}
#endif
