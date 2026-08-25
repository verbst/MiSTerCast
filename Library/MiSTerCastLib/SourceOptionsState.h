#pragma once

#include "MiSTerCastLib.h"

#include <mutex>

class SourceOptionsState
{
public:
    SourceOptions snapshot() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_value;
    }

    void publish(const SourceOptions& value)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_value = value;
    }

private:
    mutable std::mutex m_mutex;
    SourceOptions m_value = {};
};
