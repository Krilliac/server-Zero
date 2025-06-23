#pragma once
#include "Player.h"
#include "TimeSyncMgr.h"
#include "MovementInfo.h"

class TimeSyncChecks
{
public:
    /**
     * Validates movement packet timing against time sync data
     * 
     * @param player Player object
     * @param moveInfo Movement information
     * @return true if timing is valid, false if cheating detected
     */
    static bool ValidateMovementTime(Player* player, MovementInfo const& moveInfo);
    
    /**
     * Validates spell cast timing
     * 
     * @param player Player object
     * @param spellId Spell being cast
     * @param castTime Client-reported cast time
     * @return true if timing is valid
     */
    static bool ValidateSpellCastTime(Player* player, uint32 spellId, uint32 castTime);
    
    /**
     * Validates action timing (attack, use item, etc)
     * 
     * @param player Player object
     * @param action Action opcode
     * @param timestamp Client-reported timestamp
     * @return true if timing is valid
     */
    static bool ValidateActionTime(Player* player, uint32 action, uint32 timestamp);
    
    /**
     * Handles excessive time drift
     * 
     * @param player Player object
     * @param drift Measured time drift
     */
    static void HandleTimeDrift(Player* player, int32 drift);

private:
    static constexpr int32 MAX_TIME_DRIFT = 500;        // Max allowed drift (ms)
    static constexpr int32 MAX_TIME_SKIP = 250;          // Max safe correction (ms)
    static constexpr int32 SPELL_TIME_TOLERANCE = 50;    // Spell timing tolerance (ms)
    static constexpr int32 ACTION_TIME_TOLERANCE = 150;  // Action timing tolerance (ms)
    
    static int32 CalculateExpectedTime(Player* player, uint32 clientTime);
};
