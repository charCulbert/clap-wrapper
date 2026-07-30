#pragma once

// Optional host extension supplied by the clap-wrapper standalone host.  Every
// function except enqueue_event is main-thread-only. Snapshot functions use a
// two-call size negotiation: pass null storage with zero capacity, then retry
// with the returned required count.

#include <clap/clap.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLAP_WRAPPER_EXT_STANDALONE_SERVICES "com.free-audio.clap-wrapper.standalone-services/1"
#define CLAP_WRAPPER_STANDALONE_SERVICES_ABI_VERSION 1u
#define CLAP_WRAPPER_STANDALONE_DEVICE_NAME_CAPACITY 128u
#define CLAP_WRAPPER_STANDALONE_MAX_SELECTED_MIDI_PORTS 32u
#define CLAP_WRAPPER_STANDALONE_AUDIO_INPUT_ENABLED (1u << 0)
#define CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT_ENABLED (1u << 1)

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

typedef bool(CLAP_ABI *clap_wrapper_standalone_get_audio_snapshot_t)(
    const clap_host_t *host, clap_wrapper_standalone_audio_snapshot_t *snapshot);
typedef bool(CLAP_ABI *clap_wrapper_standalone_apply_audio_settings_t)(
    const clap_host_t *host, const clap_wrapper_standalone_audio_settings_t *settings);
typedef bool(CLAP_ABI *clap_wrapper_standalone_get_midi_snapshot_t)(
    const clap_host_t *host, clap_wrapper_standalone_midi_snapshot_t *snapshot);
typedef bool(CLAP_ABI *clap_wrapper_standalone_set_midi_port_open_t)(
    const clap_host_t *host, uint64_t port_id, bool should_open);
// Thread-safe and bounded. timestamp_ns is a monotonic ingestion timestamp
// used to break equal CLAP sample-time ties. event->time is a sample offset in
// the next rendered block; the standalone host rejects events outside that
// block rather than delaying a stale offset. The host copies exactly
// event_size bytes, including application-defined ramp payloads.
typedef bool(CLAP_ABI *clap_wrapper_standalone_enqueue_event_t)(
    const clap_host_t *host, const clap_event_header_t *event, uint32_t event_size,
    uint64_t timestamp_ns);
typedef bool(CLAP_ABI *clap_wrapper_standalone_get_event_telemetry_t)(
    const clap_host_t *host, clap_wrapper_standalone_event_telemetry_t *telemetry);

typedef struct clap_wrapper_host_standalone_services {
  uint32_t abi_version;
  uint32_t struct_size;
  clap_wrapper_standalone_get_audio_snapshot_t get_audio_snapshot;
  clap_wrapper_standalone_apply_audio_settings_t apply_audio_settings;
  clap_wrapper_standalone_get_midi_snapshot_t get_midi_snapshot;
  clap_wrapper_standalone_set_midi_port_open_t set_midi_port_open;
  clap_wrapper_standalone_enqueue_event_t enqueue_event;
  clap_wrapper_standalone_get_event_telemetry_t get_event_telemetry;
} clap_wrapper_host_standalone_services_t;

#ifdef __cplusplus
}
#endif
