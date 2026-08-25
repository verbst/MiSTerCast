#pragma once

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <limits>

namespace mistercast
{
constexpr uint64_t HundredNanosecondsPerSecond = 10000000;
constexpr size_t MaxStreamBufferBytes = 1245312;

inline bool IsValidStreamModeline(
    double pixelClockMHz,
    uint16_t horizontalActive,
    uint16_t horizontalBegin,
    uint16_t horizontalEnd,
    uint16_t horizontalTotal,
    uint16_t verticalActive,
    uint16_t verticalBegin,
    uint16_t verticalEnd,
    uint16_t verticalTotal,
    bool interlaced,
    bool progressiveFramebuffer = false) noexcept
{
    if (!std::isfinite(pixelClockMHz) || pixelClockMHz <= 0.0 ||
        horizontalActive == 0 || verticalActive == 0 ||
        horizontalActive > horizontalBegin || horizontalBegin > horizontalEnd ||
        horizontalEnd > horizontalTotal ||
        verticalActive > verticalBegin || verticalBegin > verticalEnd ||
        verticalEnd > verticalTotal ||
        (interlaced && (verticalActive & 1) != 0) ||
        (progressiveFramebuffer && !interlaced))
    {
        return false;
    }

    const uint64_t outputLines = interlaced && !progressiveFramebuffer
        ? verticalActive / 2
        : verticalActive;
    const uint64_t frameBytes = static_cast<uint64_t>(horizontalActive) * outputLines * 3;
    return frameBytes <= MaxStreamBufferBytes;
}

inline uint8_t ProtocolInterlaceMode(bool interlaced, bool progressiveFramebuffer) noexcept
{
    if (!interlaced)
        return 0;
    return progressiveFramebuffer ? 2 : 1;
}

inline uint32_t CounterTicksTo100ns(uint64_t ticks, uint64_t ticksPerSecond) noexcept
{
    if (ticksPerSecond == 0)
        return 0;

    const uint64_t seconds = ticks / ticksPerSecond;
    const uint64_t remainder = ticks % ticksPerSecond;
    if (seconds >= std::numeric_limits<uint32_t>::max() / HundredNanosecondsPerSecond)
        return std::numeric_limits<uint32_t>::max();

    const uint64_t whole = seconds * HundredNanosecondsPerSecond;
    const uint64_t fraction = static_cast<uint64_t>(
        static_cast<long double>(remainder) * HundredNanosecondsPerSecond / ticksPerSecond);
    const uint64_t result = whole + fraction;
    return result > std::numeric_limits<uint32_t>::max()
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(result);
}

inline double LinePeriodMilliseconds(double pixelClockMHz, uint16_t horizontalTotal) noexcept
{
    if (!std::isfinite(pixelClockMHz) || pixelClockMHz <= 0.0 || horizontalTotal == 0)
        return 0.0;
    return horizontalTotal / (pixelClockMHz * 1000.0);
}

inline double FieldPeriodMilliseconds(
    double pixelClockMHz,
    uint16_t horizontalTotal,
    uint16_t verticalTotal,
    bool interlaced) noexcept
{
    const double linePeriod = LinePeriodMilliseconds(pixelClockMHz, horizontalTotal);
    if (linePeriod == 0.0 || verticalTotal == 0)
        return 0.0;
    return linePeriod * verticalTotal / (interlaced ? 2.0 : 1.0);
}

// Target wire rate for pacing a UDP video payload burst release. Kept just
// under a gigabit link so a source NIC faster than the MiSTer's (e.g. a 2.5G
// adapter through a switch down to the MiSTer's 1G port) cannot overrun the
// switch's egress buffer toward the MiSTer. That overrun silently drops the
// tail of the burst; the FPGA then sees an incomplete field and, on
// interlaced modes, locks into its fallback framebuffer - visible as the
// picture periodically freezing/jumping. Ported from the equivalent fix in
// MiSTerCast-Linux (StreamTimingPolicy), verified there on a 2.5G -> switch
// -> 1G bench at 720x576i: 0 fallback frames across 68k+ frames, versus
// failing within ~4100 frames unpaced.
constexpr uint64_t PacingBitsPerSecond = 950'000'000;
// Packets released between pacing gates, and the per-packet wire overhead
// (Ethernet + IP + UDP headers) used to convert a packet count to wire bits.
// A payload with this many packets or fewer (audio, small/highly-compressed
// video fields) is sent unpaced in one commit.
constexpr size_t PacingBurstPackets = 32;
constexpr uint32_t PacingPacketWireOverheadBytes = 66;

// Nanoseconds after the start of a paced send at which `wireBytesReleased`
// bytes should have gone out at the target rate. Pure integer arithmetic.
inline uint64_t PacingReleaseOffsetNanoseconds(
    uint64_t wireBytesReleased,
    uint64_t bitsPerSecond = PacingBitsPerSecond) noexcept
{
    if (bitsPerSecond == 0)
        return 0;
    return wireBytesReleased * 8ULL * 1'000'000'000ULL / bitsPerSecond;
}

inline uint16_t RequestedSyncLine(
    uint16_t verticalTotal,
    double frameDelay,
    bool interlaced,
    bool automaticFrameDelay) noexcept
{
    if (verticalTotal == 0)
        return 0;

    const double safeDelay = std::isfinite(frameDelay)
        ? std::max(0.0, std::min(frameDelay, 1.0))
        : 0.0;
    const uint32_t requested = static_cast<uint32_t>(verticalTotal * safeDelay) + 1;
    const uint16_t latestSafeLine = interlaced && automaticFrameDelay
        ? std::max<uint16_t>(1, verticalTotal >> 1)
        : verticalTotal;
    return static_cast<uint16_t>(std::min<uint32_t>(requested, latestSafeLine));
}
}
