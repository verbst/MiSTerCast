#include "DiagnosticFaults.h"

namespace
{
int failures = 0;

void Check(bool condition)
{
    if (!condition)
        ++failures;
}
}

int RunDiagnosticFaultTests()
{
    using mistercast::StreamFaultOptions;
    using mistercast::StreamFaultSchedule;

    StreamFaultSchedule disabled;
    for (int index = 0; index < 10; ++index)
    {
        const auto action = disabled.Next();
        Check(action.attempt == static_cast<uint64_t>(index + 1));
        Check(!action.skip && action.stallMilliseconds == 0);
    }

    StreamFaultOptions options = {};
    options.skipEvery = 3;
    options.stallEvery = 4;
    options.stallMilliseconds = 25;
    StreamFaultSchedule enabled(options);
    for (uint64_t attempt = 1; attempt <= 12; ++attempt)
    {
        const auto action = enabled.Next();
        Check(action.attempt == attempt);
        Check(action.skip == (attempt % 3 == 0));
        Check(action.stallMilliseconds == (attempt % 4 == 0 ? 25u : 0u));
    }

    return failures;
}
