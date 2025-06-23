#pragma once
#include "Player.h"
#include "CheatData.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>

enum CheatType
{
    CHEAT_SPEED_HACK,          // Movement speed exceeds limits
    CHEAT_TELEPORT_HACK,        // Instant position change
    CHEAT_WALL_CLIMB,           // Walking through walls/terrain
    CHEAT_FLY_HACK,             // Flying without permission
    CHEAT_TIME_DESYNC,          // Client time manipulation
    CHEAT_SWIMMING_HACK,        // Swimming exploit detection
    CHEAT_PHYSICS_HACK,         // Physics violation
    CHEAT_SPELL_HACK            // Spell casting exploits
};

struct CheatRecord
{
    CheatType type;
    std::string details;
    uint32 timestamp;
    Position location;
};

class AntiCheatMgr
{
public:
    static AntiCheatMgr* instance();
    
    void Initialize();
    void Update(uint32 diff);
    
    void RecordViolation(Player* player, CheatType type, const std::string& details);
    void HandleViolation(Player* player, CheatType type);
    
    const std::vector<CheatRecord>& GetViolations(Player* player) const;
    uint32 GetTotalViolations() const;
    float GetDetectionRate() const;
    void ClearViolations(Player* player);
    
    // Configuration
    void SetSpeedTolerance(float tolerance) { m_speedTolerance = tolerance; }
    void SetDriftThreshold(int32 threshold) { m_driftThreshold = threshold; }
    void SetMaxFallDistance(float distance) { m_maxFallDistance = distance; }

private:
    void ApplyPenalty(Player* player, CheatType type);
    void LogViolation(Player* player, const CheatRecord& record);
    
    std::unordered_map<ObjectGuid, std::vector<CheatRecord>> m_violations;
    mutable std::mutex m_violationsMutex;
    
    // Metrics
    uint32 m_totalViolations = 0;
    uint32 m_detectedCheats = 0;
    
    // Configurable thresholds
    float m_speedTolerance = 1.2f;
    int32 m_driftThreshold = 50;
    float m_maxFallDistance = 50.0f;
    uint32 m_kickThreshold = 5;
};

#define sAntiCheatMgr AntiCheatMgr::instance()
