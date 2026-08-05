# Fork notes

This fork keeps focused host and format work that is not yet available in
upstream `free-audio/clap-wrapper`.

The retained changes cover:

- sample-accurate AUv3 parameter points, ramps, MIDI 2 events, and render timing;
- AUv3 parameter flushing, render-buffer safety, tail updates, macOS packaging,
  iOS static-linked packaging, and optional product factory sources;
- CHOC-based standalone audio, MIDI, device settings, and hardened lifecycle
  rollback;
- host-owned `clap.webview/3` presentation in macOS standalone and AUv3 builds,
  with ordinary `CLAP_EXT_GUI` fallback;
- WCLAP output from the same statically linked CLAP implementation.

`clap.webview/3` support is deliberately host-owned. The plug-in supplies its
URI, resources, and binary messages. The wrapper owns the native WebView and
bridges `MessageEvent`/`window.parent.postMessage`. AUv3 WebView support is
enabled when `CLAP_WRAPPER_CHOC_ROOT` points at a CHOC checkout; native CLAP GUI
hosting remains available without CHOC.

Product-specific extensions and metadata are not part of the fork. Products
provide standard CLAP parameter, state, port, preset, GUI, and webview
extensions directly.
