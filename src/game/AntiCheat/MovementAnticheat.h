/*
 * Anti-Cheat / Movement-Validation framework — per-player movement validator.
 * Slice 1 (core detection pipeline).
 *
 * One instance is owned by each Player. It tracks the last accepted movement
 * snapshot, normalises move state, runs the cheap event-driven detectors on each
 * movement packet, and reports trips to AntiCheatMgr (which owns all response).
 * It NEVER punishes or rejects packets itself — Slice 1 is observe + score.
 */

#ifndef MANGOS_ANTICHEAT_MOVEMENTANTICHEAT_H
#define MANGOS_ANTICHEAT_MOVEMENTANTICHEAT_H

#include "Common.h"
#include "AntiCheatDefines.h"

class Player;
class MovementInfo;

class MovementAnticheat
{
    public:
        explicit MovementAnticheat(Player* owner);

        // Main entry: called from the movement opcode handler after the packet
        // is parsed and before it is applied. opcode is the movement opcode.
        void HandlePositionUpdate(uint16 opcode, MovementInfo const& mi);

        // Periodic (timer-driven from Player::Update) re-validation of the player's
        // current position — a second cadence that catches static exploits where no
        // movement packets are sent (e.g. standing inside geometry).
        void PeriodicCheck();

        // Called when the SERVER relocates the player (teleport ack, map change)
        // so the next client packet is trusted and the baseline is rebuilt
        // instead of being scored as an impossible jump.
        void NotifyServerRelocation() { m_trustNext = true; }

        // Last position that passed the teleport/physics gates (rubberband target
        // for the enforcement slice). Valid only if HasValid() is true.
        bool HasValid() const { return m_hasValid; }
        float ValidX() const { return m_validX; }
        float ValidY() const { return m_validY; }
        float ValidZ() const { return m_validZ; }
        float ValidO() const { return m_validO; }

    private:
        AntiCheatMoveState NormalizeState(MovementInfo const& mi) const;

        Player* m_player;

        bool   m_hasLast;
        bool   m_trustNext;
        float  m_lastX, m_lastY, m_lastZ, m_lastO;
        uint32 m_lastMS;
        uint32 m_lastFlags;

        bool   m_hasValid;
        float  m_validX, m_validY, m_validZ, m_validO;

        // Debug-visualizer movement trace (distance rate-limited).
        bool   m_hasTrace;
        float  m_traceX, m_traceY, m_traceZ;

        // Jump / fall state machine (infinite-jump + fall-damage-suppression).
        bool   m_airborne;     // in a jump/fall episode (no FALL_LAND yet)
        float  m_fallApexZ;    // highest Z reached during the current airborne episode

        // Packet-burst + client-timestamp tracking.
        uint32 m_burstWinStartMS;  // start of the current 1s burst-count window
        uint32 m_burstCount;       // movement packets seen in the current window
        uint32 m_lastClientTime;   // last MovementInfo client timestamp
        bool   m_hasClientTime;
};

#endif // MANGOS_ANTICHEAT_MOVEMENTANTICHEAT_H
