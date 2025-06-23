#include "TimeSyncMgr.h"
#include "WorldSession.h"
#include "Log.h"
#include "Config.h"
#include "World.h"
#include "ObjectAccessor.h"
#include "TimeSyncChecks.h"
#include "Util.h"

TimeSyncMgr* TimeSyncMgr::instance()
{
    static TimeSyncMgr instance;
    return &instance;
}

void TimeSyncMgr::Initialize()
{
    // Load configuration
    m_driftThreshold = sConfig.GetIntDefault("TimeSync.MaxDrift", 500);
    m_maxSkipMultiplier = sConfig.GetFloatDefault("TimeSync.MaxSkipMultiplier", 4.0f);
    m_pingInterval = sConfig.GetIntDefault("TimeSync.Interval", 30000);
    
    sLog.outString("TimeSyncMgr initialized with settings:");
    sLog.outString("  MaxDrift: %dms", m_driftThreshold);
    sLog.outString("  MaxSkipMultiplier: %.1f", m_maxSkipMultiplier);
    sLog.outString("  PingInterval: %dms", m_pingInterval);
}

void TimeSyncMgr::Update(uint32 diff)
{
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    for (auto& [guid, data] : m_playerData)
    {
        if (Player* player = ObjectAccessor::FindPlayer(guid))
        {
            data.nextPingTimer += diff;
            
            if (!data.awaitingPong && data.nextPingTimer >= m_pingInterval)
            {
                SendPingRequest(player);
                data.awaitingPong = true;
                data.nextPingTimer = 0;
            }
        }
    }
}

void TimeSyncMgr::HandleClientPing(Player* player, uint32 clientTime)
{
    ObjectGuid guid = player->GetGUID();
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    if (m_playerData.find(guid) == m_playerData.end()) 
        return;
    
    // Respond immediately with pong
    WorldPacket dataPong(SMSG_PONG, 4);
    dataPong << clientTime;
    player->GetSession()->SendPacket(&dataPong);
}

void TimeSyncMgr::HandleClientPong(Player* player, uint32 serverTime)
{
    ObjectGuid guid = player->GetGUID();
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    auto it = m_playerData.find(guid);
    if (it == m_playerData.end() || !it->second.awaitingPong) 
        return;

    TimeSyncData& data = it->second;
    data.awaitingPong = false;
    
    const uint32 now = WorldTimer::getMSTime();
    const uint32 rtt = now - data.lastPingTime;
    const int32 newOffset = TimeSync::CalculateOffset(player, serverTime, now, rtt);
    
    UpdateOffset(player, newOffset);
}

void TimeSyncMgr::SendPingRequest(Player* player)
{
    WorldPacket dataPing(SMSG_PING, 4);
    uint32 pingTime = WorldTimer::getMSTime();
    dataPing << pingTime;
    player->GetSession()->SendPacket(&dataPing);
    
    std::lock_guard<std::mutex> lock(m_dataMutex);
    m_playerData[player->GetGUID()].lastPingTime = pingTime;
}

void TimeSyncMgr::UpdateOffset(Player* player, int32 newOffset)
{
    ObjectGuid guid = player->GetGUID();
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    auto it = m_playerData.find(guid);
    if (it == m_playerData.end()) 
        return;

    TimeSyncData& data = it->second;
    data.offset = TimeSync::ApplyKalmanFilter(newOffset, data.offset, data.kalmanGain);
    
    const int32 drift = std::abs(data.offset - data.lastDrift);
    data.lastDrift = data.offset;
    
    if (drift > m_driftThreshold)
    {
        ApplyDriftCorrection(player, drift);
    }
    else if (drift > 100)
    {
        // Dynamically adjust Kalman gain based on drift severity
        data.kalmanGain = std::min(0.9f, 0.1f + (0.8f * drift / m_driftThreshold));
    }
    
    // Trigger anti-cheat checks if needed
    TimeSyncChecks::HandleTimeDrift(player, drift);
}

void TimeSyncMgr::ApplyDriftCorrection(Player* player, int32 drift)
{
    const int32 maxSkip = static_cast<int32>(m_maxSkipMultiplier * player->GetSession()->GetLatency());
    const int32 correction = std::clamp(drift, -maxSkip, maxSkip);
    player->m_timeOffset += correction;
    
    // Logging and debugging
    if (player->IsGameMaster())
    {
        player->SendSystemMessage(fmt::format("TimeSync: Corrected drift of {}ms", correction));
    }
    
    sLog.outDebug("TimeSync: Corrected %dms drift for player %s", 
                  correction, player->GetName());
}

int32 TimeSyncMgr::GetTimeOffset(ObjectGuid guid) const
{
    std::lock_guard<std::mutex> lock(m_dataMutex);
    auto it = m_playerData.find(guid);
    return it != m_playerData.end() ? it->second.offset : 0;
}
