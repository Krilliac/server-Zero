/*
 * Server-side debug visualization toolkit — implementation.
 */

#include "DebugVis.h"
#include "Player.h"
#include "World.h"

#include <cmath>

namespace
{
    // Default marker gameobject entries (existing, guaranteed-visible colored
    // Banners + a circle, so no DB changes are needed). Distinct colours per
    // category. These can be lifted into config later if finer control is wanted.
    uint32 EntryFor(DebugVis::Category cat)
    {
        switch (cat)
        {
            case DebugVis::DV_CELL:      return 180773; // blue banner
            case DebugVis::DV_LOS_OK:    return 180774; // green banner
            case DebugVis::DV_LOS_BLOCK: return 180777; // red banner
            case DebugVis::DV_PATH:      return 180774; // green banner
            case DebugVis::DV_PATH_BAD:  return 180777; // red banner
            case DebugVis::DV_COLLISION: return 180776; // purple banner
            case DebugVis::DV_HEIGHT:    return 180775; // pink banner
            case DebugVis::DV_GENERIC:
            default:                     return 180778; // yellow banner
        }
    }
}

namespace DebugVis
{
    uint32 DespawnSeconds()
    {
        return sWorld.getConfig(CONFIG_UINT32_DEBUGVIS_DESPAWN);
    }

    bool Marker(Player* viewer, Category cat, float x, float y, float z)
    {
        if (!viewer || !viewer->IsInWorld())
            return false;

        uint32 despawnMs = DespawnSeconds() * IN_MILLISECONDS;
        return viewer->SummonGameObject(EntryFor(cat), x, y, z, viewer->GetOrientation(), despawnMs) != NULL;
    }

    uint32 Line(Player* viewer, Category cat,
                float x1, float y1, float z1, float x2, float y2, float z2,
                float spacing)
    {
        if (!viewer || !viewer->IsInWorld())
            return 0;
        if (spacing < 0.5f)
            spacing = 0.5f;

        float dx = x2 - x1, dy = y2 - y1, dz = z2 - z1;
        float len = sqrtf(dx * dx + dy * dy + dz * dz);
        uint32 steps = uint32(len / spacing);
        if (steps > 200) // safety cap so a long ray can't flood the world
            steps = 200;

        uint32 placed = 0;
        for (uint32 i = 0; i <= steps; ++i)
        {
            float t = steps ? float(i) / float(steps) : 0.0f;
            if (Marker(viewer, cat, x1 + dx * t, y1 + dy * t, z1 + dz * t))
                ++placed;
        }
        return placed;
    }
}
