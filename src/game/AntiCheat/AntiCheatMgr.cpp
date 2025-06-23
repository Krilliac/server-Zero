#include "AntiCheatMgr.h"
#include "PhysicsValidator.h"
#include "TimeSyncChecks.h"
#include "MovementChecks.h"
#include "Log.h"
#include "Config.h"
#include "World.h"
#include "Player.h"
#include "WardenVisualizer.h"
#include <ctime>
#include <algorithm>

AntiCheatMgr* AntiCheatMgr::instance()
{
    static AntiCheatMgr instance;
    return &instance;
}

void AntiCheatMgr::Initialize()
{
    m_speedTolerance = sConfig->GetFloatDefault("AntiCheat.SpeedTolerance", 1.2f);
    m_driftThreshold = sConfig->GetIntDefault("AntiCheat.DriftThreshold", 50);
    m_maxFallDistance = sConfig->GetFloatDefault("AntiCheat.MaxFallDistance", 50.0f);
    m_kickThreshold = sConfig->GetIntDefault("AntiCheat.KickThreshold", 5);
}

void AntiCheatMgr::Update(uint32 diff)
{
    // Periodic cleanup of old violations
    static uint32 cleanupTimer = 0;
    cleanupTimer += diff;
    if (cleanupTimer >= 60000) // Every minute
    {
        std::lock_guard<std::mutex> lock(m_violationsMutex);
        const uint32 now = static_cast<uint32>(std::time(nullptr));
        
        for (auto& [guid, records] : m_violations) {
            records.erase(
                std::remove_if(records.begin(), records.end(),
                    [now](const CheatRecord& r) {
                        return (now - r.timestamp) > 300; // 5 minutes
                    }),
                records.end()
            );
        }
        cleanupTimer = 0;
    }
}

void AntiCheatMgr::RecordViolation(Player* player, CheatType type, const std::string& details)
{
    std::lock_guard<std::mutex> lock(m_violationsMutex);
    ObjectGuid guid = player->GetGUID();
    
    CheatRecord record{
        type,
        details,
        static_cast<uint32>(std::time(nullptr)),
        player->GetPosition()
    };
    
    m_violations[guid].push_back(record);
    m_totalViolations++;
    m_detectedCheats++;
    
    // Visual feedback for GMs
    WardenVisualizer::ShowViolation(player, type);
    
    // Log with context
    sLog.outAntiCheat("Violation: %s - %s (Type: %d) at %s",
        player->GetName(), details.c_str(), static_cast<int>(type),
        record.location.ToString().c_str());
    
    // Handle based on severity
    HandleViolation(player, type);
}

void AntiCheatMgr::HandleViolation(Player* player, CheatType type)
{
    switch (type)
    {
        // Severe cheats: immediate action
        case CHEAT_TELEPORT_HACK:
        case CHEAT_FLY_HACK:
        case CHEAT_PHYSICS_HACK:
            ApplyPenalty(player, type);
            break;
            
        // Moderate cheats: check frequency
        case CHEAT_SPEED_HACK:
        case CHEAT_SWIMMING_HACK:
        {
            uint32 recentCount = 0;
            auto& records = m_violations[player->GetGUID()];
            const uint32 now = static_cast<uint32>(std::time(nullptr));
            
            for (auto rit = records.rbegin(); rit != records.rend(); ++rit) {
                if (rit->type != type) continue;
                if ((now - rit->timestamp) > 60) break; // Only last minute
                if (++recentCount >= m_kickThreshold) {
                    ApplyPenalty(player, type);
                    break;
                }
            }
            break;
        }
            
        // Minor cheats: warning only
        case CHEAT_TIME_DESYNC:
            player->SendSystemMessage("Time synchronization anomaly detected.");
            break;
            
        default:
            break;
    }
}

void AntiCheatMgr::ApplyPenalty(Player* player, CheatType type)
{
    switch (type)
    {
        case CHEAT_SPEED_HACK:
            player->TeleportToLastSafePosition();
            player->SendSystemMessage("Speed anomaly detected. Teleported to safe location.");
            break;
            
        case CHEAT_TELEPORT_HACK:
        case CHEAT_FLY_HACK:
        case CHEAT_PHYSICS_HACK:
            player->GetSession()->KickPlayer();
            sLog.outAntiCheat("Kicked player %s for severe cheat (Type: %d)",
                player->GetName(), static_cast<int>(type));
            break;
            
        case CHEAT_SWIMMING_HACK:
            player->SetMovement(MOVE_LAND_WALK); // Force to land
            player->SendSystemMessage("Swimming anomaly detected. Movement restricted.");
            break;
            
        default:
            break;
    }
}

const std::vector<CheatRecord>& AntiCheatMgr::GetViolations(Player* player) const
{
    static std::vector<CheatRecord> empty;
    std::lock_guard<std::mutex> lock(m_violationsMutex);
    
    auto it = m_violations.find(player->GetGUID());
    return it != m_violations.end() ? it->second : empty;
}

uint32 AntiCheatMgr::GetTotalViolations() const
{
    return m_totalViolations;
}

float AntiCheatMgr::GetDetectionRate() const
{
    // Simplified metric: detected cheats vs total players
    uint32 playerCount = sWorld->GetPlayerCount();
    if (playerCount == 0) return 0.0f;
    
    return (static_cast<float>(m_detectedCheats) / playerCount) * 100.0f;
}

void AntiCheatMgr::ClearViolations(Player* player)
{
    std::lock_guard<std::mutex> lock(m_violationsMutex);
    m_violations.erase(player->GetGUID());
}

void AntiCheatMgr::LogViolation(Player* player, const CheatRecord& record)
{
    sLog.outAntiCheat("[%s] %s: %s",
        player->GetName(),
        CheatTypeToString(record.type),
        record.details.c_str());
}

// Helper function (would be in a separate utils file)
const char* CheatTypeToString(CheatType type)
{
    switch (type) {
        case CHEAT_SPEED_HACK: return "SpeedHack";
        case CHEAT_TELEPORT_HACK: return "TeleportHack";
        case CHEAT_WALL_CLIMB: return "WallClimb";
        case CHEAT_FLY_HACK: return "FlyHack";
        case CHEAT_TIME_DESYNC: return "TimeDesync";
        case CHEAT_SWIMMING_HACK: return "SwimmingHack";
        case CHEAT_PHYSICS_HACK: return "PhysicsHack";
        case CHEAT_SPELL_HACK: return "SpellHack";
        default: return "Unknown";
    }
}
