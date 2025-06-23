#pragma once
#include "WorldPacket.h"
#include <unordered_map>
#include <string>
#include <mutex>
#include <atomic>

class SecurityMgr
{
public:
    static SecurityMgr* instance();
    
    void Initialize();
    bool ValidatePacket(WorldPacket& packet);
    void BanAddress(const std::string& address, uint32_t duration);
    
    // Configuration
    void SetMaxPacketSize(uint32_t size) { m_maxPacketSize = size; }
    void SetMaxPacketFrequency(uint32_t freq) { m_maxPacketFrequency = freq; }
    void SetBanDuration(uint32_t duration) { m_defaultBanDuration = duration; }

private:
    struct ConnectionState {
        std::atomic<uint32_t> packetCount = 0;
        std::atomic<uint32_t> lastPacketTime = 0;
        std::atomic<bool> flagged = false;
    };
    
    std::unordered_map<std::string, ConnectionState> m_connectionMap;
    mutable std::mutex m_lock;
    
    // Configurable thresholds
    uint32_t m_maxPacketSize = 4096;
    uint32_t m_maxPacketFrequency = 50;
    uint32_t m_defaultBanDuration = 300;
};

#define sSecurityMgr SecurityMgr::instance()
