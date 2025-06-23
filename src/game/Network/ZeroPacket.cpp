#include "ZeroPacket.h"

void ZeroPacket::BuildEthernet(void* buffer, 
                               const uint8_t srcMac[6], 
                               const uint8_t dstMac[6], 
                               uint16_t etherType)
{
    uint8_t* ptr = static_cast<uint8_t*>(buffer);
    std::memcpy(ptr, dstMac, 6);
    std::memcpy(ptr + 6, srcMac, 6);
    Write<uint16_t>(ptr, 12, etherType);
}

void ZeroPacket::BuildIPv4(void* buffer,
                           uint8_t versionIhl,
                           uint8_t dscpEcn,
                           uint16_t totalLength,
                           uint16_t identification,
                           uint16_t flagsFragmentOffset,
                           uint8_t ttl,
                           uint8_t protocol,
                           uint32_t srcAddr,
                           uint32_t dstAddr)
{
    uint8_t* ptr = static_cast<uint8_t*>(buffer);
    Write<uint8_t>(ptr, 0, versionIhl);
    Write<uint8_t>(ptr, 1, dscpEcn);
    Write<uint16_t>(ptr, 2, totalLength);
    Write<uint16_t>(ptr, 4, identification);
    Write<uint16_t>(ptr, 6, flagsFragmentOffset);
    Write<uint8_t>(ptr, 8, ttl);
    Write<uint8_t>(ptr, 9, protocol);
    // Checksum calculated later
    Write<uint32_t>(ptr, 12, srcAddr);
    Write<uint32_t>(ptr, 16, dstAddr);
}

void ZeroPacket::BuildUDP(void* buffer,
                          uint16_t srcPort,
                          uint16_t dstPort,
                          uint16_t length)
{
    uint8_t* ptr = static_cast<uint8_t*>(buffer);
    Write<uint16_t>(ptr, 0, srcPort);
    Write<uint16_t>(ptr, 2, dstPort);
    Write<uint16_t>(ptr, 4, length);
    // Checksum calculated later
}
