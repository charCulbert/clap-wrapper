#pragma once

// Optional host extension supplied by the clap-wrapper standalone host. Every
// function except event ingress and output dequeue is main-thread-only.
// Snapshot functions use a two-call size negotiation: pass null storage with
// zero capacity, then retry with the returned required count.
// Event queues use fixed storage. Input producers may retry under contention.
// The input render consumer makes bounded non-blocking FIFO attempts and never
// waits for a reserved cell; a stalled input reservation can temporarily hide
// later published cells until that producer publishes. Output uses one producer
// (audio process while active, main-thread flush while inactive) and one
// consumer, with a bounded constant-time push.

#include <clap/clap.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLAP_WRAPPER_EXT_STANDALONE_SERVICES "com.free-audio.clap-wrapper.standalone-services/1"
#define CLAP_WRAPPER_STANDALONE_SERVICES_ABI_VERSION 1u
#define CLAP_WRAPPER_STANDALONE_DEVICE_NAME_CAPACITY 128u
#define CLAP_WRAPPER_STANDALONE_MAX_SELECTED_MIDI_PORTS 32u
#define CLAP_WRAPPER_STANDALONE_EVENT_SIZE_CAPACITY 1024u
#define CLAP_WRAPPER_STANDALONE_AUDIO_INPUT_ENABLED (1u << 0)
#define CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT_ENABLED (1u << 1)
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

typedef bool(CLAP_ABI *clap_wrapper_standalone_get_audio_snapshot_t)(
    const clap_host_t *host, clap_wrapper_standalone_audio_snapshot_t *snapshot);
typedef bool(CLAP_ABI *clap_wrapper_standalone_apply_audio_settings_t)(
    const clap_host_t *host, const clap_wrapper_standalone_audio_settings_t *settings);
typedef bool(CLAP_ABI *clap_wrapper_standalone_get_midi_snapshot_t)(
    const clap_host_t *host, clap_wrapper_standalone_midi_snapshot_t *snapshot);
typedef bool(CLAP_ABI *clap_wrapper_standalone_set_midi_port_open_t)(
    const clap_host_t *host, uint64_t port_id, bool should_open);
// Thread-safe and bounded. event->time is the caller's sample offset in the
// next rendered block and is copied without alteration. timestamp_ns only
// gives a stable order to events with the same offset. The host copies exactly
// event_size bytes, including application-defined ramp payloads.
typedef bool(CLAP_ABI *clap_wrapper_standalone_enqueue_event_t)(
    const clap_host_t *host, const clap_event_header_t *event, uint32_t event_size,
    uint64_t timestamp_ns);
// Thread-safe and bounded ingress for external devices whose timestamp_ns uses
// the host monotonic clock. An event captured during block A targets block B at
// the same normalized phase. If its reservation publishes too late for B, or a
// stalled earlier FIFO cell blocks it, the first later eligible block receives
// it at that preserved phase. Late events, unavailable clocks, and invalid
// callback anchors are delivered at frame zero.
typedef bool(CLAP_ABI *clap_wrapper_standalone_enqueue_timestamped_event_t)(
    const clap_host_t *host, const clap_event_header_t *event, uint32_t event_size,
    uint64_t timestamp_ns);
// Copies and removes one plugin output event. event_capacity must be at least
// CLAP_WRAPPER_STANDALONE_EVENT_SIZE_CAPACITY and info->struct_size must cover
// the full v1 info struct. The single consumer makes one bounded, non-blocking
// attempt.
// false means empty or bad destination storage. CLAP header.time is exact.
// HAS_BLOCK_CONTEXT gives the render block needed to schedule header.time;
// inactive parameter-flush output has zero block fields and no context flag.
typedef bool(CLAP_ABI *clap_wrapper_standalone_dequeue_output_event_t)(
    const clap_host_t *host, void *event, uint32_t event_capacity,
    clap_wrapper_standalone_output_event_info_t *info);
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
  clap_wrapper_standalone_enqueue_timestamped_event_t enqueue_timestamped_event;
  clap_wrapper_standalone_dequeue_output_event_t dequeue_output_event;
  clap_wrapper_standalone_get_event_telemetry_t get_output_event_telemetry;
} clap_wrapper_host_standalone_services_t;

// ABI version remains 1. Consumers must guard every appended function pointer
// with struct_size before reading or calling it. Older v1 hosts may end after
// get_event_telemetry.
#define CLAP_WRAPPER_STANDALONE_SERVICES_HAS_MEMBER(services, member)                         \
  ((services) != NULL &&                                                                      \
   (services)->struct_size >= offsetof(clap_wrapper_host_standalone_services_t, member) +     \
                                  sizeof((services)->member))

#ifdef __cplusplus
}
#endif
