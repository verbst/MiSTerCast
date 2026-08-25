#pragma once

#include <cstdint>

namespace mistercast
{
struct StagingTextureSpec
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
};

inline bool SameStagingTexture(
    const StagingTextureSpec& left,
    const StagingTextureSpec& right) noexcept
{
    return left.width == right.width &&
        left.height == right.height &&
        left.format == right.format;
}
}
