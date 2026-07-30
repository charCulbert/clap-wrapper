#pragma once

#include <atomic>
#include <clap/clap.h>

namespace ClapWrapper::detail::shared
{

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
