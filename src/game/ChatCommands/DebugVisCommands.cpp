/*
 * GM chat commands for the server-side debug visualization toolkit:
 *   .debug vis cells|los|path|collision|height
 * Each draws a one-shot, auto-despawning set of markers (engine-style debug draw).
 */

#include "Chat.h"
#include "DebugVis.h"
#include "PerformanceMonitor.h"
#include "Player.h"
#include "Map.h"
#include "World.h"
#include "GridDefines.h"
#include "PathFinder.h"

#include <cmath>
#include <cstdlib>

bool ChatHandler::HandleDebugVisCellsCommand(char* args)
{
    Player* player = m_session ? m_session->GetPlayer() : NULL;
    if (!player)
        return false;

    int radius = 2;
    if (args && *args)
        radius = atoi(args);
    if (radius < 1) radius = 1;
    if (radius > 5) radius = 5; // (2r+1)^2 markers cap

    Map* map = player->GetMap();
    const float cell = SIZE_OF_GRID_CELL;
    // Snap to the cell lattice so markers line up with cell spacing.
    float baseX = floor(player->GetPositionX() / cell) * cell;
    float baseY = floor(player->GetPositionY() / cell) * cell;

    uint32 placed = 0;
    for (int i = -radius; i <= radius; ++i)
    {
        for (int j = -radius; j <= radius; ++j)
        {
            float wx = baseX + i * cell;
            float wy = baseY + j * cell;
            float wz = map->GetHeight(wx, wy, player->GetPositionZ() + 5.0f);
            if (wz < -50000.0f)
                wz = player->GetPositionZ();
            if (DebugVis::Marker(player, DebugVis::DV_CELL, wx, wy, wz))
                ++placed;
        }
    }
    PSendSysMessage("DebugVis: drew %u cell-grid markers (cell=%.1f yd, radius=%d). Despawn in %us.",
                    placed, cell, radius, DebugVis::DespawnSeconds());
    return true;
}

bool ChatHandler::HandleDebugVisLosCommand(char* /*args*/)
{
    Player* player = m_session ? m_session->GetPlayer() : NULL;
    if (!player)
        return false;

    Unit* target = getSelectedUnit();
    if (!target)
    {
        SendSysMessage("DebugVis: select a target unit first.");
        SetSentErrorMessage(true);
        return false;
    }

    Map* map = player->GetMap();
    float x1 = player->GetPositionX(), y1 = player->GetPositionY(), z1 = player->GetPositionZ() + 2.0f;
    float x2 = target->GetPositionX(), y2 = target->GetPositionY(), z2 = target->GetPositionZ() + 2.0f;

    if (map->IsInLineOfSight(x1, y1, z1, x2, y2, z2))
    {
        DebugVis::Line(player, DebugVis::DV_LOS_OK, x1, y1, z1, x2, y2, z2, 2.0f);
        SendSysMessage("DebugVis: line of sight is CLEAR (green).");
    }
    else
    {
        float hx = x2, hy = y2, hz = z2;
        map->GetHitPosition(x1, y1, z1, hx, hy, hz, -0.5f);
        DebugVis::Line(player, DebugVis::DV_LOS_BLOCK, x1, y1, z1, hx, hy, hz, 2.0f);
        DebugVis::Marker(player, DebugVis::DV_LOS_BLOCK, hx, hy, hz);
        PSendSysMessage("DebugVis: line of sight BLOCKED (red); hit at (%.1f, %.1f, %.1f).", hx, hy, hz);
    }
    return true;
}

bool ChatHandler::HandleDebugVisPathCommand(char* /*args*/)
{
    Player* player = m_session ? m_session->GetPlayer() : NULL;
    if (!player)
        return false;

    Unit* target = getSelectedUnit();
    if (!target)
    {
        SendSysMessage("DebugVis: select a target unit first.");
        SetSentErrorMessage(true);
        return false;
    }

    PathFinder pf(player);
    pf.calculate(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
    PathType type = pf.getPathType();
    PointsArray& pts = pf.getPath();

    DebugVis::Category cat = (type & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_NOT_USING_PATH))
                             ? DebugVis::DV_PATH_BAD : DebugVis::DV_PATH;

    uint32 placed = 0;
    for (PointsArray::const_iterator it = pts.begin(); it != pts.end(); ++it)
        if (DebugVis::Marker(player, cat, it->x, it->y, it->z))
            ++placed;

    PSendSysMessage("DebugVis: path type=0x%X, %u points (%s).",
                    uint32(type), placed, cat == DebugVis::DV_PATH ? "green=ok" : "red=incomplete/none");
    return true;
}

bool ChatHandler::HandleDebugVisCollisionCommand(char* args)
{
    Player* player = m_session ? m_session->GetPlayer() : NULL;
    if (!player)
        return false;

    float dist = 40.0f;
    if (args && *args)
    {
        float v = (float)atof(args);
        if (v > 1.0f && v < 300.0f)
            dist = v;
    }

    Map* map = player->GetMap();
    float o = player->GetOrientation();
    float x1 = player->GetPositionX(), y1 = player->GetPositionY(), z1 = player->GetPositionZ() + 2.0f;
    float x2 = x1 + cos(o) * dist, y2 = y1 + sin(o) * dist, z2 = z1;

    float hx = x2, hy = y2, hz = z2;
    if (map->GetHitPosition(x1, y1, z1, hx, hy, hz, -0.5f))
    {
        DebugVis::Line(player, DebugVis::DV_COLLISION, x1, y1, z1, hx, hy, hz, 2.0f);
        DebugVis::Marker(player, DebugVis::DV_COLLISION, hx, hy, hz);
        PSendSysMessage("DebugVis: collision at (%.1f, %.1f, %.1f), %.1f yd ahead.",
                        hx, hy, hz, sqrtf((hx - x1) * (hx - x1) + (hy - y1) * (hy - y1)));
    }
    else
    {
        DebugVis::Line(player, DebugVis::DV_COLLISION, x1, y1, z1, x2, y2, z2, 2.0f);
        PSendSysMessage("DebugVis: no collision within %.0f yd ahead.", dist);
    }
    return true;
}

bool ChatHandler::HandleDebugPerfCommand(char* args)
{
    if (args && *args && (*args == 'r' || *args == 'R'))
    {
        PerformanceMonitor::Reset();
        SendSysMessage("PerformanceMonitor: stats reset.");
        return true;
    }

    uint32 ticks, avgMs, maxMs, lastMs;
    PerformanceMonitor::GetStats(ticks, avgMs, maxMs, lastMs);
    PSendSysMessage("Server perf: world ticks=%u  avg=%ums  max=%ums  last=%ums  (.debug perf r to reset)",
                    ticks, avgMs, maxMs, lastMs);
    return true;
}

bool ChatHandler::HandleDebugVisHeightCommand(char* /*args*/)
{
    Player* player = m_session ? m_session->GetPlayer() : NULL;
    if (!player)
        return false;

    Map* map = player->GetMap();
    float px = player->GetPositionX(), py = player->GetPositionY(), pz = player->GetPositionZ();
    float groundZ = map->GetHeight(px, py, pz);

    if (groundZ < -50000.0f)
    {
        SendSysMessage("DebugVis: no terrain height data at this position.");
        return true;
    }

    DebugVis::Marker(player, DebugVis::DV_HEIGHT, px, py, groundZ);
    PSendSysMessage("DebugVis: ground Z=%.2f, you Z=%.2f (delta %.2f).", groundZ, pz, pz - groundZ);
    return true;
}
