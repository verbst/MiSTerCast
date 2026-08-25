#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace mistercast
{
// CmdAudio takes its byte count in a uint16, so one drain must stay well under
// 65535 bytes: a larger backlog truncates, and an exact multiple of 65536
// puts an empty CMD_AUDIO on the wire that the core rejects with UDP_ERROR.
// That is not hypothetical: the WASAPI loopback buffer is a full second deep,
// so any stall (notably the gap between the stream starting and the first
// video frame) can build one. 8192 values (4096 stereo frames, 16 KB) is
// ~85 ms at 48 kHz stereo; steady state is ~3.2 KB/frame.
constexpr size_t MaxAudioValuesPerCommand = 8192;

inline int16_t FloatToPcm16(float sample) noexcept
{
    if (!std::isfinite(sample))
        return 0;
    if (sample <= -1.0f)
        return std::numeric_limits<int16_t>::min();
    if (sample >= 1.0f)
        return std::numeric_limits<int16_t>::max();
    return static_cast<int16_t>(std::lround(sample * std::numeric_limits<int16_t>::max()));
}

inline bool ConvertFloatFramesToStereo(
    const float* source,
    size_t frames,
    uint16_t channels,
    int16_t* destination,
    size_t destinationValues) noexcept
{
    if (source == nullptr || destination == nullptr || channels == 0 ||
        frames > std::numeric_limits<size_t>::max() / 2 || destinationValues < frames * 2)
        return false;

    for (size_t frame = 0; frame < frames; ++frame)
    {
        const float* input = source + frame * channels;
        destination[frame * 2] = FloatToPcm16(input[0]);
        destination[frame * 2 + 1] = FloatToPcm16(input[channels > 1 ? 1 : 0]);
    }
    return true;
}
}
