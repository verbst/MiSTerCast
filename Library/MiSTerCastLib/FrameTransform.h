#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

enum class Rotation : uint8_t
{
    None,
    CW90,
    CCW90,
    Flip180
};

enum class SamplingMode : uint8_t
{
    Point,
    Bilinear,
    LineBlend
};

inline const char* SamplingModeName(SamplingMode sampling) noexcept
{
    switch (sampling)
    {
    case SamplingMode::Point:
        return "point";
    case SamplingMode::Bilinear:
        return "bilinear";
    case SamplingMode::LineBlend:
        return "line-blend";
    default:
        return "unknown";
    }
}

inline bool UsesInterlacedFieldBuffer(bool interlaced, bool progressiveFramebuffer) noexcept
{
    return interlaced && !progressiveFramebuffer;
}

inline size_t Rgb24FrameSize(
    uint32_t width,
    uint32_t activeHeight,
    bool interlaced,
    bool progressiveFramebuffer = false)
{
    const uint32_t outputHeight = UsesInterlacedFieldBuffer(interlaced, progressiveFramebuffer)
        ? activeHeight / 2
        : activeHeight;
    if (width == 0 || outputHeight == 0 || width > std::numeric_limits<size_t>::max() / outputHeight / 3)
        return 0;

    return static_cast<size_t>(width) * outputHeight * 3;
}

namespace mistercast
{
namespace transform_detail
{
struct LinearSample
{
    uint32_t first = 0;
    uint32_t second = 0;
    uint16_t fraction = 0; // 0..256, with eight fractional bits.
};

struct AreaTables
{
    std::vector<uint32_t> offsets;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> weights;
};

struct AxisMapping
{
    uint32_t extent = 0;
    bool reversed = false;
};

struct TransformMapping
{
    AxisMapping horizontal;
    AxisMapping vertical;
    bool swapped = false;
};

inline TransformMapping BuildTransformMapping(
    uint32_t sourceWidth,
    uint32_t sourceHeight,
    Rotation rotation) noexcept
{
    const AxisMapping x = { sourceWidth, false };
    const AxisMapping reverseX = { sourceWidth, true };
    const AxisMapping y = { sourceHeight, false };
    const AxisMapping reverseY = { sourceHeight, true };
    switch (rotation)
    {
    case Rotation::None:
        return { x, y, false };
    case Rotation::Flip180:
        return { reverseX, reverseY, false };
    case Rotation::CW90:
        return { reverseY, x, true };
    case Rotation::CCW90:
        return { y, reverseX, true };
    default:
        return { x, y, false };
    }
}

inline uint32_t FullOutputRow(uint32_t row, bool fieldBuffer, uint8_t field) noexcept
{
    // Groovy_MiSTer maps protocol field 0 to display field 1 and field 1 to
    // display field 0. Sampling that parity here makes adjacent fields
    // reconstruct the original progressive source frame.
    return fieldBuffer ? row * 2 + !(field & 1) : row;
}

inline void BuildPointTable(
    uint32_t destinationExtent,
    const AxisMapping& source,
    std::vector<uint32_t>& table)
{
    table.resize(destinationExtent);
    for (uint32_t destination = 0; destination < destinationExtent; ++destination)
    {
        const double position =
            (static_cast<double>(destination) + 0.5) / destinationExtent;
        const double sourcePosition = source.reversed ? 1.0 - position : position;
        table[destination] = std::min(
            source.extent - 1,
            static_cast<uint32_t>(sourcePosition * source.extent));
    }
}

inline LinearSample BuildLinearSample(
    uint32_t destination,
    uint32_t destinationExtent,
    const AxisMapping& source) noexcept
{
    // Centered texel coordinate: (d + 0.5) * source / destination - 0.5.
    const int64_t numerator =
        static_cast<int64_t>(2 * static_cast<uint64_t>(destination) + 1) * source.extent -
        destinationExtent;
    const uint64_t denominator = static_cast<uint64_t>(destinationExtent) * 2;
    uint32_t first = 0;
    uint32_t second = 0;
    uint16_t fraction = 0;
    if (numerator > 0)
    {
        first = static_cast<uint32_t>(static_cast<uint64_t>(numerator) / denominator);
        const uint64_t remainder = static_cast<uint64_t>(numerator) % denominator;
        if (first >= source.extent - 1)
        {
            first = second = source.extent - 1;
        }
        else
        {
            second = first + 1;
            fraction = static_cast<uint16_t>(
                (remainder * 256 + denominator / 2) / denominator);
            if (fraction == 256)
            {
                first = second;
                second = std::min(second + 1, source.extent - 1);
                fraction = 0;
            }
        }
    }

    if (source.reversed)
    {
        first = source.extent - 1 - first;
        second = source.extent - 1 - second;
    }
    return { first, second, fraction };
}

inline void BuildAreaTables(
    uint32_t destinationExtent,
    const AxisMapping& source,
    AreaTables& table)
{
    table.offsets.resize(static_cast<size_t>(destinationExtent) + 1);
    table.indices.clear();
    table.weights.clear();

    // Source and destination boundaries use a grid whose unit is
    // 1/destinationExtent. Every output row's weights therefore sum exactly
    // to source.extent for both reduction and enlargement.
    for (uint32_t destination = 0; destination < destinationExtent; ++destination)
    {
        table.offsets[destination] = static_cast<uint32_t>(table.indices.size());
        const uint64_t begin = static_cast<uint64_t>(destination) * source.extent;
        const uint64_t end = static_cast<uint64_t>(destination + 1) * source.extent;
        const uint32_t first = static_cast<uint32_t>(begin / destinationExtent);
        const uint32_t last = static_cast<uint32_t>((end - 1) / destinationExtent);
        for (uint32_t sourceIndex = first; sourceIndex <= last; ++sourceIndex)
        {
            const uint64_t pixelBegin = static_cast<uint64_t>(sourceIndex) * destinationExtent;
            const uint64_t pixelEnd = static_cast<uint64_t>(sourceIndex + 1) * destinationExtent;
            const uint64_t overlap =
                std::min(end, pixelEnd) - std::max(begin, pixelBegin);
            table.indices.push_back(source.reversed
                ? source.extent - 1 - sourceIndex
                : sourceIndex);
            table.weights.push_back(static_cast<uint32_t>(overlap));
        }
    }
    table.offsets[destinationExtent] = static_cast<uint32_t>(table.indices.size());
}

inline void TransformPoint(
    const uint8_t* source,
    uint32_t sourceStride,
    uint32_t outputWidth,
    uint32_t activeHeight,
    bool fieldBuffer,
    uint8_t field,
    uint32_t outputHeight,
    const TransformMapping& mapping,
    uint8_t* destination)
{
    thread_local std::vector<uint32_t> columns;
    thread_local std::vector<uint32_t> rows;
    BuildPointTable(outputWidth, mapping.horizontal, columns);
    BuildPointTable(activeHeight, mapping.vertical, rows);

    for (uint32_t y = 0; y < outputHeight; ++y)
    {
        const uint32_t fullY = FullOutputRow(y, fieldBuffer, field);
        if (mapping.swapped)
        {
            const size_t column = static_cast<size_t>(rows[fullY]) * 4;
            for (uint32_t x = 0; x < outputWidth; ++x, destination += 3)
            {
                const uint8_t* pixel =
                    source + static_cast<size_t>(columns[x]) * sourceStride + column;
                destination[0] = pixel[0];
                destination[1] = pixel[1];
                destination[2] = pixel[2];
            }
        }
        else
        {
            const uint8_t* row = source + static_cast<size_t>(rows[fullY]) * sourceStride;
            for (uint32_t x = 0; x < outputWidth; ++x, destination += 3)
            {
                const uint8_t* pixel = row + static_cast<size_t>(columns[x]) * 4;
                destination[0] = pixel[0];
                destination[1] = pixel[1];
                destination[2] = pixel[2];
            }
        }
    }
}

inline void TransformBilinear(
    const uint8_t* source,
    uint32_t sourceStride,
    uint32_t outputWidth,
    uint32_t activeHeight,
    bool fieldBuffer,
    uint8_t field,
    uint32_t outputHeight,
    const TransformMapping& mapping,
    uint8_t* destination)
{
    thread_local std::vector<LinearSample> columns;
    thread_local std::vector<LinearSample> rows;
    columns.resize(outputWidth);
    rows.resize(activeHeight);
    for (uint32_t x = 0; x < outputWidth; ++x)
        columns[x] = BuildLinearSample(x, outputWidth, mapping.horizontal);
    for (uint32_t y = 0; y < activeHeight; ++y)
        rows[y] = BuildLinearSample(y, activeHeight, mapping.vertical);

    const auto pixel = [source, sourceStride, &mapping](uint32_t horizontal, uint32_t vertical)
    {
        const uint32_t sourceX = mapping.swapped ? vertical : horizontal;
        const uint32_t sourceY = mapping.swapped ? horizontal : vertical;
        return source + static_cast<size_t>(sourceY) * sourceStride +
            static_cast<size_t>(sourceX) * 4;
    };

    for (uint32_t y = 0; y < outputHeight; ++y)
    {
        const LinearSample& vertical = rows[FullOutputRow(y, fieldBuffer, field)];
        const uint32_t wy1 = vertical.fraction;
        const uint32_t wy0 = 256 - wy1;
        for (uint32_t x = 0; x < outputWidth; ++x, destination += 3)
        {
            const LinearSample& horizontal = columns[x];
            const uint32_t wx1 = horizontal.fraction;
            const uint32_t wx0 = 256 - wx1;
            const uint8_t* p00 = pixel(horizontal.first, vertical.first);
            const uint8_t* p10 = pixel(horizontal.second, vertical.first);
            const uint8_t* p01 = pixel(horizontal.first, vertical.second);
            const uint8_t* p11 = pixel(horizontal.second, vertical.second);
            for (unsigned int channel = 0; channel < 3; ++channel)
            {
                const uint32_t sum =
                    static_cast<uint32_t>(p00[channel]) * wx0 * wy0 +
                    static_cast<uint32_t>(p10[channel]) * wx1 * wy0 +
                    static_cast<uint32_t>(p01[channel]) * wx0 * wy1 +
                    static_cast<uint32_t>(p11[channel]) * wx1 * wy1;
                destination[channel] = static_cast<uint8_t>((sum + 32768) >> 16);
            }
        }
    }
}

inline void TransformLineBlend(
    const uint8_t* source,
    uint32_t sourceStride,
    uint32_t outputWidth,
    uint32_t activeHeight,
    bool fieldBuffer,
    uint8_t field,
    uint32_t outputHeight,
    const TransformMapping& mapping,
    uint8_t* destination)
{
    thread_local std::vector<uint32_t> columns;
    thread_local AreaTables rows;
    thread_local std::vector<size_t> rowOffsets;
    BuildPointTable(outputWidth, mapping.horizontal, columns);
    BuildAreaTables(activeHeight, mapping.vertical, rows);
    if (!mapping.swapped)
    {
        rowOffsets.resize(rows.indices.size());
        for (size_t index = 0; index < rows.indices.size(); ++index)
            rowOffsets[index] = static_cast<size_t>(rows.indices[index]) * sourceStride;
    }

    const uint32_t verticalExtent = mapping.vertical.extent;
    const uint64_t reciprocal = (uint64_t{ 1 } << 32) / verticalExtent;
    const auto normalize = [verticalExtent, reciprocal](uint32_t sum)
    {
        const uint32_t rounded = sum + verticalExtent / 2;
        uint32_t quotient = static_cast<uint32_t>(
            (static_cast<uint64_t>(rounded) * reciprocal) >> 32);
        if (rounded - quotient * verticalExtent >= verticalExtent)
            ++quotient;
        return static_cast<uint8_t>(quotient);
    };

    for (uint32_t y = 0; y < outputHeight; ++y)
    {
        const uint32_t fullY = FullOutputRow(y, fieldBuffer, field);
        if (!mapping.swapped)
        {
            for (uint32_t x = 0; x < outputWidth; ++x, destination += 3)
            {
                uint32_t blue = 0;
                uint32_t green = 0;
                uint32_t red = 0;
                const size_t column = static_cast<size_t>(columns[x]) * 4;
                for (uint32_t index = rows.offsets[fullY];
                    index < rows.offsets[fullY + 1]; ++index)
                {
                    const uint8_t* pixel = source + rowOffsets[index] + column;
                    blue += static_cast<uint32_t>(pixel[0]) * rows.weights[index];
                    green += static_cast<uint32_t>(pixel[1]) * rows.weights[index];
                    red += static_cast<uint32_t>(pixel[2]) * rows.weights[index];
                }
                destination[0] = normalize(blue);
                destination[1] = normalize(green);
                destination[2] = normalize(red);
            }
            continue;
        }

        for (uint32_t x = 0; x < outputWidth; ++x, destination += 3)
        {
            uint32_t blue = 0;
            uint32_t green = 0;
            uint32_t red = 0;
            for (uint32_t index = rows.offsets[fullY];
                index < rows.offsets[fullY + 1]; ++index)
            {
                const uint32_t sourceX = rows.indices[index];
                const uint32_t sourceY = columns[x];
                const uint8_t* pixel = source +
                    static_cast<size_t>(sourceY) * sourceStride +
                    static_cast<size_t>(sourceX) * 4;
                blue += static_cast<uint32_t>(pixel[0]) * rows.weights[index];
                green += static_cast<uint32_t>(pixel[1]) * rows.weights[index];
                red += static_cast<uint32_t>(pixel[2]) * rows.weights[index];
            }
            destination[0] = normalize(blue);
            destination[1] = normalize(green);
            destination[2] = normalize(red);
        }
    }
}
}
}

inline bool TransformBgraToRgb24(
    const uint8_t* source,
    uint32_t sourceWidth,
    uint32_t sourceHeight,
    uint32_t sourceStride,
    uint32_t outputWidth,
    uint32_t activeHeight,
    bool interlaced,
    bool progressiveFramebuffer,
    uint8_t field,
    Rotation rotation,
    SamplingMode sampling,
    uint8_t* destination,
    size_t destinationSize)
{
    const bool fieldBuffer = UsesInterlacedFieldBuffer(interlaced, progressiveFramebuffer);
    const size_t requiredSize = Rgb24FrameSize(
        outputWidth, activeHeight, interlaced, progressiveFramebuffer);
    if (source == nullptr || destination == nullptr || sourceWidth == 0 || sourceHeight == 0 ||
        sourceStride < static_cast<size_t>(sourceWidth) * 4 ||
        requiredSize == 0 || destinationSize < requiredSize)
    {
        return false;
    }

    const uint32_t outputHeight = fieldBuffer ? activeHeight / 2 : activeHeight;
    const mistercast::transform_detail::TransformMapping mapping =
        mistercast::transform_detail::BuildTransformMapping(
            sourceWidth, sourceHeight, rotation);
    switch (sampling)
    {
    case SamplingMode::Point:
        mistercast::transform_detail::TransformPoint(
            source, sourceStride, outputWidth, activeHeight, fieldBuffer,
            field, outputHeight, mapping, destination);
        return true;
    case SamplingMode::Bilinear:
        mistercast::transform_detail::TransformBilinear(
            source, sourceStride, outputWidth, activeHeight, fieldBuffer,
            field, outputHeight, mapping, destination);
        return true;
    case SamplingMode::LineBlend:
        mistercast::transform_detail::TransformLineBlend(
            source, sourceStride, outputWidth, activeHeight, fieldBuffer,
            field, outputHeight, mapping, destination);
        return true;
    default:
        return false;
    }
}
