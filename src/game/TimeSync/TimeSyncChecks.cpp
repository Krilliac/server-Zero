#include "TimeSyncChecks.h"
#include "AntiCheatMgr.h"
#include "Log.h"
#include "World.h"
#include "Spell.h"
#include "SpellMgr.h"

bool TimeSyncChecks::ValidateMovementTime(Player* player, MovementInfo const& moveInfo)
{
    const int32 offset = sTimeSyncMgr->GetTimeOffset(player->GetGUID());
    const uint32 serverTime = WorldTimer::getMSTime();
    const int32 expectedTime = CalculateExpectedTime(player, moveInfo.time);
    const int32 timeDiff = std::abs(static_cast<int32>(serverTime) - expectedTime);

    // Calculate tolerance based on latency and movement type
    uint32 tolerance = player->GetSession()->GetLatency() * 2;
    if (moveInfo.HasMovementFlag(MOVEFLAG_SWIMMING)) {
        tolerance += 50;  // Extra tolerance for swimming
    }

    if (timeDiff > static_cast<int32>(tolerance))
    {
        sAntiCheatMgr->RecordViolation(player, CHEAT_TIME_DESYNC,
            fmt::format("Movement: diff={}ms, max={}ms", timeDiff, tolerance));
        return false;
    }
    return true;
}

bool TimeSyncChecks::ValidateSpellCastTime(Player* player, uint32 spellId, uint32 castTime)
{
    const SpellEntry* spellInfo = sSpellStore.LookupEntry(spellId);
    if (!spellInfo) return true;

    const int32 expectedTime = CalculateExpectedTime(player, castTime);
    const int32 serverTime = WorldTimer::getMSTime();
    const int32 timeDiff = std::abs(serverTime - expectedTime);

    if (timeDiff > SPELL_TIME_TOLERANCE)
    {
        sAntiCheatMgr->RecordViolation(player, CHEAT_TIME_DESYNC,
            fmt::format("Spell {}: diff={}ms", spellId, timeDiff));
        return false;
    }
    return true;
}

bool TimeSyncChecks::ValidateActionTime(Player* player, uint32 action, uint32 timestamp)
{
    const int32 expectedTime = CalculateExpectedTime(player, timestamp);
    const int32 serverTime = WorldTimer::getMSTime();
    const int32 timeDiff = std::abs(serverTime - expectedTime);

    // Different tolerances for different actions
    uint32 tolerance = ACTION_TIME_TOLERANCE;
    if (action == CMSG_ATTACKSWING) {
        tolerance = 100;  // Tighter tolerance for combat actions
    }

    if (timeDiff > static_cast<int32>(tolerance))
    {
        sAntiCheatMgr->RecordViolation(player, CHEAT_TIME_DESYNC,
            fmt::format("Action {}: diff={}ms", action, timeDiff));
        return false;
    }
    return true;
}

int32 TimeSyncChecks::CalculateExpectedTime(Player* player, uint32 clientTime)
{
    return static_cast<int32>(clientTime) + sTimeSyncMgr->GetTimeOffset(player->GetGUID());
}

void TimeSyncChecks::HandleTimeDrift(Player* player, int32 drift)
{
    if (std::abs(drift) > MAX_TIME_DRIFT)
    {
        sTimeSyncMgr->ApplyDriftCorrection(player, drift);

        // Kick players with excessive drift
        if (std::abs(drift) > MAX_TIME_SKIP * 2)
        {
            player->GetSession()->KickPlayer();
            sLog.outError("TimeSync: Kicked %s for excessive time drift (%dms)", 
                          player->GetName(), drift);
        }
    }
}
