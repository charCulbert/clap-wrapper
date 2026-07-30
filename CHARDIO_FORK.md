# Chardio downstream fork

The `chardio/main` branch carries Chardio-maintained downstream changes and
tracks the `free-audio/clap-wrapper` upstream project.

`make_clapfirst_plugins` accepts `AUV3_BUILD_STANDALONE TRUE|FALSE`. It defaults
to `TRUE` for compatibility; `FALSE` creates the AUv3 appex without adding the
macOS AUv3 standalone host target.
