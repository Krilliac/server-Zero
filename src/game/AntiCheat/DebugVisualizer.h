/*
 * Anti-Cheat / Movement-Validation framework — debug visualizer.
 * Slice 2.
 *
 * Diagnostic aid only: spawns temporary, colour/model-coded gameobjects trailing
 * the player to visualise the detection pipeline (normal movement, time-sync, and
 * each violation kind). Never affects gameplay. Config-gated, OFF by default, and
 * requires the master AntiCheat switch to be on. Intended for test realms (often
 * with playerbots un-exempted via config to drive the detectors).
 */

#ifndef MANGOS_ANTICHEAT_DEBUGVISUALIZER_H
#define MANGOS_ANTICHEAT_DEBUGVISUALIZER_H

#include "Common.h"
#include "AntiCheatDefines.h"

class Player;

namespace DebugVisualizer
{
    // True if the visualizer is active (master AntiCheat + visualizer both on).
    bool Enabled();
    // True if the per-packet movement trace is additionally enabled.
    bool TraceEnabled();

    // Drop a colour-coded marker for a fired violation at (x,y,z).
    void Mark(Player* player, AntiCheatViolationType type, float x, float y, float z);
    // Drop a movement-trace marker (caller rate-limits by distance).
    void Trace(Player* player, AntiCheatMoveState state, float x, float y, float z);
}

#endif // MANGOS_ANTICHEAT_DEBUGVISUALIZER_H
