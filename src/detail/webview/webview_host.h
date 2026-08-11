#pragma once

#include <clap/clap.h>
#include <clap/ext/draft/webview.h>

#include <choc/gui/choc_WebView.h>
#include "clap_proxy.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace freeaudio::clap_wrapper
{

class WebViewHost
{
  template <typename Function>
  decltype(auto) withMainThread(Function &&function) const
  {
    if (threadCheckedPlugin != nullptr)
    {
      auto guard = threadCheckedPlugin->AlwaysMainThread();
      return function();
    }

    return function();
  }

public:
  using HostReceive = std::function<bool(const void *, uint32_t)>;

  WebViewHost(const clap_plugin_t *plugin, const clap_plugin_webview_t *webview,
              const clap_plugin_gui_t *sizingGui, Clap::Plugin *threadCheckedPlugin = nullptr,
              bool deferInitialNavigation = false, HostReceive hostReceive = {})
      : plugin(plugin), webview(webview), sizingGui(sizingGui),
        threadCheckedPlugin(threadCheckedPlugin), hostReceive(std::move(hostReceive))
  {
    if (!plugin || !webview || !webview->get_uri || !webview->get_resource || !webview->receive)
      return;

    const auto uriSize = withMainThread([&] { return webview->get_uri(plugin, nullptr, 0); });
    if (uriSize <= 1) return;

    std::string uri(static_cast<size_t>(uriSize), '\0');
    if (withMainThread([&] {
          return webview->get_uri(plugin, uri.data(), static_cast<uint32_t>(uri.size()));
        }) != uriSize)
      return;
    uri.resize(static_cast<size_t>(uriSize - 1));

    startSizingGui();

    choc::ui::WebView::Options options;
#if defined(CLAP_WRAPPER_HAS_CHOC_DEFERRED_NAVIGATION)
    options.deferInitialNavigation = deferInitialNavigation;
#else
    (void) deferInitialNavigation;
#endif
    auto navigationURI = uri;
    auto navigateWhenReady = true;
    if (!isAbsoluteURI(uri))
    {
      const auto slash = uri.find_last_of('/');
      const auto directory = slash == std::string::npos ? std::string("/") : uri.substr(0, slash + 1);
      options.customSchemeURI = "charclap://ui" + directory;
      options.fetchResource = [this, directory, uri](const std::string &path) {
        return fetchResource(isDirectoryRequest(path, directory) ? uri : path);
      };
      navigateWhenReady = false;
    }

    options.webviewIsReady = [this, navigationURI, navigateWhenReady](choc::ui::WebView &view) {
      view.bind("clapHostPostMessage", [this](const choc::value::ValueView &arguments) {
        if (!this->webview || !this->webview->receive || !arguments.isArray() ||
            arguments.size() != 1 ||
            !arguments[0].isArray())
          return choc::value::Value(false);

        const auto source = arguments[0];
        std::vector<uint8_t> bytes(source.size());
        for (uint32_t i = 0; i < source.size(); ++i)
          bytes[i] = static_cast<uint8_t>(source[i].getWithDefault<int32_t>(0));

        if (this->hostReceive && this->hostReceive(bytes.data(), static_cast<uint32_t>(bytes.size())))
          return choc::value::Value(true);

        const auto accepted = this->withMainThread([&] {
          return this->webview->receive(this->plugin, bytes.data(),
                                        static_cast<uint32_t>(bytes.size()));
        });
        return choc::value::Value(accepted);
      });
      view.addInitScript(messageBridgeScript);
      if (this->hostReceive) view.addInitScript(standaloneMappingScript);
      if (navigateWhenReady) view.navigate(navigationURI);
    };

    nativeView = std::make_unique<choc::ui::WebView>(options);
    if (!nativeView->loadedOK()) nativeView.reset();
  }

  ~WebViewHost()
  {
    nativeView.reset();
    stopSizingGui();
  }

  WebViewHost(const WebViewHost &) = delete;
  WebViewHost &operator=(const WebViewHost &) = delete;

  [[nodiscard]] bool isOpen() const noexcept { return nativeView != nullptr; }

  bool navigate()
  {
    return nativeView && nativeView->navigate({});
  }

  [[nodiscard]] void *viewHandle() const noexcept
  {
    return nativeView ? nativeView->getViewHandle() : nullptr;
  }

  [[nodiscard]] uint32_t width() const noexcept { return viewWidth; }
  [[nodiscard]] uint32_t height() const noexcept { return viewHeight; }
  [[nodiscard]] bool canResize() const noexcept
  {
    return sizingGui && withMainThread([&] { return sizingGui->can_resize(plugin); });
  }

  bool setSize(uint32_t width, uint32_t height)
  {
    if (sizingGui && !withMainThread([&] { return sizingGui->set_size(plugin, width, height); }))
      return false;
    viewWidth = width;
    viewHeight = height;
    return true;
  }

  bool adjustSize(uint32_t &width, uint32_t &height) const
  {
    return !sizingGui || !sizingGui->adjust_size ||
           withMainThread([&] { return sizingGui->adjust_size(plugin, &width, &height); });
  }

  bool send(const void *buffer, uint32_t size)
  {
    if (!nativeView || (!buffer && size != 0)) return false;

    const auto *bytes = static_cast<const uint8_t *>(buffer);
    std::ostringstream script;
    script << "window.dispatchEvent(new MessageEvent('message',{data:new Uint8Array([";
    for (uint32_t i = 0; i < size; ++i)
    {
      if (i != 0) script << ',';
      script << static_cast<uint32_t>(bytes[i]);
    }
    script << "]).buffer}));";
    return nativeView->evaluateJavascript(script.str());
  }

private:
  static constexpr uint64_t maximumResourceSize = 64 * 1024 * 1024;
  static constexpr const char *messageBridgeScript = R"JS(
(() => {
  window.addEventListener('message', event => {
    if (event.source !== window)
      return;
    let bytes;
    if (event.data instanceof ArrayBuffer)
      bytes = new Uint8Array(event.data);
    else if (ArrayBuffer.isView(event.data))
      bytes = new Uint8Array(event.data.buffer, event.data.byteOffset, event.data.byteLength);
    else
      return;
    event.stopImmediatePropagation();
    clapHostPostMessage(Array.from(bytes));
  }, { capture: true });
})();
)JS";

  static constexpr const char *standaloneMappingScript = R"JS(
(() => {
  if (window.__clapWrapperStandaloneMapping)
    return;
  window.__clapWrapperStandaloneMapping = true;

  const prefix = 'standalone-map:';
  const notePrefix = 'standalone-note:';
  const encoder = new TextEncoder();
  const decoder = new TextDecoder();
  const mappings = new Map();
  let mappingMode = false;
  let target = null;
  let keyboardVisible = false;
  const activeNotes = new Set();

  const panel = document.createElement('div');
  panel.id = 'clap-wrapper-standalone-midi';
  const shadow = panel.attachShadow({ mode: 'open' });
  shadow.innerHTML = `
    <style>
      :host {
        position: fixed;
        top: 8px;
        right: 8px;
        z-index: 2147483647;
        display: block;
        color: #f4f4f4;
        font: 12px -apple-system, BlinkMacSystemFont, sans-serif;
        pointer-events: auto;
      }
      .panel {
        min-width: 180px;
        max-width: 280px;
        padding: 8px;
        border: 1px solid #777;
        border-radius: 6px;
        background: #202020;
        box-shadow: 0 3px 14px rgba(0, 0, 0, .35);
      }
      .toolbar, .row { display: flex; align-items: center; gap: 6px; }
      .toolbar { justify-content: space-between; }
      button {
        color: inherit;
        border: 1px solid #777;
        border-radius: 4px;
        background: #2b2b2b;
        padding: 4px 7px;
        font: inherit;
        cursor: pointer;
      }
      button:hover, button:focus-visible { background: #3b3b3b; }
      button[data-active] { border-color: #78b9ff; background: #2a6f9f; }
      .status { margin-top: 6px; color: #c8c8c8; line-height: 1.3; }
      .rows { margin-top: 6px; }
      .row { justify-content: space-between; padding-top: 4px; }
      .name { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      .empty { color: #979797; }
      .clear { padding: 2px 5px; }
    </style>
    <div class="panel" role="region" aria-label="MIDI mapping">
      <div class="toolbar">
        <button class="map" type="button">Map MIDI</button>
        <button class="clear-all" type="button" title="Clear all MIDI mappings">Clear all</button>
      </div>
      <div class="status" aria-live="polite">Ready</div>
      <div class="rows"><span class="empty">No mappings</span></div>
    </div>`;

  const keyboard = document.createElement('div');
  keyboard.id = 'clap-wrapper-standalone-keyboard';
  const keyboardShadow = keyboard.attachShadow({ mode: 'open' });
  keyboardShadow.innerHTML = `
    <style>
      :host {
        position: fixed;
        left: 8px;
        right: 8px;
        bottom: 8px;
        z-index: 2147483646;
        display: none;
        color: #f4f4f4;
        font: 12px -apple-system, BlinkMacSystemFont, sans-serif;
        pointer-events: auto;
      }
      :host([data-visible]) { display: block; }
      .shell {
        padding: 6px;
        border: 1px solid #777;
        border-radius: 6px;
        background: #202020;
        box-shadow: 0 3px 14px rgba(0, 0, 0, .35);
      }
      .label { margin: 0 0 5px 2px; color: #c8c8c8; }
      .keyboard { display: flex; position: relative; height: 92px; }
      button {
        position: relative;
        flex: 1;
        min-width: 14px;
        margin: 0;
        border: 1px solid #555;
        border-radius: 0 0 3px 3px;
        background: #f4f4f4;
        color: #222;
        cursor: pointer;
        touch-action: none;
        user-select: none;
      }
      button:hover, button:focus-visible { background: #d9f4e6; }
      button[data-on] { background: #8ee3b2; }
      button.black {
        position: absolute;
        top: 0;
        height: 59%;
        min-width: 0;
        width: 3.2%;
        border-color: #000;
        border-radius: 0 0 2px 2px;
        background: #111;
        color: #fff;
        transform: translateX(-50%);
        z-index: 2;
      }
      button.black:hover, button.black:focus-visible { background: #3a3a3a; }
      button.black[data-on] { background: #4e9f77; }
    </style>
    <div class="shell" role="region" aria-label="Host keyboard">
      <div class="label">Host keyboard</div>
      <div class="keyboard"></div>
    </div>`;

  const mapButton = shadow.querySelector('.map');
  const clearAllButton = shadow.querySelector('.clear-all');
  const status = shadow.querySelector('.status');
  const rows = shadow.querySelector('.rows');

  const post = command => window.parent.postMessage(
    encoder.encode(`${prefix}${command}`).buffer, '*');
  const keyboardKeys = keyboardShadow.querySelector('.keyboard');
  const noteOn = note => {
    if (!keyboardVisible || activeNotes.has(note)) return;
    activeNotes.add(note);
    keyboardKeys.querySelector(`[data-note="${note}"]`)?.toggleAttribute('data-on', true);
    window.parent.postMessage(encoder.encode(`${notePrefix}on:${note}`).buffer, '*');
  };
  const noteOff = note => {
    if (!activeNotes.delete(note)) return;
    keyboardKeys.querySelector(`[data-note="${note}"]`)?.removeAttribute('data-on');
    window.parent.postMessage(encoder.encode(`${notePrefix}off:${note}`).buffer, '*');
  };
  const releaseAllNotes = () => [...activeNotes].forEach(noteOff);
  const isBlack = note => [1, 3, 6, 8, 10].includes(note % 12);
  const whiteNotes = [];
  for (let note = 48; note <= 72; ++note)
    if (!isBlack(note)) whiteNotes.push(note);
  const whiteIndex = new Map(whiteNotes.map((note, index) => [note, index]));
  for (let note = 48; note <= 72; ++note) {
    const key = document.createElement('button');
    const black = isBlack(note);
    key.className = black ? 'key black' : 'key';
    key.type = 'button';
    key.dataset.note = String(note);
    key.setAttribute('aria-label', `MIDI note ${note}`);
    if (black)
      key.style.left = `${(whiteIndex.get(note - 1) + 1) * 100 / whiteNotes.length}%`;
    key.addEventListener('pointerdown', event => {
      event.preventDefault();
      key.setPointerCapture?.(event.pointerId);
      noteOn(note);
    });
    key.addEventListener('pointerup', event => {
      event.preventDefault();
      noteOff(note);
    });
    key.addEventListener('pointercancel', () => noteOff(note));
    keyboardKeys.append(key);
  }
  const computerKeys = new Map([
    ['a', 48], ['w', 49], ['s', 50], ['e', 51], ['d', 52], ['f', 53],
    ['t', 54], ['g', 55], ['y', 56], ['h', 57], ['u', 58], ['j', 59],
    ['k', 60], ['o', 61], ['l', 62], ['p', 63], [';', 64], ["'", 65]
  ]);
  window.addEventListener('keydown', event => {
    if (!keyboardVisible || event.repeat) return;
    const note = computerKeys.get(event.key.toLowerCase());
    if (note === undefined) return;
    event.preventDefault();
    noteOn(note);
  }, true);
  window.addEventListener('keyup', event => {
    if (!keyboardVisible) return;
    const note = computerKeys.get(event.key.toLowerCase());
    if (note === undefined) return;
    event.preventDefault();
    noteOff(note);
  }, true);
  window.addEventListener('blur', releaseAllNotes);
  const decode = value => {
    if (value instanceof ArrayBuffer)
      return decoder.decode(new Uint8Array(value));
    if (ArrayBuffer.isView(value))
      return decoder.decode(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
    return '';
  };
  const safeDecode = value => {
    try { return decodeURIComponent(value); } catch (_) { return value; }
  };

  const render = () => {
    rows.textContent = '';
    if (!mappings.size) {
      const empty = document.createElement('span');
      empty.className = 'empty';
      empty.textContent = 'No mappings';
      rows.append(empty);
      return;
    }
    for (const [id, mapping] of mappings) {
      const row = document.createElement('div');
      row.className = 'row';
      const name = document.createElement('span');
      name.className = 'name';
      name.textContent = `${mapping.name || `Parameter ${id}`} — CC ${mapping.cc}`;
      name.title = name.textContent;
      const clear = document.createElement('button');
      clear.className = 'clear';
      clear.type = 'button';
      clear.textContent = 'Clear';
      clear.addEventListener('click', () => post(`clear:${id}`));
      row.append(name, clear);
      rows.append(row);
    }
  };

  const setMode = enabled => {
    mappingMode = enabled;
    mapButton.toggleAttribute('data-active', enabled);
    mapButton.textContent = enabled ? 'Cancel MIDI' : 'Map MIDI';
    if (!enabled) target = null;
  };

  mapButton.addEventListener('click', () => post(mappingMode ? 'cancel' : 'begin'));
  clearAllButton.addEventListener('click', () => post('clear-all'));
  window.addEventListener('keydown', event => {
    if (event.key === 'Delete' && mappingMode && target !== null) {
      event.preventDefault();
      event.stopPropagation();
      post(`clear:${target}`);
      return;
    }
    if (event.key === 'Escape' && mappingMode) {
      event.preventDefault();
      post('cancel');
    }
  }, true);
  window.addEventListener('message', event => {
    const text = decode(event.data);
    if (!text.startsWith(prefix)) return;
    const command = text.slice(prefix.length);
    if (command === 'mode:1') {
      setMode(true);
      status.textContent = 'Touch a plug-in control, then move a MIDI CC';
    } else if (command === 'mode:0') {
      setMode(false);
      status.textContent = 'Ready';
    } else if (command.startsWith('target:')) {
      const separator = command.indexOf(':', 7);
      const name = separator < 0 ? '' : safeDecode(command.slice(separator + 1));
      target = command.slice(7, separator < 0 ? command.length : separator);
      status.textContent = `Move a MIDI CC for ${name || `parameter ${target}`}`;
    } else if (command.startsWith('mapped:')) {
      const parts = command.split(':');
      if (parts.length >= 4) {
        mappings.set(parts[1], { cc: parts[2], name: safeDecode(parts.slice(4).join(':')) });
        status.textContent = `Mapped to CC ${parts[2]}`;
        render();
      }
    } else if (command.startsWith('unmapped:')) {
      mappings.delete(command.slice(9));
      render();
    } else if (command === 'keyboard:1' || command === 'keyboard:0') {
      keyboardVisible = command === 'keyboard:1';
      keyboard.toggleAttribute('data-visible', keyboardVisible);
      if (!keyboardVisible) releaseAllNotes();
    }
  });

  const mount = () => {
    const root = document.body || document.documentElement;
    root.append(panel, keyboard);
    render();
    post('ready');
  };
  if (document.body)
    mount();
  else
    document.addEventListener('DOMContentLoaded', mount, { once: true });
})();
)JS";

  struct ResourceStream
  {
    clap_ostream_t stream { this, write };
    std::vector<uint8_t> data;

    static int64_t CLAP_ABI write(const clap_ostream_t *stream, const void *buffer, uint64_t size)
    {
      auto &self = *static_cast<ResourceStream *>(stream->ctx);
      if (size > maximumResourceSize - self.data.size()) return -1;
      const auto *bytes = static_cast<const uint8_t *>(buffer);
      self.data.insert(self.data.end(), bytes, bytes + size);
      return static_cast<int64_t>(size);
    }
  };

  // WebKit hands the scheme handler the URL's normalised path, which drops the
  // trailing slash: navigating to "charclap://ui/ui/" arrives here as "/ui".
  // Treat both spellings as a request for the directory's index document.
  static bool isDirectoryRequest(const std::string &path, const std::string &directory)
  {
    if (path == directory) return true;
    return path.size() + 1 == directory.size() && directory.compare(0, path.size(), path) == 0;
  }

  static bool isAbsoluteURI(const std::string &uri)
  {
    const auto colon = uri.find(':');
    const auto slash = uri.find('/');
    return colon != std::string::npos && (slash == std::string::npos || colon < slash);
  }

  std::optional<choc::ui::WebView::Options::Resource> fetchResource(const std::string &path) const
  {
    char mime[256] {};
    ResourceStream stream;
    if (!withMainThread([&] {
      return webview->get_resource(plugin, path.c_str(), mime, sizeof(mime), &stream.stream);
    }))
      return std::nullopt;

    choc::ui::WebView::Options::Resource resource;
    resource.data = std::move(stream.data);
    resource.mimeType = mime;
    return resource;
  }

  void startSizingGui()
  {
    if (!sizingGui || !sizingGui->is_api_supported ||
        !withMainThread([&] {
          return sizingGui->is_api_supported(plugin, CLAP_WINDOW_API_WEBVIEW, false);
        }) ||
        !withMainThread([&] { return sizingGui->create(plugin, CLAP_WINDOW_API_WEBVIEW, false); }))
    {
      sizingGui = nullptr;
      return;
    }

    sizingGuiCreated = true;
    withMainThread([&] { sizingGui->get_size(plugin, &viewWidth, &viewHeight); });
    if (viewWidth == 0 || viewHeight == 0 || viewWidth > 16384 || viewHeight > 16384)
    {
      viewWidth = 800;
      viewHeight = 500;
    }

    clap_window_t window {};
    window.api = CLAP_WINDOW_API_WEBVIEW;
    window.ptr = nullptr;
    withMainThread([&] { sizingGui->set_parent(plugin, &window); });
    withMainThread([&] { sizingGui->show(plugin); });
  }

  void stopSizingGui()
  {
    if (!sizingGuiCreated) return;
    withMainThread([&] { sizingGui->hide(plugin); });
    withMainThread([&] { sizingGui->destroy(plugin); });
    sizingGuiCreated = false;
  }

  const clap_plugin_t *plugin = nullptr;
  const clap_plugin_webview_t *webview = nullptr;
  const clap_plugin_gui_t *sizingGui = nullptr;
  Clap::Plugin *threadCheckedPlugin = nullptr;
  HostReceive hostReceive;
  std::unique_ptr<choc::ui::WebView> nativeView;
  uint32_t viewWidth = 800;
  uint32_t viewHeight = 500;
  bool sizingGuiCreated = false;
};

}  // namespace freeaudio::clap_wrapper
