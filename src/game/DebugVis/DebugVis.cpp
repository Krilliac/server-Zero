/*
 * Server-side debug visualization toolkit — implementation.
 */

#include "DebugVis.h"
#include "Player.h"
#include "World.h"
#include "Config/Config.h"

#include <cmath>
#include <map>

namespace
{
    // Per-category marker MODEL/COLOUR (a GameObjectDisplayInfo.dbc display id),
    // applied per-instance via GAMEOBJECT_DISPLAYID. Two presets selectable with
    // DebugVis.Style (0 = colour-coded light beams [default], 1 = floating
    // crystals); any single category is overridable via DebugVis.Disp.<Cat>.
    uint32 ColorDisplayId(DebugVis::Category cat)
    {
        const bool crystals = sConfig.GetIntDefault("DebugVis.Style", 0) != 0;

        // {beam, crystal} display ids per category.
        uint32 beam, crystal; const char* key;
        switch (cat)
        {
            case DebugVis::DV_CELL:      key = "DebugVis.Disp.Cell";      beam = 263;  crystal = 5811; break; // blue   / dark crystal
            case DebugVis::DV_LOS_OK:    key = "DebugVis.Disp.LosOk";     beam = 3993; crystal = 6431; break; // green  / glyphed crystal
            case DebugVis::DV_LOS_BLOCK: key = "DebugVis.Disp.LosBlock";  beam = 327;  crystal = 6573; break; // red    / red crystal
            case DebugVis::DV_PATH:      key = "DebugVis.Disp.Path";      beam = 3993; crystal = 6431; break; // green  / glyphed crystal
            case DebugVis::DV_PATH_BAD:  key = "DebugVis.Disp.PathBad";   beam = 327;  crystal = 6573; break; // red    / red crystal
            case DebugVis::DV_COLLISION: key = "DebugVis.Disp.Collision"; beam = 363;  crystal = 1667; break; // purple / purple crystal
            case DebugVis::DV_HEIGHT:    key = "DebugVis.Disp.Height";    beam = 266;  crystal = 6570; break; // yellow / silithus crystal
            case DebugVis::DV_HITPOINT:  key = "DebugVis.Disp.HitPoint";  beam = 6430; crystal = 6571; break; // cannon target reticle / broken red crystal
            case DebugVis::DV_GENERIC:
            default:                     key = "DebugVis.Disp.Generic";   beam = 6679; crystal = 5746; break; // glowing circle / crimson shard
        }
        return uint32(sConfig.GetIntDefault(key, int32(crystals ? crystal : beam)));
    }

    // entry -> per-instance tooltip text for spawned pool markers. Touched only
    // from the world/session thread (command handlers + GO-query handler), so no
    // locking needed. Bounded to the pool size (ring reuse overwrites).
    std::map<uint32, std::string>& LabelMap()
    {
        static std::map<uint32, std::string> s_labels;
        return s_labels;
    }

    // Next ring slot. Each labeled marker consumes one pool entry.
    uint32 NextLabeledEntry(const std::string& label)
    {
        static uint32 s_cursor = 0;
        uint32 entry = uint32(DebugVis::DEBUGVIS_ENTRY_BASE) + (s_cursor % DebugVis::DEBUGVIS_ENTRY_COUNT);
        s_cursor = (s_cursor + 1) % DebugVis::DEBUGVIS_ENTRY_COUNT;
        LabelMap()[entry] = label;
        return entry;
    }

    // Shared, never-relabeled pool entry for unlabeled fill markers (ray dots).
    // Uses the very top of the pool so the ring above doesn't clobber it often.
    uint32 SharedFillEntry()
    {
        return uint32(DebugVis::DEBUGVIS_ENTRY_BASE) + uint32(DebugVis::DEBUGVIS_ENTRY_COUNT) - 1;
    }
}

namespace DebugVis
{
    uint32 DespawnSeconds()
    {
        return sWorld.getConfig(CONFIG_UINT32_DEBUGVIS_DESPAWN);
    }

    bool IsDebugEntry(uint32 entry)
    {
        return entry >= uint32(DEBUGVIS_ENTRY_BASE) &&
               entry <  uint32(DEBUGVIS_ENTRY_BASE) + uint32(DEBUGVIS_ENTRY_COUNT);
    }

    bool GetEntryLabel(uint32 entry, std::string& out)
    {
        std::map<uint32, std::string>& m = LabelMap();
        std::map<uint32, std::string>::const_iterator it = m.find(entry);
        if (it == m.end() || it->second.empty())
            return false;
        out = it->second;
        return true;
    }

    bool Marker(Player* viewer, Category cat, float x, float y, float z, const std::string& label)
    {
        if (!viewer || !viewer->IsInWorld())
            return false;

        // Labeled markers each take a distinct pool entry (per-instance tooltip);
        // unlabeled ones share a single entry (the tooltip is irrelevant for them).
        uint32 entry = label.empty() ? SharedFillEntry() : NextLabeledEntry(label);
        uint32 despawnMs = DespawnSeconds() * IN_MILLISECONDS;

        // Colour/model is forced per-instance, independent of the pool template.
        return viewer->SummonGameObject(entry, x, y, z, viewer->GetOrientation(),
                                        despawnMs, ColorDisplayId(cat)) != NULL;
    }

    uint32 Line(Player* viewer, Category cat,
                float x1, float y1, float z1, float x2, float y2, float z2,
                float spacing, const std::string& label)
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
            // Only the first dot carries the (optional) verbose tooltip; the rest
            // are unlabeled fill so we don't burn the whole ring on one ray.
            const std::string& dotLabel = (i == 0) ? label : std::string();
            if (Marker(viewer, cat, x1 + dx * t, y1 + dy * t, z1 + dz * t, dotLabel))
                ++placed;
        }
        return placed;
    }
}
