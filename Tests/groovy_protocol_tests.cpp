#include "GroovyProtocol.h"

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

int RunGroovyProtocolTests()
{
    using namespace mistercast::protocol;

    uint8_t blit[BlitHeaderSize] = {};
    Check(EncodeBlitHeader(blit, sizeof(blit), 0x78563412, 1, 0xabcd));
    Check(blit[0] == CommandBlitFieldVsync);
    Check(blit[1] == 0x12 && blit[2] == 0x34 && blit[3] == 0x56 && blit[4] == 0x78);
    Check(blit[5] == 1 && blit[6] == 0xcd && blit[7] == 0xab);
    Check(!EncodeBlitHeader(blit, sizeof(blit) - 1, 1, 0, 1));
    Check(!EncodeBlitHeader(nullptr, sizeof(blit), 1, 0, 1));

    const uint8_t packet[FpgaStatusSize] = {
        0x78, 0x56, 0x34, 0x12,
        0xcd, 0xab,
        0xef, 0xcd, 0xab, 0x90,
        0x34, 0x12,
        0xa5
    };
    DecodedFpgaStatus status = {};
    Check(DecodeFpgaStatus(packet, sizeof(packet), status));
    Check(status.frameEcho == 0x12345678);
    Check(status.vCountEcho == 0xabcd);
    Check(status.frame == 0x90abcdef);
    Check(status.vCount == 0x1234);
    Check(status.bits == 0xa5);
    Check(!DecodeFpgaStatus(packet, sizeof(packet) - 1, status));

    Check(ShouldAcceptStatus(false, 100, 1));
    Check(ShouldAcceptStatus(true, 100, 100));
    Check(ShouldAcceptStatus(true, 100, 101));
    Check(!ShouldAcceptStatus(true, 100, 99));
    Check(ShouldAcceptStatus(true, std::numeric_limits<uint32_t>::max(), 0));
    Check(!ShouldAcceptStatus(true, 0, std::numeric_limits<uint32_t>::max()));
    Check(!ShouldAcceptStatus(true, 0, 0x80000000u));

    return failures;
}
