#include "groovymister.h"

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include <lz4.h>

namespace
{
int failures = 0;

void Check(bool condition)
{
    if (!condition)
        ++failures;
}

void CheckRioCompletionDraining()
{
    WSADATA winsock = {};
    Check(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);
    SOCKET server = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    Check(server != INVALID_SOCKET);
    if (server == INVALID_SOCKET)
    {
        WSACleanup();
        return;
    }

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Check(bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    int addressSize = sizeof(address);
    Check(getsockname(server, reinterpret_cast<sockaddr*>(&address), &addressSize) == 0);
    const DWORD timeout = 2000;
    setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    std::atomic<unsigned> blits = 0;
    std::atomic<unsigned> payloads = 0;
    std::thread endpoint([&]() {
        uint8_t packet[2048];
        sockaddr_in peer = {};
        int peerSize = sizeof(peer);
        for (;;)
        {
            const int received = recvfrom(
                server,
                reinterpret_cast<char*>(packet),
                sizeof(packet),
                0,
                reinterpret_cast<sockaddr*>(&peer),
                &peerSize);
            if (received <= 0)
                break;
            if (received == 1 && packet[0] == 1)
                break;
            if (received == 5 && packet[0] == 2)
            {
                uint8_t ack[13] = {};
                sendto(
                    server,
                    reinterpret_cast<const char*>(ack),
                    sizeof(ack),
                    0,
                    reinterpret_cast<sockaddr*>(&peer),
                    peerSize);
            }
            else if (received == 8 && packet[0] == 7)
            {
                uint32_t frame = 0;
                uint16_t syncLine = 0;
                std::memcpy(&frame, packet + 1, sizeof(frame));
                std::memcpy(&syncLine, packet + 6, sizeof(syncLine));
                uint8_t ack[13] = {};
                std::memcpy(ack, &frame, sizeof(frame));
                std::memcpy(ack + 4, &syncLine, sizeof(syncLine));
                std::memcpy(ack + 6, &frame, sizeof(frame));
                ack[12] = 0x84; // VRAM synced and queued.
                sendto(
                    server,
                    reinterpret_cast<const char*>(ack),
                    sizeof(ack),
                    0,
                    reinterpret_cast<sockaddr*>(&peer),
                    peerSize);
                ++blits;
            }
            else if (received == 12)
            {
                ++payloads;
            }
        }
    });

    constexpr uint32_t FrameCount = 300;
    {
        GroovyMister transport;
        Check(transport.CmdInit(
            "127.0.0.1", ntohs(address.sin_port), 0, 0, 0, 0, 1500) == 0);
        transport.CmdSwitchres(1.0, 2, 2, 2, 4, 2, 2, 2, 5, 0);
        for (uint32_t frame = 1; frame <= FrameCount; ++frame)
        {
            const ULONGLONG deadline = GetTickCount64() + 1000;
            while (!transport.CanWriteBlitBuffer(0) && GetTickCount64() < deadline)
                SwitchToThread();
            Check(transport.CanWriteBlitBuffer(0));
            std::memset(transport.getPBufferBlit(0), 42, 12);
            transport.CmdBlit(frame, 0, 2, 15000, 0);
            transport.WaitSync();
        }
        transport.CmdClose();
        transport.CmdClose();
    }

    endpoint.join();
    Check(blits == FrameCount);
    Check(payloads == FrameCount);
    closesocket(server);
    WSACleanup();
}

void CheckIncompressibleInterlacedFieldFallback()
{
    constexpr uint32_t Width = 128;
    constexpr uint32_t ActiveHeight = 128;
    constexpr size_t RawFieldSize = Width * (ActiveHeight / 2) * 3;

    std::vector<char> expected(RawFieldSize);
    uint32_t random = 0x6d2b79f5u;
    for (char& value : expected)
    {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        value = static_cast<char>(random & 0xff);
    }
    std::vector<char> compressed(RawFieldSize);
    Check(LZ4_compress_default(
        expected.data(), compressed.data(), static_cast<int>(expected.size()),
        static_cast<int>(compressed.size())) == 0);

    WSADATA winsock = {};
    Check(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);
    SOCKET server = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    Check(server != INVALID_SOCKET);
    if (server == INVALID_SOCKET)
    {
        WSACleanup();
        return;
    }

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Check(bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    int addressSize = sizeof(address);
    Check(getsockname(server, reinterpret_cast<sockaddr*>(&address), &addressSize) == 0);
    const DWORD timeout = 2000;
    setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    std::atomic_bool rawHeader = false;
    std::atomic_bool payloadMatches = true;
    std::atomic_size_t payloadBytes = 0;
    std::thread endpoint([&]() {
        uint8_t packet[2048];
        sockaddr_in peer = {};
        int peerSize = sizeof(peer);
        size_t receivedPayload = 0;
        for (;;)
        {
            const int received = recvfrom(
                server,
                reinterpret_cast<char*>(packet),
                sizeof(packet),
                0,
                reinterpret_cast<sockaddr*>(&peer),
                &peerSize);
            if (received <= 0)
                break;

            if (receivedPayload < RawFieldSize && rawHeader.load(std::memory_order_acquire))
            {
                if (receivedPayload + received > RawFieldSize ||
                    std::memcmp(packet, expected.data() + receivedPayload, received) != 0)
                {
                    payloadMatches.store(false, std::memory_order_release);
                }
                receivedPayload += received;
                payloadBytes.store(receivedPayload, std::memory_order_release);
                continue;
            }
            if (received == 1 && packet[0] == 1)
                break;
            if (received == 5 && packet[0] == 2)
            {
                uint8_t ack[13] = {};
                sendto(
                    server,
                    reinterpret_cast<const char*>(ack),
                    sizeof(ack),
                    0,
                    reinterpret_cast<sockaddr*>(&peer),
                    peerSize);
            }
            else if (received == 8 && packet[0] == 7 && packet[5] == 1)
            {
                rawHeader.store(true, std::memory_order_release);
                uint8_t ack[13] = {};
                std::memcpy(ack, packet + 1, sizeof(uint32_t));
                std::memcpy(ack + 6, packet + 1, sizeof(uint32_t));
                ack[12] = 0x84;
                sendto(
                    server,
                    reinterpret_cast<const char*>(ack),
                    sizeof(ack),
                    0,
                    reinterpret_cast<sockaddr*>(&peer),
                    peerSize);
            }
        }
    });

    {
        GroovyMister transport;
        Check(transport.CmdInit(
            "127.0.0.1", ntohs(address.sin_port), 1, 0, 0, 0, 1500) == 0);
        transport.CmdSwitchres(5.0, Width, 130, 132, 140, ActiveHeight, 130, 132, 140, 1);
        std::memcpy(transport.getPBufferBlit(1), expected.data(), expected.size());
        transport.CmdBlit(1, 1, 70, 15000, 0);
        const ULONGLONG deadline = GetTickCount64() + 2000;
        while (!transport.CanWriteBlitBuffer(1) && GetTickCount64() < deadline)
            SwitchToThread();
        Check(transport.CanWriteBlitBuffer(1));
        transport.CmdClose();
    }

    endpoint.join();
    Check(rawHeader.load(std::memory_order_acquire));
    Check(payloadMatches.load(std::memory_order_acquire));
    Check(payloadBytes.load(std::memory_order_acquire) == RawFieldSize);
    closesocket(server);
    WSACleanup();
}
}

int RunTransportLifecycleTests()
{
    // Reject an undersized datagram path before creating sockets, and retain a
    // user-facing reason that the GUI and CLI can report.
    {
        GroovyMister transport;
        Check(transport.CmdInit("127.0.0.1", 65534, 1, 0, 0, 0, 1499) == -1);
        Check(transport.getLastError().find("MTU") != std::string::npos);
    }

    // INIT with no endpoint must time out, cancel its pending RIO operations,
    // and tolerate explicit plus destructor cleanup without touching resources twice.
    {
        GroovyMister transport;
        Check(transport.CmdInit("127.0.0.1", 65534, 1, 0, 0, 0, 1500) == -1);
        transport.CmdClose();
        transport.CmdClose();
    }
    {
        GroovyMister transport;
        Check(transport.CmdInit("127.0.0.1", 65534, 1, 0, 0, 0, 1500) == -1);
    }
    CheckRioCompletionDraining();
    CheckIncompressibleInterlacedFieldFallback();
    return failures;
}
