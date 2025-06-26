#include "AntiCheatMgr.h"
#include "MovementChecks.h"          // New movement validation system
#include "TimeSync/TimeSyncMgr.h"    // Time synchronization
#include "Log.h"
#include "Config/Config.h"
#include "World.h"
#include "Player.h"
#include "WardenVisualizer.h"
#include <ctime>
#include <algorithm>
#include <iomanip>

AntiCheatMgr* AntiCheatMgr::instance()
{
    static AntiCheatMgr instance;
    return &instance;
}

void AntiCheatMgr::Initialize()
{
    // Load configuration
    m_speedTolerance = sConfig.GetFloatDefault("AntiCheat.SpeedTolerance", 1.25f);
    m_driftThreshold = sConfig.GetIntDefault("AntiCheat.DriftThreshold", 500);
    m_maxFallDistance = sConfig.GetFloatDefault("AntiCheat.MaxFallDistance", 50.0f);
    m_kickThreshold = sConfig.GetIntDefault("AntiCheat.KickThreshold", 3);
    m_botDetectionEnabled = sConfig.GetBoolDefault("AntiCheat.BotDetection", true);
    
    // Initialize subsystems
    MovementChecks::Initialize();
    TimeSyncMgr::Initialize();
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
    
    // Update subsystems
    MovementChecks::Update(diff);
    TimeSyncMgr::Update(diff);
}

void AntiCheatMgr::RecordViolation(Player* player, CheatType type, const std::string& details)
{
    if (player->IsGameMaster()) {
        return; // Ignore violations from GMs
    }
    
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
    if (Player* gm = FindAvailableGM()) {
        WardenVisualizer::ShowViolation(gm, player, type);
    }
    
    // Detailed logging
    LogViolation(player, record);
    
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
        case CHEAT_BOT:  // Added bot detection
            ApplyPenalty(player, type);
            break;
            
        // Moderate cheats: check frequency
        case CHEAT_SPEED_HACK:
        case CHEAT_SWIMMING_HACK:
        case CHEAT_TIME_DESYNC:
        {
            uint32 recentCount = CountRecentViolations(player, type, 60); // Last minute
            if (recentCount >= m_kickThreshold) {
                ApplyPenalty(player, type);
            }
            break;
        }
            
        // Minor cheats: warning only
        case CHEAT_WALL_CLIMB:
            player->SendSystemMessage("Collision anomaly detected.");
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
        case CHEAT_BOT:  // Bot detection penalty
            player->GetSession()->KickPlayer("Severe cheat detected");
            sLog.outAntiCheat("Kicked player %s for severe cheat (Type: %s)",
                player->GetName(), CheatTypeToString(type));
            break;
            
        case CHEAT_SWIMMING_HACK:
            player->SetMovement(MOVE_LAND_WALK); // Force to land
            player->SendSystemMessage("Swimming anomaly detected. Movement restricted.");
            break;
            
        case CHEAT_TIME_DESYNC:
            sTimeSyncMgr->ResetForPlayer(player); // Re-sync time
            player->SendSystemMessage("Time synchronization reset.");
            break;
            
        default:
            break;
    }
}

void AntiCheatMgr::ValidateMovement(Player* player)
{
    /**
     * Comprehensive movement validation entry point:
     * 1. Time synchronization
     * 2. Physics validation
     * 3. Anti-cheat checks
     */
    if (player->IsGameMaster() || player->IsBeingTeleported()) {
        return;
    }
    
    // 1. Time synchronization check
    uint32 clientTime = player->GetMovementInfo().GetTime();
    if (!sTimeSyncMgr->ValidateMovementTime(player, clientTime)) {
        int32 drift = sTimeSyncMgr->GetTimeDrift(player->GetGUID());
        RecordViolation(player, CHEAT_TIME_DESYNC, 
            fmt::format("Time drift: {}ms", drift));
    }
    
    // 2. Core movement validation
    MovementChecks::FullMovementCheck(player, player->GetMovementInfo());
}

// ================= HELPER FUNCTIONS ================= //

uint32 AntiCheatMgr::CountRecentViolations(Player* player, CheatType type, uint32 seconds) const
{
    uint32 count = 0;
    const auto& records = m_violations.at(player->GetGUID());
    const uint32 now = static_cast<uint32>(std::time(nullptr));
    
    for (auto rit = records.rbegin(); rit != records.rend(); ++rit) {
        if (rit->type != type) continue;
        if ((now - rit->timestamp) > seconds) break;
        count++;
    }
    return count;
}

Player* AntiCheatMgr::FindAvailableGM() const
{
    for (auto& [guid, player] : sWorld->GetPlayers()) {
        if (player->IsGameMaster() && player->IsVisible()) {
            return player;
        }
    }
    return nullptr;
}

void AntiCheatMgr::LogViolation(Player* player, const CheatRecord& record)
{
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&record.timestamp), "%Y-%m-%d %H:%M:%S");
    
    sLog.outAntiCheat("[%s] %s: %s at %s (Map: %u, Zone: %u)",
        oss.str().c_str(),
        player->GetName(),
        CheatTypeToString(record.type),
        record.details.c_str(),
        player->GetMapId(),
        player->GetZoneId());
}

const char* AntiCheatMgr::CheatTypeToString(CheatType type)
{
    switch (type) {
        case CHEAT_SPEED_HACK:       return "SpeedHack";
        case CHEAT_TELEPORT_HACK:    return "TeleportHack";
        case CHEAT_WALL_CLIMB:       return "WallClimb";
        case CHEAT_FLY_HACK:         return "FlyHack";
        case CHEAT_TIME_DESYNC:      return "TimeDesync";
        case CHEAT_SWIMMING_HACK:    return "SwimmingHack";
        case CHEAT_PHYSICS_HACK:     return "PhysicsHack";
        case CHEAT_SPELL_HACK:       return "SpellHack";
        case CHEAT_BOT:              return "BotPattern"; // New bot detection
        default:                     return "Unknown";
    }
}