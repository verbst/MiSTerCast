#pragma once

#include <cstddef>
#include <cstdint>

namespace mistercast
{
namespace protocol
{
constexpr uint8_t CommandBlitFieldVsync = 7;
constexpr size_t BlitHeaderSize = 8;
constexpr size_t FpgaStatusSize = 13;

template <typename T>
inline T ReadLittleEndian(const uint8_t* source) noexcept
{
    T value = 0;
    for (size_t index = 0; index < sizeof(T); ++index)
        value |= static_cast<T>(source[index]) << (index * 8);
    return value;
}

template <typename T>
inline void WriteLittleEndian(uint8_t* destination, T value) noexcept
{
    for (size_t index = 0; index < sizeof(T); ++index)
        destination[index] = static_cast<uint8_t>(value >> (index * 8));
}

inline bool EncodeBlitHeader(
    uint8_t* destination,
    size_t capacity,
    uint32_t frame,
    uint8_t field,
    uint16_t syncLine) noexcept
{
    if (destination == nullptr || capacity < BlitHeaderSize)
        return false;

    destination[0] = CommandBlitFieldVsync;
    WriteLittleEndian(destination + 1, frame);
    destination[5] = field;
    WriteLittleEndian(destination + 6, syncLine);
    return true;
}

struct DecodedFpgaStatus
{
    uint32_t frameEcho;
    uint16_t vCountEcho;
    uint32_t frame;
    uint16_t vCount;
    uint8_t bits;
};

inline bool DecodeFpgaStatus(
    const uint8_t* packet,
    size_t packetSize,
    DecodedFpgaStatus& status) noexcept
{
    if (packet == nullptr || packetSize != FpgaStatusSize)
        return false;

    status.frameEcho = ReadLittleEndian<uint32_t>(packet);
    status.vCountEcho = ReadLittleEndian<uint16_t>(packet + 4);
    status.frame = ReadLittleEndian<uint32_t>(packet + 6);
    status.vCount = ReadLittleEndian<uint16_t>(packet + 10);
    status.bits = packet[12];
    return true;
}

// Frame counters are serial numbers. Signed modular subtraction keeps their
// ordering valid when the uint32 counter wraps from UINT32_MAX to zero.
inline bool FrameAfter(uint32_t candidate, uint32_t current) noexcept
{
    const uint32_t distance = candidate - current;
    return distance != 0 && distance < 0x80000000u;
}

inline bool ShouldAcceptStatus(
    bool haveStatus,
    uint32_t currentFrameEcho,
    uint32_t candidateFrameEcho) noexcept
{
    return !haveStatus || candidateFrameEcho == currentFrameEcho ||
        FrameAfter(candidateFrameEcho, currentFrameEcho);
}
}
}
