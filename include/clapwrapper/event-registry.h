#pragma once

#include <clap/ext/event-registry.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Clap
{

// This registry is only used while a plugin is initialized on the main thread.
// Its IDs remain valid for the Plugin instance lifetime.
class EventRegistry
{
 public:
  bool query(const char *spaceName, uint16_t *spaceId)
  {
    if (spaceId == nullptr || spaceName == nullptr || *spaceName == '\0')
    {
      if (spaceId != nullptr) *spaceId = UINT16_MAX;
      return false;
    }

    for (uint16_t i = 0; i < _names.size(); ++i)
    {
      if (_names[i] == spaceName)
      {
        *spaceId = static_cast<uint16_t>(i + 1);
        return true;
      }
    }

    if (_names.size() >= UINT16_MAX - 1)
    {
      *spaceId = UINT16_MAX;
      return false;
    }

    _names.emplace_back(spaceName);
    *spaceId = static_cast<uint16_t>(_names.size());
    return true;
  }

 private:
  std::vector<std::string> _names;
};

}  // namespace Clap
