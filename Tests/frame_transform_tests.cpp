#include "FrameTransform.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{
int failures = 0;

void Check(bool condition, const char* expression, int line)
{
    if (!condition)
    {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

std::vector<uint8_t> GrayFrame(uint32_t width, uint32_t height, const std::vector<uint8_t>& values)
{
    std::vector<uint8_t> frame(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < values.size(); ++i)
    {
        frame[i * 4] = values[i];
        frame[i * 4 + 1] = values[i];
        frame[i * 4 + 2] = values[i];
        frame[i * 4 + 3] = 255;
    }
    return frame;
}

std::vector<uint8_t> PerPixel(const std::vector<uint8_t>& rgb)
{
    std::vector<uint8_t> pixels;
    for (size_t i = 0; i < rgb.size(); i += 3)
        pixels.push_back(rgb[i]);
    return pixels;
}

void CheckRotation(Rotation rotation, uint32_t outputWidth, uint32_t outputHeight, const std::vector<uint8_t>& expected)
{
    const auto frame = GrayFrame(2, 3, {0, 1, 2, 3, 4, 5});
    for (const SamplingMode sampling : {
        SamplingMode::Point, SamplingMode::Bilinear, SamplingMode::LineBlend })
    {
        std::vector<uint8_t> rgb(Rgb24FrameSize(outputWidth, outputHeight, false));
        CHECK(TransformBgraToRgb24(
            frame.data(), 2, 3, 8, outputWidth, outputHeight, false, false, 0,
            rotation, sampling, rgb.data(), rgb.size()));
        CHECK(PerPixel(rgb) == expected);
    }
}

void RotationTests()
{
    CheckRotation(Rotation::None, 2, 3, {0, 1, 2, 3, 4, 5});
    CheckRotation(Rotation::Flip180, 2, 3, {5, 4, 3, 2, 1, 0});
    CheckRotation(Rotation::CW90, 3, 2, {4, 2, 0, 5, 3, 1});
    CheckRotation(Rotation::CCW90, 3, 2, {1, 3, 5, 0, 2, 4});
    CheckRotation(static_cast<Rotation>(200), 2, 3, {0, 1, 2, 3, 4, 5});
}

void InterlaceTests()
{
    const auto frame = GrayFrame(2, 4, {0, 1, 2, 3, 4, 5, 6, 7});
    std::vector<uint8_t> rgb(Rgb24FrameSize(2, 4, true));

    CHECK(TransformBgraToRgb24(
        frame.data(), 2, 4, 8, 2, 4, true, false, 0, Rotation::None,
        SamplingMode::Point, rgb.data(), rgb.size()));
    CHECK(PerPixel(rgb) == std::vector<uint8_t>({2, 3, 6, 7}));

    CHECK(TransformBgraToRgb24(
        frame.data(), 2, 4, 8, 2, 4, true, false, 1, Rotation::None,
        SamplingMode::Point, rgb.data(), rgb.size()));
    CHECK(PerPixel(rgb) == std::vector<uint8_t>({0, 1, 4, 5}));

    std::vector<uint8_t> progressiveRgb(Rgb24FrameSize(2, 4, true, true));
    CHECK(progressiveRgb.size() == 24);
    CHECK(TransformBgraToRgb24(
        frame.data(), 2, 4, 8, 2, 4, true, true, 1,
        Rotation::None, SamplingMode::Point, progressiveRgb.data(), progressiveRgb.size()));
    CHECK(PerPixel(progressiveRgb) == std::vector<uint8_t>({0, 1, 2, 3, 4, 5, 6, 7}));

    const auto lines = GrayFrame(1, 8, {0, 20, 40, 60, 80, 100, 120, 140});
    std::vector<uint8_t> blended(Rgb24FrameSize(1, 4, true));
    CHECK(TransformBgraToRgb24(
        lines.data(), 1, 8, 4, 1, 4, true, false, 0, Rotation::None,
        SamplingMode::LineBlend, blended.data(), blended.size()));
    CHECK(PerPixel(blended) == std::vector<uint8_t>({50, 130}));
    CHECK(TransformBgraToRgb24(
        lines.data(), 1, 8, 4, 1, 4, true, false, 1, Rotation::None,
        SamplingMode::LineBlend, blended.data(), blended.size()));
    CHECK(PerPixel(blended) == std::vector<uint8_t>({10, 90}));
}

void SamplingTests()
{
    const auto ramp = GrayFrame(4, 4, {
        0, 1, 2, 3,
        4, 5, 6, 7,
        8, 9, 10, 11,
        12, 13, 14, 15,
    });
    std::vector<uint8_t> point(Rgb24FrameSize(2, 2, false));
    CHECK(TransformBgraToRgb24(
        ramp.data(), 4, 4, 16, 2, 2, false, false, 0, Rotation::None,
        SamplingMode::Point, point.data(), point.size()));
    CHECK(PerPixel(point) == std::vector<uint8_t>({5, 7, 13, 15}));

    const auto quad = GrayFrame(2, 2, {0, 64, 128, 255});
    std::vector<uint8_t> bilinear(Rgb24FrameSize(1, 1, false));
    CHECK(TransformBgraToRgb24(
        quad.data(), 2, 2, 8, 1, 1, false, false, 0, Rotation::None,
        SamplingMode::Bilinear, bilinear.data(), bilinear.size()));
    CHECK(PerPixel(bilinear) == std::vector<uint8_t>({112}));

    const auto pair = GrayFrame(2, 1, {10, 110});
    bilinear.resize(Rgb24FrameSize(4, 1, false));
    CHECK(TransformBgraToRgb24(
        pair.data(), 2, 1, 8, 4, 1, false, false, 0, Rotation::None,
        SamplingMode::Bilinear, bilinear.data(), bilinear.size()));
    CHECK(PerPixel(bilinear) == std::vector<uint8_t>({10, 35, 85, 110}));

    std::vector<uint8_t> wideRampValues(257);
    for (size_t x = 0; x < wideRampValues.size(); ++x)
        wideRampValues[x] = static_cast<uint8_t>(x * 255 / 256);
    const auto wideRamp = GrayFrame(257, 1, wideRampValues);
    bilinear.resize(Rgb24FrameSize(256, 1, false));
    CHECK(TransformBgraToRgb24(
        wideRamp.data(), 257, 1, 257 * 4, 256, 1, false, false, 0,
        Rotation::None, SamplingMode::Bilinear, bilinear.data(), bilinear.size()));
    const auto narrowedRamp = PerPixel(bilinear);
    CHECK(narrowedRamp.front() == wideRampValues.front());
    CHECK(narrowedRamp.back() == wideRampValues.back());
    CHECK(std::is_sorted(narrowedRamp.begin(), narrowedRamp.end()));

    auto lines = GrayFrame(1, 4, {10, 30, 50, 70});
    std::vector<uint8_t> lineBlend(Rgb24FrameSize(1, 2, false));
    CHECK(TransformBgraToRgb24(
        lines.data(), 1, 4, 4, 1, 2, false, false, 0, Rotation::None,
        SamplingMode::LineBlend, lineBlend.data(), lineBlend.size()));
    CHECK(PerPixel(lineBlend) == std::vector<uint8_t>({20, 60}));

    lines = GrayFrame(1, 5, {0, 10, 20, 30, 40});
    CHECK(TransformBgraToRgb24(
        lines.data(), 1, 5, 4, 1, 2, false, false, 0, Rotation::None,
        SamplingMode::LineBlend, lineBlend.data(), lineBlend.size()));
    CHECK(PerPixel(lineBlend) == std::vector<uint8_t>({8, 32}));

    lines = GrayFrame(1, 2, {20, 80});
    lineBlend.resize(Rgb24FrameSize(1, 3, false));
    CHECK(TransformBgraToRgb24(
        lines.data(), 1, 2, 4, 1, 3, false, false, 0, Rotation::None,
        SamplingMode::LineBlend, lineBlend.data(), lineBlend.size()));
    CHECK(PerPixel(lineBlend) == std::vector<uint8_t>({20, 50, 80}));

    std::vector<uint8_t> fullHdLines(1080);
    for (size_t y = 0; y < fullHdLines.size(); ++y)
        fullHdLines[y] = static_cast<uint8_t>(y % 251);
    lines = GrayFrame(1, 1080, fullHdLines);
    lineBlend.resize(Rgb24FrameSize(1, 240, false));
    CHECK(TransformBgraToRgb24(
        lines.data(), 1, 1080, 4, 1, 240, false, false, 0, Rotation::None,
        SamplingMode::LineBlend, lineBlend.data(), lineBlend.size()));
    const auto reduced = PerPixel(lineBlend);
    CHECK(reduced.size() == 240);
    CHECK(reduced[0] == 2 && reduced[1] == 6);

    for (uint32_t sourceHeight = 1; sourceHeight <= 40; ++sourceHeight)
    {
        const auto flat = GrayFrame(
            3, sourceHeight, std::vector<uint8_t>(3 * sourceHeight, 123));
        for (const uint32_t outputHeight : {1u, 2u, 3u, 4u, 5u, 7u, 8u, 11u, 16u, 23u})
        {
            lineBlend.resize(Rgb24FrameSize(2, outputHeight, false));
            CHECK(TransformBgraToRgb24(
                flat.data(), 3, sourceHeight, 12, 2, outputHeight, false, false, 0,
                Rotation::None, SamplingMode::LineBlend,
                lineBlend.data(), lineBlend.size()));
            CHECK(std::all_of(lineBlend.begin(), lineBlend.end(),
                [](uint8_t value) { return value == 123; }));
        }
    }

    CHECK(std::string(SamplingModeName(SamplingMode::Point)) == "point");
    CHECK(std::string(SamplingModeName(SamplingMode::Bilinear)) == "bilinear");
    CHECK(std::string(SamplingModeName(SamplingMode::LineBlend)) == "line-blend");
}

void BoundsAndChannelTests()
{
    const std::vector<uint8_t> frame = {7, 8, 9, 255};
    const size_t required = Rgb24FrameSize(1, 1, false);
    std::vector<uint8_t> guarded(required + 4, 0xcd);
    CHECK(TransformBgraToRgb24(
        frame.data(), 1, 1, 4, 1, 1, false, false, 0, Rotation::None,
        SamplingMode::Point, guarded.data(), required));
    CHECK(guarded[0] == 7 && guarded[1] == 8 && guarded[2] == 9);
    CHECK(guarded[required] == 0xcd && guarded.back() == 0xcd);

    CHECK(!TransformBgraToRgb24(
        frame.data(), 1, 1, 4, 1, 1, false, false, 0, Rotation::None,
        SamplingMode::Point, guarded.data(), required - 1));
    CHECK(!TransformBgraToRgb24(
        frame.data(), 1, 1, 3, 1, 1, false, false, 0, Rotation::None,
        SamplingMode::Point, guarded.data(), required));
    CHECK(!TransformBgraToRgb24(
        frame.data(), 1, 1, 4, 1, 1, false, false, 0, Rotation::None,
        static_cast<SamplingMode>(200), guarded.data(), required));
    CHECK(Rgb24FrameSize(1, 1, true) == 0);
}
}

int RunAudioProcessingTests();
int RunCaptureResourceTests();
int RunDiagnosticFaultTests();
int RunGroovyProtocolTests();
int RunInterlacePhaseTests();
int RunStreamingTimingTests();
int RunTransportLifecycleTests();
int RunSourceOptionsStateTests();

int main()
{
    RotationTests();
    InterlaceTests();
    SamplingTests();
    BoundsAndChannelTests();
    failures += RunAudioProcessingTests();
    failures += RunCaptureResourceTests();
    failures += RunDiagnosticFaultTests();
    failures += RunGroovyProtocolTests();
    failures += RunInterlacePhaseTests();
    failures += RunStreamingTimingTests();
    failures += RunTransportLifecycleTests();
    failures += RunSourceOptionsStateTests();

    if (failures != 0)
    {
        std::cerr << failures << " core test(s) failed\n";
        return 1;
    }

    std::cout << "All core tests passed\n";
    return 0;
}
