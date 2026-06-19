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

        // Movement-sync clock service: smoothed offset between the server clock
        // (getMSTime) and the client's reported movement timestamps. Its drift over
        // time is the desync signal; consumers (detection + optional relay
        // correction) read it. Valid once HasClockOffset() is true.
        bool   HasClockOffset() const { return m_hasClockOffset; }
        int64  GetClockOffsetMs() const { return m_clockOffsetMs; }

        // Called when the SERVER relocates the player (teleport ack, map change)
        // so the next client packet is trusted and the baseline is rebuilt
        // instead of being scored as an impossible jump.
        void NotifyServerRelocation() { m_trustNext = true; }

        // Called when the client reports a movement time skip
        // (CMSG_MOVE_TIME_SKIPPED) — a legitimate client clock jump after a freeze.
        // Re-baselines the clock service + arms a grace window so the same event
        // isn't double-scored as desync, AND scores the skip itself for abuse
        // (oversized / spammed skips are a time-based speed/teleport-mask vector).
        void NotifyClientTimeSkip(uint32 skippedMs);

        // Time-based anti-cheat for the spell-cast path (CMSG_CAST_SPELL): detects
        // GCD bypass (casts closer together than the spell's global cooldown allows)
        // and cast spam. castTimeMs/gcdMs come from the spell entry.
        void NotifySpellCast(uint32 spellId, uint32 castTimeMs, uint32 gcdMs);

        // Time-based anti-cheat for movement ACK packets (force-speed-change,
        // water-walk): flags client-timestamp regression in the ack (manipulated
        // ack timing) and folds the sample into the clock-offset service.
        void NotifyMoveAckTime(uint32 clientTime);

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

        // Movement-sync clock-offset service (smoothed server-vs-client clock).
        bool   m_hasClockOffset;
        int64  m_clockOffsetMs;

        // Time-sync / desync-resync state.
        uint32 m_timeSkipGraceUntilMS; // suppress per-packet desync scoring until here
        uint32 m_desyncStreak;         // consecutive desync trips (auto-resync trigger)
        uint32 m_lastResyncMS;         // last auto-resync server time (cooldown)

        // CMSG_MOVE_TIME_SKIPPED abuse window (frequency + accumulation).
        uint32 m_skipWinStartMS;
        uint32 m_skipCount;
        uint32 m_skipAccumMs;

        // Spell-cast timing (GCD bypass + cast spam).
        bool   m_hasLastCast;
        uint32 m_lastCastMS;
        uint32 m_lastCastGcd;
        uint32 m_castWinStartMS;
        uint32 m_castCount;

        // Movement-ACK client-timestamp tracking (kept separate from the movement
        // path's m_lastClientTime so it can't skew the per-packet desync delta).
        bool   m_hasAckTime;
        uint32 m_lastAckTime;
};

#endif // MANGOS_ANTICHEAT_MOVEMENTANTICHEAT_H
