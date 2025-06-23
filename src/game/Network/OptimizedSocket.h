#pragma once
#include "WorldPacket.h"
#include "RateLimiter.h"
#include "PacketBundler.h"
#include "ZstdCompression.h"
#include "NetworkAddressResolver.h"
#include "ZeroPacket.h"
#include <ace/SOCK_Stream.h>
#include <memory>
#include <queue>
#include <mutex>
#include <array>

class OptimizedSocket
{
public:
    OptimizedSocket(ACE_SOCK_Stream& stream);
    ~OptimizedSocket();

    bool HandleIncoming();
    bool SendPacket(const WorldPacket& packet);
    void Update(uint32 diff);

    // Zero-copy packet builders
    void BuildMovementPacket(void* buffer, 
                             const MovementInfo& moveInfo,
                             uint32 clientTime);

private:
    void ProcessPacket(WorldPacket& packet);
    void FlushBundledPackets();
    void HandleError(const char* context);

    ACE_SOCK_Stream& m_stream;
    std::unique_ptr<RateLimiter> m_rateLimiter;
    std::unique_ptr<PacketBundler> m_bundler;
    std::queue<std::vector<uint8_t>> m_outgoing;  // Stores raw packets
    std::mutex m_outgoingMutex;
    uint32 m_compressionLevel = 3;
    bool m_useUDPFastPath = true;
    
    // Pre-allocated buffers for zero-copy
    std::array<uint8_t, 1500> m_udpBuffer;
};
