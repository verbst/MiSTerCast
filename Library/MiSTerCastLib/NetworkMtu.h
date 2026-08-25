#pragma once

#include <cstdint>

namespace mistercast
{
constexpr uint32_t DefaultPathMtu = 1500;
constexpr uint32_t Ipv4UdpHeaderBytes = 28;

inline uint32_t ResolvePathMtu(uint32_t configuredMtu) noexcept
{
    return configuredMtu == 0 ? DefaultPathMtu : configuredMtu;
}

inline bool IsSupportedPathMtu(uint32_t configuredMtu) noexcept
{
    const uint32_t pathMtu = ResolvePathMtu(configuredMtu);
    // The transport has 846 registered slices, sized for a 1472-byte UDP
    // payload. A smaller MTU would require more descriptors than allocated.
    return pathMtu >= DefaultPathMtu && pathMtu <= 65535;
}

inline uint32_t UdpPayloadBytes(uint32_t configuredMtu) noexcept
{
    const uint32_t pathMtu = ResolvePathMtu(configuredMtu);
    return IsSupportedPathMtu(configuredMtu) ? pathMtu - Ipv4UdpHeaderBytes : 0;
}

inline bool InterfaceSupportsPathMtu(
    uint32_t configuredMtu,
    uint32_t interfaceMtu) noexcept
{
    return IsSupportedPathMtu(configuredMtu) &&
        interfaceMtu >= ResolvePathMtu(configuredMtu);
}
}
