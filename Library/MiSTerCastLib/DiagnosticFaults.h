#pragma once

#include <cstdint>

namespace mistercast
{
struct StreamFaultOptions
{
    uint32_t skipEvery = 0;
    uint32_t stallEvery = 0;
    uint32_t stallMilliseconds = 0;
};

struct StreamFaultAction
{
    uint64_t attempt = 0;
    bool skip = false;
    uint32_t stallMilliseconds = 0;
};

class StreamFaultSchedule
{
public:
    explicit StreamFaultSchedule(StreamFaultOptions options = {}) noexcept
        : m_options(options)
    {
    }

    StreamFaultAction Next() noexcept
    {
        StreamFaultAction action = {};
        action.attempt = ++m_attempt;
        action.skip = m_options.skipEvery != 0 && action.attempt % m_options.skipEvery == 0;
        if (m_options.stallEvery != 0 && action.attempt % m_options.stallEvery == 0)
            action.stallMilliseconds = m_options.stallMilliseconds;
        return action;
    }

private:
    StreamFaultOptions m_options;
    uint64_t m_attempt = 0;
};
}
