#include "CaptureResources.h"
#include "NetworkMtu.h"

namespace
{
int failures = 0;

void Check(bool condition)
{
    if (!condition)
        ++failures;
}
}

int RunCaptureResourceTests()
{
    using mistercast::InterfaceSupportsPathMtu;
    using mistercast::IsSupportedPathMtu;
    using mistercast::ResolvePathMtu;
    using mistercast::SameStagingTexture;
    using mistercast::StagingTextureSpec;
    using mistercast::UdpPayloadBytes;

    const StagingTextureSpec initial = { 1920, 1080, 87 };
    Check(SameStagingTexture(initial, { 1920, 1080, 87 }));
    Check(!SameStagingTexture(initial, { 1280, 1080, 87 }));
    Check(!SameStagingTexture(initial, { 1920, 720, 87 }));
    Check(!SameStagingTexture(initial, { 1920, 1080, 28 }));

    Check(ResolvePathMtu(0) == 1500);
    Check(IsSupportedPathMtu(0));
    Check(IsSupportedPathMtu(1500));
    Check(IsSupportedPathMtu(9000));
    Check(!IsSupportedPathMtu(1499));
    Check(!IsSupportedPathMtu(65536));
    Check(UdpPayloadBytes(0) == 1472);
    Check(UdpPayloadBytes(9000) == 8972);
    Check(UdpPayloadBytes(1499) == 0);
    Check(InterfaceSupportsPathMtu(0, 1500));
    Check(InterfaceSupportsPathMtu(1500, 1500));
    Check(InterfaceSupportsPathMtu(1500, 9000));
    Check(!InterfaceSupportsPathMtu(1500, 1492));

    return failures;
}
