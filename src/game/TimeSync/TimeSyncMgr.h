#pragma once
#include "Player.h"
#include "TimeSync.h"
#include <unordered_map>
#include <mutex>

class TimeSyncMgr
{
public:
    static TimeSyncMgr* instance();
    
    void Initialize();
    void Update(uint32 diff);
    
    void HandleClientPing(Player* player, uint32 clientTime);
    void HandleClientPong(Player* player, uint32 serverTime);
    int32 GetTimeOffset(ObjectGuid guid) const;
    
    // Configuration
    void SetDriftThreshold(int32 threshold) { m_driftThreshold = threshold; }
    void SetMaxSkipMultiplier(float multiplier) { m_maxSkipMultiplier = multiplier; }
    void SetPingInterval(uint32 interval) { m_pingInterval = interval; }

    // Player-specific data structure
    struct TimeSyncData
    {
        int32 offset = 0;               // Current time offset (ms)
        int32 lastDrift = 0;             // Last recorded drift value (ms)
        uint32 lastPingTime = 0;         // Timestamp of last ping send (ms)
        uint32 nextPingTimer = 0;        // Timer for next ping (ms)
        float kalmanGain = 0.5f;         // Current Kalman gain value
        bool awaitingPong = false;       // Waiting for pong response
    };

private:
    void SendPingRequest(Player* player);
    void ApplyDriftCorrection(Player* player, int32 drift);
    void UpdateOffset(Player* player, int32 newOffset);
    
    std::unordered_map<ObjectGuid, TimeSyncData> m_playerData;
    mutable std::mutex m_dataMutex;
    
    // Configurable values
    int32 m_driftThreshold = 500;        // Max allowed drift before correction (ms)
    float m_maxSkipMultiplier = 4.0f;    // Max correction relative to latency
    uint32 m_pingInterval = 30000;       // Time between ping requests (ms)
};

#define sTimeSyncMgr TimeSyncMgr::instance()
