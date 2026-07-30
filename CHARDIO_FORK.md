# Chardio downstream fork

The `chardio/main` branch carries Chardio-maintained downstream changes and
tracks the `free-audio/clap-wrapper` upstream project.

`make_clapfirst_plugins` accepts `AUV3_BUILD_STANDALONE TRUE|FALSE`. It defaults
to `TRUE` for compatibility; `FALSE` creates the AUv3 appex without adding the
macOS AUv3 standalone host target.

## AUv3 CLAP event ingress

The wrapper host provides `CLAP_EXT_EVENT_REGISTRY`. Event-space names are
registered only from the plugin's main-thread initialization path; IDs are
stable, nonzero, and valid for that `Clap::Plugin` lifetime.

`include/clapwrapper/auv3-param-ramp.h` declares the downstream
`com.charculbert.clap-wrapper.auv3-param-ramp/1` plugin extension. A plugin
that advertises ABI version 1 receives each `AURenderEventParameterRamp` as one
plugin-defined event through its fixed, allocation-free translator. The input
includes the CLAP id, original AU parameter address, cookie, target, sample
offset, and unmodified duration. The translator also receives the owning
`clap_plugin_t*`, allowing it to use per-instance state such as the event-space
ID registered during initialization. Plugins without this extension retain the
legacy `CLAP_EVENT_PARAM_VALUE` fallback; the AUv3 adapter exposes both
fallback and translation-failure counters in `OverflowCounts`.

The AUv3 event union uses a fixed 64-byte custom-event slot. Chardio's ramp
event measures 64 bytes and the previous wrapper union measured 56 bytes, so
64 bytes is the smallest exact fit. At the default 8192 input-event capacity,
this adds 64 KiB. MIDI Event Lists are converted to bounded `CLAP_EVENT_MIDI2`
events without growing render-time storage.

Single-output AUv3 renders always process each invocation, even when hosts
reuse an `AudioTimeStamp`; only multi-bus renders cache adapter-owned output.
Note IDs are assigned after same-sample reordering, from the prior block's
active-note snapshot. An unadmitted note is dropped rather than exposing an
ID that cannot be paired with its later NOTE_OFF.
