/*
 * Server-side debug visualization toolkit.
 *
 * Engine-style "debug draw", but rendered server-side by spawning temporary
 * gameobjects the client can see. General purpose: anti-cheat movement traces,
 * map cells/grid, line-of-sight, pathfinding, collision raycasts, terrain
 * height/liquid — anything server-authoritative we can place a marker for.
 *
 * Diagnostic only (markers are non-interactive generic gameobjects; no gameplay
 * effect) and GM-driven via the `.debug` commands. Markers auto-despawn.
 */

#ifndef MANGOS_DEBUGVIS_H
#define MANGOS_DEBUGVIS_H

#include "Common.h"

class Player;

namespace DebugVis
{
    // Visual categories -> colour/model (mapped to gameobject entries internally).
    enum Category
    {
        DV_GENERIC = 0,
        DV_CELL,        // map grid / cell boundaries
        DV_LOS_OK,      // line of sight: clear segment
        DV_LOS_BLOCK,   // line of sight: blocked / hit point
        DV_PATH,        // pathfinding: a navmesh path point
        DV_PATH_BAD,    // pathfinding: incomplete / no path
        DV_COLLISION,   // collision raycast hit
        DV_HEIGHT       // terrain / liquid height sample
    };

    // Marker lifetime (seconds) from config.
    uint32 DespawnSeconds();

    // Spawn a single marker at (x,y,z), summoned into the viewer's map so the
    // viewer (and nearby players) can see it. Returns false if it couldn't spawn.
    bool Marker(Player* viewer, Category cat, float x, float y, float z);

    // Spawn markers along the 3D segment p1->p2 every `spacing` yards (raw 3D, not
    // ground-snapped — used for LoS/collision rays). Returns markers placed.
    uint32 Line(Player* viewer, Category cat,
                float x1, float y1, float z1, float x2, float y2, float z2,
                float spacing);
}

#endif // MANGOS_DEBUGVIS_H
