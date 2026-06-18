/*
 * Anti-Cheat / Movement-Validation framework — debug visualizer implementation.
 * Slice 2.
 */

#include "DebugVisualizer.h"
#include "Player.h"
#include "World.h"
#include "Config/Config.h"
#include "Log.h"

namespace
{
    // Map a violation type to its configured marker gameobject entry.
    uint32 EntryForViolation(AntiCheatViolationType type)
    {
        switch (type)
        {
            case AC_VIOLATION_SPEED:           return sWorld.getConfig(CONFIG_UINT32_ACDBG_GO_SPEED);
            case AC_VIOLATION_TELEPORT:        return sWorld.getConfig(CONFIG_UINT32_ACDBG_GO_TELEPORT);
            case AC_VIOLATION_VERTICAL:        return sWorld.getConfig(CONFIG_UINT32_ACDBG_GO_VERTICAL);
            case AC_VIOLATION_FLAG_CONTRADICT: return sWorld.getConfig(CONFIG_UINT32_ACDBG_GO_FLAG);
            case AC_VIOLATION_PHYSICS:         return sWorld.getConfig(CONFIG_UINT32_ACDBG_GO_PHYSICS);
            case AC_VIOLATION_DESYNC:          return sWorld.getConfig(CONFIG_UINT32_ACDBG_GO_TIMESYNC);
            default:                           return sWorld.getConfig(CONFIG_UINT32_ACDBG_GO_MOVEMENT);
        }
    }

    void Spawn(Player* player, uint32 entry, float x, float y, float z)
    {
        if (!player || !player->IsInWorld() || !entry)
            return;

        uint32 despawnMs = sWorld.getConfig(CONFIG_UINT32_ACDBG_DESPAWN) * IN_MILLISECONDS;
        // Temporary, non-interactive gameobject; auto-despawns. Visible to nearby
        // players. SummonGameObject returns NULL safely on a bad entry.
        if (!player->SummonGameObject(entry, x, y, z, player->GetOrientation(), despawnMs))
            DEBUG_LOG("DebugVisualizer: failed to summon marker GO entry %u (check DebugVisualizer.GO.* config)", entry);
    }
}

namespace DebugVisualizer
{
    bool Enabled()
    {
        return sWorld.getConfig(CONFIG_BOOL_ANTICHEAT_ENABLE) &&
               sWorld.getConfig(CONFIG_BOOL_ACDBG_ENABLE);
    }

    bool TraceEnabled()
    {
        return Enabled() && sWorld.getConfig(CONFIG_BOOL_ACDBG_TRACE);
    }

    void Mark(Player* player, AntiCheatViolationType type, float x, float y, float z)
    {
        if (!Enabled())
            return;
        Spawn(player, EntryForViolation(type), x, y, z);

        // Optional dynamic cue: play a SpellVisualKit on the offender (more
        // visible than a static marker). 0 = off. Configurable kit id.
        uint32 vkit = uint32(sConfig.GetIntDefault("DebugVisualizer.ViolationVisual", 0));
        if (vkit && player && player->IsInWorld())
            player->PlaySpellVisual(vkit);
    }

    void Trace(Player* player, AntiCheatMoveState /*state*/, float x, float y, float z)
    {
        if (!TraceEnabled())
            return;
        Spawn(player, sWorld.getConfig(CONFIG_UINT32_ACDBG_GO_MOVEMENT), x, y, z);
    }
}
