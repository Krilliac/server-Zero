#pragma once
#include <cstdint>
#include <cstring>
#include <type_traits>

class ZeroPacket
{
public:
    // Memory operations
    template <typename T>
    static void Write(void* buffer, size_t offset, T value)
    {
        static_assert(std::is_trivially_copyable_v<T>, "Value must be trivially copyable");
        std::memcpy(static_cast<uint8_t*>(buffer) + offset, &value, sizeof(T));
    }

    template <typename T>
    static T Read(const void* buffer, size_t offset)
    {
        static_assert(std::is_trivially_copyable_v<T>, "Value must be trivially copyable");
        T value;
        std::memcpy(&value, static_cast<const uint8_t*>(buffer) + offset, sizeof(T));
        return value;
    }

    // Protocol builders
    static void BuildEthernet(void* buffer, 
                              const uint8_t srcMac[6], 
                              const uint8_t dstMac[6], 
                              uint16_t etherType);
    
    static void BuildIPv4(void* buffer,
                          uint8_t versionIhl,
                          uint8_t dscpEcn,
                          uint16_t totalLength,
                          uint16_t identification,
                          uint16_t flagsFragmentOffset,
                          uint8_t ttl,
                          uint8_t protocol,
                          uint32_t srcAddr,
                          uint32_t dstAddr);
    
    static void BuildUDP(void* buffer,
                         uint16_t srcPort,
                         uint16_t dstPort,
                         uint16_t length);
};
