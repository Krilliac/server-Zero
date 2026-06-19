/*
 * GM/dev commands for the Movement subsystem: .movement status/config/set.
 * Get/inspect movement state, and live-tune the movement config (smoothing +
 * global player speed rate), mirroring the .timesync command pattern.
 */

#include "Chat.h"
#include "Player.h"
#include "World.h"
#include "ObjectAccessor.h"
#include "Log.h"

#include <cstring>
#include <cctype>
#include <cstdlib>

bool ChatHandler::HandleMovementStatusCommand(char* /*args*/)
{
    Player* t = getSelectedPlayer();
    if (!t)
        t = m_session ? m_session->GetPlayer() : NULL;
    if (!t)
    {
        SendSysMessage(".movement status FAILED: no player. Select/target a player or run in-game.");
        SetSentErrorMessage(true);
        return false;
    }

    PSendSysMessage("Movement %s: pos(%.1f, %.1f, %.1f)  run=%.1f walk=%.1f swim=%.1f  flags=0x%X",
                    t->GetName(), t->GetPositionX(), t->GetPositionY(), t->GetPositionZ(),
                    t->GetSpeed(MOVE_RUN), t->GetSpeed(MOVE_WALK), t->GetSpeed(MOVE_SWIM),
                    t->m_movementInfo.GetMovementFlags());
    return true;
}

bool ChatHandler::HandleMovementConfigCommand(char* /*args*/)
{
    PSendSysMessage("Movement config: smoothing=%u heartbeatMs=%u maxExtrapolateMs=%u playerSpeedRate=%u%%",
                    (uint32)sWorld.getConfig(CONFIG_BOOL_MOVEMENT_SMOOTHING),
                    sWorld.getConfig(CONFIG_UINT32_MOVEMENT_HEARTBEAT_MS),
                    sWorld.getConfig(CONFIG_UINT32_MOVEMENT_MAX_EXTRAPOLATE_MS),
                    sWorld.getConfig(CONFIG_UINT32_MOVEMENT_SPEED_RATE));
    SendSysMessage("Set at runtime with .movement set <field> <value> (reverts on restart/reload).");
    return true;
}

bool ChatHandler::HandleMovementSetCommand(char* args)
{
    char* f = strtok(args, " ");
    char* v = strtok(NULL, " ");
    if (!f || !v)
    {
        SendSysMessage(".movement set FAILED. Usage: .movement set <field> <value>. Fields:");
        SendSysMessage("  smoothing (0/1), heartbeatms (100-2000), maxextrapolatems (100-3000),");
        SendSysMessage("  speedrate (10-1000, percent of normal player speed; applies live)");
        SetSentErrorMessage(true);
        return false;
    }
    std::string field = f;
    for (size_t i = 0; i < field.size(); ++i) field[i] = (char)tolower(field[i]);
    int32 val = atoi(v);

    if (field == "smoothing")
    {
        sWorld.setConfig(CONFIG_BOOL_MOVEMENT_SMOOTHING, val != 0);
    }
    else if (field == "heartbeatms")
    {
        if (val < 100) val = 100; if (val > 2000) val = 2000;
        sWorld.setConfig(CONFIG_UINT32_MOVEMENT_HEARTBEAT_MS, uint32(val));
    }
    else if (field == "maxextrapolatems")
    {
        if (val < 100) val = 100; if (val > 3000) val = 3000;
        sWorld.setConfig(CONFIG_UINT32_MOVEMENT_MAX_EXTRAPOLATE_MS, uint32(val));
    }
    else if (field == "speedrate")
    {
        if (val < 10) val = 10; if (val > 1000) val = 1000;
        sWorld.setConfig(CONFIG_UINT32_MOVEMENT_SPEED_RATE, uint32(val));
        // Apply live: refresh every online player's speeds (forced => clients told).
        sObjectAccessor.DoForAllPlayers([](Player* p)
        {
            if (!p) return;
            for (int mt = 0; mt < MAX_MOVE_TYPE; ++mt)
                p->UpdateSpeed(UnitMoveType(mt), true);
        });
    }
    else
    {
        PSendSysMessage(".movement set FAILED: unknown field '%s'.", field.c_str());
        SetSentErrorMessage(true);
        return false;
    }
    PSendSysMessage("Movement: set %s = %d (runtime; reverts on restart/reload from file).", field.c_str(), val);
    return true;
}
