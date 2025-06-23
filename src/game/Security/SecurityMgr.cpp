#include "SecurityMgr.h"
#include "WorldSession.h"
#include "Log.h"
#include "Config.h"
#include "World.h"
#include "Player.h"
#include <ctime>

SecurityMgr* SecurityMgr::instance()
{
    static SecurityMgr instance;
    return &instance;
}

void SecurityMgr::Initialize()
{
    m_maxPacketSize = sConfig.GetIntDefault("Security.MaxPacketSize", 4096);
    m_maxPacketFrequency = sConfig.GetIntDefault("Security.MaxPacketFrequency", 50);
    m_defaultBanDuration = sConfig.GetIntDefault("Security.BanDuration", 300);
}

bool SecurityMgr::ValidatePacket(WorldPacket& packet)
{
    const std::string address = packet.GetSession()->GetRemoteAddress();
    const uint32_t currentTime = static_cast<uint32_t>(std::time(nullptr));
    
    std::lock_guard<std::mutex> lock(m_lock);
    ConnectionState& state = m_connectionMap[address];
    
    // 1. Check if connection is flagged (banned)
    if (state.flagged.load()) {
        return false;
    }
    
    // 2. Packet size check
    if (packet.size() > m_maxPacketSize) {
        sLog.outSecurity("Oversized packet (%u bytes) from %s", packet.size(), address.c_str());
        BanAddress(address, m_defaultBanDuration);
        return false;
    }
    
    // 3. Packet frequency check
    const uint32_t lastTime = state.lastPacketTime.load();
    if (lastTime > 0 && (currentTime - lastTime) < 1) {
        const uint32_t count = ++state.packetCount;
        if (count > m_maxPacketFrequency) {
            sLog.outSecurity("Packet flood (%u packets/s) from %s", count, address.c_str());
            BanAddress(address, m_defaultBanDuration);
            return false;
        }
    } else {
        state.packetCount = 1;
    }
    state.lastPacketTime = currentTime;
    
    return true;
}

void SecurityMgr::BanAddress(const std::string& address, uint32_t duration)
{
    std::lock_guard<std::mutex> lock(m_lock);
    ConnectionState& state = m_connectionMap[address];
    state.flagged = true;
    
    // Log ban with duration
    sLog.outSecurity("Banned %s for %u seconds", address.c_str(), duration);
    
    // Disconnect all sessions from this address
    for (auto& session : sWorld->GetSessions()) {
        if (session.second->GetRemoteAddress() == address) {
            session.second->KickPlayer("Security violation");
        }
    }
    
    // Schedule unban
    sWorld->Schedule([this, address]() {
        std::lock_guard<std::mutex> lock(m_lock);
        if (auto it = m_connectionMap.find(address); it != m_connectionMap.end()) {
            it->second.flagged = false;
            sLog.outSecurity("Unbanned %s", address.c_str());
        }
    }, duration * 1000);
}
