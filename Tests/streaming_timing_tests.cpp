#include "StreamingTiming.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace
{
int failures = 0;

void Check(bool condition)
{
    if (!condition)
        ++failures;
}
}

int RunStreamingTimingTests()
{
    using mistercast::CounterTicksTo100ns;
    using mistercast::FieldPeriodMilliseconds;
    using mistercast::LinePeriodMilliseconds;
    using mistercast::IsValidStreamModeline;
    using mistercast::ProtocolInterlaceMode;
    using mistercast::RequestedSyncLine;

    Check(CounterTicksTo100ns(10000000, 10000000) == 10000000);
    Check(CounterTicksTo100ns(3579545, 3579545) == 10000000);
    Check(CounterTicksTo100ns(12000000, 24000000) == 5000000);
    Check(CounterTicksTo100ns(1, 0) == 0);
    Check(CounterTicksTo100ns(std::numeric_limits<uint64_t>::max() / 2, 1) ==
        std::numeric_limits<uint32_t>::max());

    Check(std::abs(LinePeriodMilliseconds(25.175, 800) - 0.0317775571) < 0.0000001);
    Check(std::abs(FieldPeriodMilliseconds(25.175, 800, 525, false) - 16.6832175) < 0.0001);
    Check(std::abs(FieldPeriodMilliseconds(12.5875, 800, 525, true) - 16.6832175) < 0.0001);
    Check(FieldPeriodMilliseconds(0.0, 800, 525, false) == 0.0);

    Check(RequestedSyncLine(525, 0.0, false, true) == 1);
    Check(RequestedSyncLine(525, 0.5, false, true) == 263);
    Check(RequestedSyncLine(525, 1.0, false, true) == 525);
    Check(RequestedSyncLine(525, 0.9, true, true) == 262);
    Check(RequestedSyncLine(525, 0.9, true, false) == 473);
    Check(RequestedSyncLine(525, std::numeric_limits<double>::quiet_NaN(), false, true) == 1);
    Check(RequestedSyncLine(0, 0.5, false, true) == 0);

    Check(IsValidStreamModeline(13.846, 720, 744, 809, 880, 480, 488, 494, 525, true));
    Check(IsValidStreamModeline(13.875, 720, 741, 806, 888, 576, 581, 586, 625, true));
    Check(!IsValidStreamModeline(0.0, 720, 744, 809, 880, 480, 488, 494, 525, true));
    Check(!IsValidStreamModeline(13.846, 720, 700, 809, 880, 480, 488, 494, 525, true));
    Check(!IsValidStreamModeline(13.846, 720, 744, 809, 880, 481, 488, 494, 525, true));
    Check(!IsValidStreamModeline(40.0, 1280, 1300, 1320, 1400, 720, 725, 730, 750, false));
    Check(IsValidStreamModeline(13.846, 720, 744, 809, 880, 480, 488, 494, 525, true, true));
    Check(!IsValidStreamModeline(13.846, 720, 744, 809, 880, 480, 488, 494, 525, false, true));
    Check(!IsValidStreamModeline(40.0, 1280, 1300, 1320, 1400, 720, 725, 730, 750, true, true));
    Check(ProtocolInterlaceMode(false, false) == 0);
    Check(ProtocolInterlaceMode(true, false) == 1);
    Check(ProtocolInterlaceMode(true, true) == 2);

    return failures;
}
