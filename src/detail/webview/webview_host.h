#pragma once

#include <clap/clap.h>
#include <clap/ext/draft/webview.h>

#include <choc/gui/choc_WebView.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace freeaudio::clap_wrapper
{

class WebViewHost
{
public:
  WebViewHost(const clap_plugin_t *plugin, const clap_plugin_webview_t *webview,
              const clap_plugin_gui_t *sizingGui)
      : plugin(plugin), webview(webview), sizingGui(sizingGui)
  {
    if (!plugin || !webview || !webview->get_uri || !webview->get_resource || !webview->receive)
      return;

    const auto uriSize = webview->get_uri(plugin, nullptr, 0);
    if (uriSize <= 1) return;

    std::string uri(static_cast<size_t>(uriSize), '\0');
    if (webview->get_uri(plugin, uri.data(), static_cast<uint32_t>(uri.size())) != uriSize) return;
    uri.resize(static_cast<size_t>(uriSize - 1));

    startSizingGui();

    choc::ui::WebView::Options options;
    auto navigationURI = uri;
    if (!isAbsoluteURI(uri))
    {
      options.customSchemeURI = "clap-plugin://ui";
      options.fetchResource = [this](const std::string &path) { return fetchResource(path); };
      navigationURI = options.customSchemeURI + uri;
    }

    options.webviewIsReady = [this, navigationURI](choc::ui::WebView &view) {
      view.bind("clapHostPostMessage", [this](const choc::value::ValueView &arguments) {
        if (!this->webview || !this->webview->receive || !arguments.isArray() ||
            arguments.size() != 1 ||
            !arguments[0].isArray())
          return choc::value::Value(false);

        const auto source = arguments[0];
        std::vector<uint8_t> bytes(source.size());
        for (uint32_t i = 0; i < source.size(); ++i)
          bytes[i] = static_cast<uint8_t>(source[i].getWithDefault<int32_t>(0));

        return choc::value::Value(this->webview->receive(
            this->plugin, bytes.data(), static_cast<uint32_t>(bytes.size())));
      });
      view.addInitScript(messageBridgeScript);
      view.navigate(navigationURI);
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
  [[nodiscard]] void *viewHandle() const noexcept
  {
    return nativeView ? nativeView->getViewHandle() : nullptr;
  }

  [[nodiscard]] uint32_t width() const noexcept { return viewWidth; }
  [[nodiscard]] uint32_t height() const noexcept { return viewHeight; }
  [[nodiscard]] bool canResize() const noexcept { return sizingGui && sizingGui->can_resize(plugin); }

  bool setSize(uint32_t width, uint32_t height)
  {
    if (sizingGui && !sizingGui->set_size(plugin, width, height)) return false;
    viewWidth = width;
    viewHeight = height;
    return true;
  }

  bool adjustSize(uint32_t &width, uint32_t &height) const
  {
    return !sizingGui || !sizingGui->adjust_size || sizingGui->adjust_size(plugin, &width, &height);
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
    if (!webview->get_resource(plugin, path.c_str(), mime, sizeof(mime), &stream.stream))
      return std::nullopt;

    choc::ui::WebView::Options::Resource resource;
    resource.data = std::move(stream.data);
    resource.mimeType = mime;
    return resource;
  }

  void startSizingGui()
  {
    if (!sizingGui || !sizingGui->is_api_supported ||
        !sizingGui->is_api_supported(plugin, CLAP_WINDOW_API_WEBVIEW, false) ||
        !sizingGui->create(plugin, CLAP_WINDOW_API_WEBVIEW, false))
    {
      sizingGui = nullptr;
      return;
    }

    sizingGuiCreated = true;
    sizingGui->get_size(plugin, &viewWidth, &viewHeight);
    if (viewWidth == 0 || viewHeight == 0 || viewWidth > 16384 || viewHeight > 16384)
    {
      viewWidth = 800;
      viewHeight = 500;
    }

    clap_window_t window {};
    window.api = CLAP_WINDOW_API_WEBVIEW;
    window.ptr = nullptr;
    sizingGui->set_parent(plugin, &window);
    sizingGui->show(plugin);
  }

  void stopSizingGui()
  {
    if (!sizingGuiCreated) return;
    sizingGui->hide(plugin);
    sizingGui->destroy(plugin);
    sizingGuiCreated = false;
  }

  const clap_plugin_t *plugin = nullptr;
  const clap_plugin_webview_t *webview = nullptr;
  const clap_plugin_gui_t *sizingGui = nullptr;
  std::unique_ptr<choc::ui::WebView> nativeView;
  uint32_t viewWidth = 800;
  uint32_t viewHeight = 500;
  bool sizingGuiCreated = false;
};

}  // namespace freeaudio::clap_wrapper
