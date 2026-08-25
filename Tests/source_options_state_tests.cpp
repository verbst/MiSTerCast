#define NOMINMAX
#include <Windows.h>

#include <atomic>
#include <string>
#include <thread>

#include "SourceOptionsState.h"

namespace
{
bool SameOptions(const SourceOptions& left, const SourceOptions& right)
{
    return left.syncrefresh == right.syncrefresh &&
        left.framedelay == right.framedelay &&
        left.display == right.display &&
        left.audio == right.audio &&
        left.preview == right.preview &&
        left.alignment == right.alignment &&
        left.cropmode == right.cropmode &&
        left.width == right.width &&
        left.height == right.height &&
        left.xoffset == right.xoffset &&
        left.yoffset == right.yoffset &&
        left.rotation == right.rotation &&
        left.sampling == right.sampling &&
        left.windowHandle == right.windowHandle;
}
}

int RunSourceOptionsStateTests()
{
    const SourceOptions first = {
        true, 1, 1, true, false, Alignment::TopLeft, CropMode::X1,
        320, 240, -10, 20, Rotation::CW90, SamplingMode::Bilinear, 0x1234 };
    const SourceOptions second = {
        false, 9, 2, false, true, Alignment::BottomRight, CropMode::Full54,
        720, 480, 30, -40, Rotation::CCW90, SamplingMode::LineBlend, 0x5678 };

    SourceOptionsState state;
    state.publish(first);

    std::atomic_bool start = false;
    std::atomic_bool finished = false;
    std::atomic_int failures = 0;

    std::thread writer([&]() {
        while (!start.load(std::memory_order_acquire))
        {
        }
        for (int iteration = 0; iteration < 10000; ++iteration)
            state.publish((iteration & 1) == 0 ? second : first);
        finished.store(true, std::memory_order_release);
    });

    start.store(true, std::memory_order_release);
    do
    {
        const SourceOptions snapshot = state.snapshot();
        if (!SameOptions(snapshot, first) && !SameOptions(snapshot, second))
            failures.fetch_add(1, std::memory_order_relaxed);
    } while (!finished.load(std::memory_order_acquire));

    writer.join();
    return failures.load(std::memory_order_relaxed);
}
