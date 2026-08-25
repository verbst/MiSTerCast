#include "AudioProcessing.h"

#include <cstdint>
#include <limits>
#include <vector>

namespace
{
int failures = 0;

void Check(bool condition)
{
    if (!condition)
        ++failures;
}
}

int RunAudioProcessingTests()
{
    using mistercast::ConvertFloatFramesToStereo;
    using mistercast::FloatToPcm16;

    Check(FloatToPcm16(-2.0f) == std::numeric_limits<int16_t>::min());
    Check(FloatToPcm16(-1.0f) == std::numeric_limits<int16_t>::min());
    Check(FloatToPcm16(-0.5f) == -16384);
    Check(FloatToPcm16(0.0f) == 0);
    Check(FloatToPcm16(0.5f) == 16384);
    Check(FloatToPcm16(1.0f) == std::numeric_limits<int16_t>::max());
    Check(FloatToPcm16(2.0f) == std::numeric_limits<int16_t>::max());
    Check(FloatToPcm16(std::numeric_limits<float>::quiet_NaN()) == 0);
    Check(FloatToPcm16(std::numeric_limits<float>::infinity()) == 0);

    const float mono[] = {-1.0f, 0.5f};
    std::vector<int16_t> stereo(4);
    Check(ConvertFloatFramesToStereo(mono, 2, 1, stereo.data(), stereo.size()));
    Check(stereo == std::vector<int16_t>({-32768, -32768, 16384, 16384}));

    const float surround[] = {0.25f, -0.25f, 0.9f, 0.8f, 0.75f, -0.75f, 0.7f, 0.6f};
    Check(ConvertFloatFramesToStereo(surround, 2, 4, stereo.data(), stereo.size()));
    Check(stereo == std::vector<int16_t>({8192, -8192, 24575, -24575}));
    Check(!ConvertFloatFramesToStereo(surround, 2, 4, stereo.data(), stereo.size() - 1));
    Check(!ConvertFloatFramesToStereo(surround, 2, 0, stereo.data(), stereo.size()));

    Check(mistercast::MaxAudioValuesPerCommand % 2 == 0);
    Check(mistercast::MaxAudioValuesPerCommand * sizeof(int16_t) <= std::numeric_limits<uint16_t>::max());
    return failures;
}
