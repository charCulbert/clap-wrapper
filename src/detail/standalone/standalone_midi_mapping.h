#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <clap/ext/params.h>

namespace freeaudio::clap_wrapper::standalone::detail
{

class StandaloneMidiMappingTable
{
public:
    static constexpr uint32_t channelCount = 16;
    static constexpr uint32_t ccCount = 128;
    static constexpr uint32_t invalidParamId = UINT32_MAX;

    struct Value
    {
        uint32_t paramId = invalidParamId;
        double value = 0.0;
    };

    bool setMapping(int32_t channel, uint32_t cc, uint32_t paramId,
                    double min, double max, uint32_t flags,
                    uint64_t effectiveBlock) noexcept
    {
        if (!validSlot(channel, cc) || paramId == invalidParamId ||
            !std::isfinite(min) || !std::isfinite(max) || max < min)
            return false;

        auto& slot = slotFor(channel, cc);
        slot.paramId.store(invalidParamId, std::memory_order_release);
        slot.minBits.store(toBits(min), std::memory_order_relaxed);
        slot.maxBits.store(toBits(max), std::memory_order_relaxed);
        slot.flags.store(flags, std::memory_order_relaxed);
        slot.effectiveBlock.store(effectiveBlock, std::memory_order_relaxed);
        slot.paramId.store(paramId, std::memory_order_release);
        return true;
    }

    bool clearMapping(int32_t channel, uint32_t cc) noexcept
    {
        if (!validSlot(channel, cc)) return false;
        slotFor(channel, cc).paramId.store(invalidParamId, std::memory_order_release);
        return true;
    }

    bool valueFor(const uint8_t* data, uint32_t size, uint64_t block,
                  Value& result) const noexcept
    {
        if (data == nullptr || size != 3 || (data[0] & 0xf0u) != 0xb0u)
            return false;

        const auto channel = static_cast<int32_t>(data[0] & 0x0fu);
        const auto cc = static_cast<uint32_t>(data[1]);
        const Slot* slot = &channelMappings[static_cast<size_t>(channel) * ccCount + cc];
        auto paramId = activeParamId(*slot, block);
        if (paramId == invalidParamId)
        {
            slot = &omniMappings[cc];
            paramId = activeParamId(*slot, block);
        }
        if (paramId == invalidParamId) return false;

        const auto min = fromBits(slot->minBits.load(std::memory_order_relaxed));
        const auto max = fromBits(slot->maxBits.load(std::memory_order_relaxed));
        auto value = min + (max - min) * (static_cast<double>(data[2]) / 127.0);
        if ((slot->flags.load(std::memory_order_relaxed) & CLAP_PARAM_IS_STEPPED) != 0)
            value = std::round(value);

        result.paramId = paramId;
        result.value = value;
        return true;
    }

private:
    struct Slot
    {
        std::atomic<uint32_t> paramId{invalidParamId};
        std::atomic<uint64_t> minBits{0};
        std::atomic<uint64_t> maxBits{0};
        std::atomic<uint32_t> flags{0};
        std::atomic<uint64_t> effectiveBlock{0};
    };

    static bool validSlot(int32_t channel, uint32_t cc) noexcept
    {
        return cc < ccCount && (channel == -1 || (channel >= 0 && channel < static_cast<int32_t>(channelCount)));
    }

    Slot& slotFor(int32_t channel, uint32_t cc) noexcept
    {
        return channel == -1 ? omniMappings[cc]
                             : channelMappings[static_cast<size_t>(channel) * ccCount + cc];
    }

    const Slot& slotFor(int32_t channel, uint32_t cc) const noexcept
    {
        return channel == -1 ? omniMappings[cc]
                             : channelMappings[static_cast<size_t>(channel) * ccCount + cc];
    }

    static uint32_t activeParamId(const Slot& slot, uint64_t block) noexcept
    {
        const auto paramId = slot.paramId.load(std::memory_order_acquire);
        if (paramId == invalidParamId || block < slot.effectiveBlock.load(std::memory_order_relaxed))
            return invalidParamId;
        return paramId;
    }

    static uint64_t toBits(double value) noexcept
    {
        uint64_t bits{};
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    static double fromBits(uint64_t bits) noexcept
    {
        double value{};
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    std::array<Slot, channelCount * ccCount> channelMappings{};
    std::array<Slot, ccCount> omniMappings{};
};

} // namespace freeaudio::clap_wrapper::standalone::detail
