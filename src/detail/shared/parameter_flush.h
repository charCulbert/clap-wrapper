#pragma once

#include <atomic>
#include <mutex>
#include <utility>
#include <clap/clap.h>

namespace ClapWrapper::detail::shared
{

// Main/idle coordination only. The steady audio process path must never acquire this mutex.
class ParameterFlushLifecycle
{
 public:
  bool requiresAudioThread() const noexcept
  {
    return _requiresAudioThread.load(std::memory_order_acquire);
  }

  template <typename Callback>
  bool serviceIfInactive(Callback &&callback)
  {
    if (requiresAudioThread()) return false;

    std::lock_guard<std::mutex> lock(_mutex);
    if (requiresAudioThread()) return false;

    std::forward<Callback>(callback)();
    return true;
  }

  template <typename Callback>
  bool activate(Callback &&callback)
  {
    _requiresAudioThread.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(_mutex);

    if (std::forward<Callback>(callback)()) return true;

    _requiresAudioThread.store(false, std::memory_order_release);
    return false;
  }

  template <typename Activate, typename Start, typename Rollback>
  bool activateAndStart(Activate &&activateCallback, Start &&startCallback,
                        Rollback &&rollbackCallback)
  {
    return activate(
        [&]
        {
          if (!std::forward<Activate>(activateCallback)()) return false;
          if (std::forward<Start>(startCallback)()) return true;

          std::forward<Rollback>(rollbackCallback)();
          return false;
        });
  }

  template <typename Callback>
  void deactivate(Callback &&callback)
  {
    _requiresAudioThread.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(_mutex);
    std::forward<Callback>(callback)();
    _requiresAudioThread.store(false, std::memory_order_release);
  }

 private:
  std::atomic_bool _requiresAudioThread{false};
  std::mutex _mutex;
};

inline bool serviceParameterFlushRequest(const clap_plugin_t *plugin,
                                         const clap_plugin_params_t *params,
                                         std::atomic_bool *requestFlag,
                                         const clap_output_events_t *output)
{
  if (plugin == nullptr || params == nullptr || params->flush == nullptr || requestFlag == nullptr ||
      output == nullptr || output->try_push == nullptr ||
      !requestFlag->exchange(false, std::memory_order_acq_rel))
    return false;

  clap_input_events_t emptyInput = {};
  emptyInput.size = [](const clap_input_events_t *) -> uint32_t { return 0; };
  emptyInput.get = [](const clap_input_events_t *, uint32_t) -> const clap_event_header_t *
  { return nullptr; };

  struct ForwardContext
  {
    const clap_output_events_t *target;
    bool rejected = false;
  } context{output};

  clap_output_events_t forwardingOutput = {};
  forwardingOutput.ctx = &context;
  forwardingOutput.try_push = [](const clap_output_events_t *list,
                                 const clap_event_header_t *event) -> bool
  {
    auto *forward = static_cast<ForwardContext *>(list->ctx);
    const bool accepted = forward->target->try_push(forward->target, event);
    forward->rejected |= !accepted;
    return accepted;
  };

  params->flush(plugin, &emptyInput, &forwardingOutput);
  if (context.rejected) requestFlag->store(true, std::memory_order_release);
  return true;
}

}  // namespace ClapWrapper::detail::shared
