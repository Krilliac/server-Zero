#include "OptimizedSocket.h"
#include "Log.h"
#include "World.h"
#include "Config.h"
#include "MovementInfo.h"
#include "NetworkAddressResolver.h"

OptimizedSocket::OptimizedSocket(ACE_SOCK_Stream& stream)
    : m_stream(stream),
      m_rateLimiter(new RateLimiter()),
      m_bundler(new PacketBundler())
{
    m_compressionLevel = sConfig.GetIntDefault("Network.CompressionLevel", 3);
    m_useUDPFastPath = sConfig.GetBoolDefault("Network.UDPFastPath", true);
}

OptimizedSocket::~OptimizedSocket() {}

bool OptimizedSocket::HandleIncoming()
{
    ACE_Message_Block block(4096);
    const ssize_t bytesRead = m_stream.recv(block.rd_ptr(), block.space());
    
    if (bytesRead <= 0) {
        HandleError("recv");
        return false;
    }
    
    block.wr_ptr(bytesRead);
    WorldPacket packet(block.rd_ptr(), bytesRead);
    ProcessPacket(packet);
    return true;
}

void OptimizedSocket::ProcessPacket(WorldPacket& packet)
{
    // Apply rate limiting first
    if (!m_rateLimiter->AllowPacket()) {
        sLog.outNetwork("Rate limited packet from %s", m_stream.get_remote_addr().get_host_addr());
        return;
    }
    
    // Handle movement packets via UDP fastpath
    if (packet.GetOpcode() == CMSG_MOVE_CHARACTER && m_useUDPFastPath) {
        // Process immediately without compression/bundling
    } else {
        // Compress non-movement packets
        if (packet.size() > 64) {
            WorldPacket compressed = ZstdCompress(packet, m_compressionLevel);
            m_bundler->AddPacket(compressed);
        } else {
            m_bundler->AddPacket(packet);
        }
    }
}

bool OptimizedSocket::SendPacket(const WorldPacket& packet)
{
    // Movement packets use UDP channel
    if (packet.GetOpcode() == MSG_MOVE_UPDATE && m_useUDPFastPath) {
        BuildMovementPacket(m_udpBuffer.data(), 
                           *reinterpret_cast<const MovementInfo*>(packet.contents()),
                           WorldTimer::getMSTime());
        return SendRawPacket(m_udpBuffer.data(), 14 + 20 + 8 + sizeof(MovementInfo) + 4);
    }
    
    // Other packets use TCP with bundling
    std::vector<uint8_t> packetData(packet.size());
    std::memcpy(packetData.data(), packet.contents(), packet.size());
    
    std::lock_guard<std::mutex> lock(m_outgoingMutex);
    m_outgoing.push(std::move(packetData));
    return true;
}

void OptimizedSocket::ProcessUDP(WorldPacket& packet)
{
    // Apply Zstd compression for large packets
    const size_t compressionThreshold = sConfig.GetIntDefault("Network.CompressionThreshold", 128);
    if (packet.size() > compressionThreshold) {
        WorldPacket compressed = CompressPacket(packet);
        SendPacket(compressed);
    } else {
        SendPacket(packet);
    }
    
    // Movement prediction reconciliation
    if (packet.GetOpcode() == MSG_MOVE_HEARTBEAT) {
        sMovementMgr->ReconcilePosition(_player, packet);
    }
}

void OptimizedSocket::Update(uint32 diff)
{
    // Flush if we have packets and bundler is ready
    if (!m_outgoing.empty()) {
        FlushBundledPackets();
    }
    m_bundler->Update(diff);
}

void OptimizedSocket::FlushBundledPackets()
{
    std::lock_guard<std::mutex> lock(m_outgoingMutex);
    
    while (!m_outgoing.empty()) {
        const std::vector<uint8_t>& packet = m_outgoing.front();
        const ssize_t sent = m_stream.send(packet.data(), packet.size());
        
        if (sent != static_cast<ssize_t>(packet.size())) {
            HandleError("send");
            break;
        }
        m_outgoing.pop();
    }
}

void OptimizedSocket::HandleError(const char* context)
{
    sLog.outError("Network error (%s): %s", context, ACE_OS::strerror(ACE_OS::last_error()));
    // Additional error handling logic
}

void OptimizedSocket::BuildMovementPacket(void* buffer, 
                                         const MovementInfo& moveInfo,
                                         uint32 clientTime)
{
    // Resolve addresses dynamically
    ACE_INET_Addr localAddr, peerAddr;
    m_stream.get_local_addr(localAddr);
    m_stream.get_remote_addr(peerAddr);
    
    uint8_t srcMac[6], dstMac[6];
    NetworkAddressResolver::GetInterfaceMac(localAddr, srcMac);
    NetworkAddressResolver::GetInterfaceMac(peerAddr, dstMac);  // Requires ARP in real impl

    // Build Ethernet frame (14 bytes)
    ZeroPacket::BuildEthernet(buffer, srcMac, dstMac, 0x0800);  // IPv4
    
    // Build IP header (20 bytes)
    uint8_t* ipHeader = static_cast<uint8_t*>(buffer) + 14;
    ZeroPacket::BuildIPv4(ipHeader,
                          0x45,  // Version 4, IHL 5 (20 bytes)
                          0x00,  // DSCP/ECN
                          20 + 8 + sizeof(MovementInfo),  // Total length
                          0x1234,  // Identification
                          0x4000,  // Flags: Don't fragment
                          64,       // TTL
                          17,       // Protocol: UDP
                          localAddr.get_ip_address(),  // Source IP
                          peerAddr.get_ip_address());   // Dest IP
    
    // Build UDP header (8 bytes)
    uint8_t* udpHeader = ipHeader + 20;
    ZeroPacket::BuildUDP(udpHeader,
                         NetworkAddressResolver::AllocateDynamicPort(),  // Source port
                         peerAddr.get_port_number(),  // Dest port
                         8 + sizeof(MovementInfo));   // Length
    
    // Write movement data
    uint8_t* payload = udpHeader + 8;
    std::memcpy(payload, &moveInfo, sizeof(MovementInfo));
    ZeroPacket::Write<uint32_t>(payload, sizeof(MovementInfo), clientTime);
}

bool OptimizedSocket::SendRawPacket(const void* data, size_t size)
{
    // Bypass bundler and rate limiter for critical packets
    const ssize_t sent = m_stream.send(data, size);
    if (sent != static_cast<ssize_t>(size)) {
        HandleError("raw_send");
        return false;
    }
    return true;
}
