/*
 * Anti-Cheat / Movement-Validation framework — per-player movement validator.
 * Slice 1 (core detection pipeline).
 */

#include "MovementAnticheat.h"
#include "AntiCheatMgr.h"
#include "PhysicsValidator.h"
#include "Player.h"
#include "Unit.h"
#include "Timer.h"

#include <cmath>

namespace
{
    const float  VERT_CLIMB_SUSPECT = 5.0f;   // yd upward in one packet on ground
    const float  SPEED_SLACK_YD     = 2.0f;   // constant distance fudge per packet
    const uint32 GAP_RESET_MS       = 3000;   // packet gap implying load/teleport
}

MovementAnticheat::MovementAnticheat(Player* owner)
    : m_player(owner), m_hasLast(false), m_trustNext(false),
      m_lastX(0.f), m_lastY(0.f), m_lastZ(0.f), m_lastO(0.f),
      m_lastMS(0), m_lastFlags(0),
      m_hasValid(false), m_validX(0.f), m_validY(0.f), m_validZ(0.f), m_validO(0.f)
{
}

AntiCheatMoveState MovementAnticheat::NormalizeState(MovementInfo const& mi) const
{
    if (mi.HasMovementFlag(MOVEFLAG_ONTRANSPORT))
        return AC_MOVE_TRANSPORT;
    if (mi.HasMovementFlag(MOVEFLAG_SWIMMING))
        return AC_MOVE_SWIM;
    if (mi.HasMovementFlag(MovementFlags(MOVEFLAG_FALLING | MOVEFLAG_FALLINGFAR)))
        return AC_MOVE_FALL;
    if (mi.HasMovementFlag(MovementFlags(MOVEFLAG_FLYING | MOVEFLAG_CAN_FLY | MOVEFLAG_LEVITATING)))
        return AC_MOVE_FLY;
    return AC_MOVE_GROUND;
}

void MovementAnticheat::HandlePositionUpdate(uint16 /*opcode*/, MovementInfo const& mi)
{
    if (!m_player || !sAntiCheatMgr->MovementEnabled())
        return;

    uint32 nowMS = getMSTime();
    Position const* pos = mi.GetPos();
    AntiCheatMoveState state = NormalizeState(mi);

    // (Re)establish baseline on first packet, after a server relocation, after a
    // long gap (loading screen / teleport), or while on a taxi spline.
    if (!m_hasLast || m_trustNext ||
        getMSTimeDiff(m_lastMS, nowMS) > GAP_RESET_MS || m_player->IsTaxiFlying())
    {
        m_trustNext = false;
        m_hasLast = true;
        m_lastX = pos->x; m_lastY = pos->y; m_lastZ = pos->z; m_lastO = pos->o;
        m_lastMS = nowMS; m_lastFlags = mi.GetMovementFlags();
        m_hasValid = true;
        m_validX = pos->x; m_validY = pos->y; m_validZ = pos->z; m_validO = pos->o;
        return;
    }

    uint32 dtMS = getMSTimeDiff(m_lastMS, nowMS);
    if (dtMS == 0)
        dtMS = 1;
    float dtSec = float(dtMS) / 1000.0f;

    float dx = pos->x - m_lastX;
    float dy = pos->y - m_lastY;
    float dz = pos->z - m_lastZ;
    float horiz = sqrtf(dx * dx + dy * dy);

    uint32 latency = m_player->GetSession() ? m_player->GetSession()->GetLatencyEWMA() : 0;

    // Choose the relevant speed for the current state.
    UnitMoveType mtype = MOVE_RUN;
    if (state == AC_MOVE_SWIM)
        mtype = MOVE_SWIM;
    else if (mi.HasMovementFlag(MOVEFLAG_WALK_MODE))
        mtype = MOVE_WALK;
    float allowed = m_player->GetSpeed(mtype);

    AntiCheatContext ctx;
    ctx.mapId = m_player->GetMapId();
    ctx.x = pos->x; ctx.y = pos->y; ctx.z = pos->z;
    ctx.speed = horiz / dtSec;
    ctx.latency = latency;

    bool cheapTrip = false;

    // --- Detector: flag contradiction (vanilla players never legitimately fly) ---
    if (mi.HasMovementFlag(MovementFlags(MOVEFLAG_FLYING | MOVEFLAG_CAN_FLY)))
    {
        ctx.detail = "fly movement flag set";
        sAntiCheatMgr->RecordViolation(m_player, AC_VIOLATION_FLAG_CONTRADICT, 40.0f, ctx);
        cheapTrip = true;
    }

    // --- Detector: teleport / blink (single-packet displacement) ---
    float teleMax = float(sAntiCheatMgr->GetTeleportDistance())
                  + allowed * (float(latency) / 1000.0f);
    if (state != AC_MOVE_TRANSPORT && !m_player->IsTaxiFlying() && horiz > teleMax)
    {
        ctx.detail = "teleport/blink jump";
        sAntiCheatMgr->RecordViolation(m_player, AC_VIOLATION_TELEPORT, 25.0f, ctx);
        cheapTrip = true;
    }
    else if (state != AC_MOVE_TRANSPORT)
    {
        // --- Detector: speed (distance vs time, latency-tolerant) ---
        float tol = float(sAntiCheatMgr->GetSpeedTolerancePct()) / 100.0f;
        float expectMax = allowed * tol * dtSec
                        + allowed * (float(latency) / 1000.0f) + SPEED_SLACK_YD;
        if (horiz > expectMax * 1.05f && allowed > 0.0f)
        {
            float ratio = horiz / (expectMax > 0.01f ? expectMax : 0.01f);
            float weight = (ratio - 1.0f) * 50.0f;
            if (weight < 5.0f) weight = 5.0f;
            if (weight > 25.0f) weight = 25.0f;
            ctx.detail = "speed over allowed";
            sAntiCheatMgr->RecordViolation(m_player, AC_VIOLATION_SPEED, weight, ctx);
            cheapTrip = true;
        }
    }

    // --- Detector: unexplained vertical climb on the ground ---
    if (state == AC_MOVE_GROUND && dz > VERT_CLIMB_SUSPECT && horiz < dz &&
        !mi.HasMovementFlag(MovementFlags(MOVEFLAG_FALLING | MOVEFLAG_FALLINGFAR)))
    {
        ctx.detail = "vertical climb without cause";
        sAntiCheatMgr->RecordViolation(m_player, AC_VIOLATION_VERTICAL, 15.0f, ctx);
        cheapTrip = true;
    }

    // --- Physics plausibility (moderate cost): only on suspicion or vertical move ---
    if (sAntiCheatMgr->PhysicsEnabled() && state == AC_MOVE_GROUND &&
        (cheapTrip || fabs(dz) > 2.0f))
    {
        const char* reason = NULL;
        AntiCheatPhysicsResult r = PhysicsValidator::Validate(m_player, state, mi, &reason);
        if (r == AC_PHYS_IMPOSSIBLE)
        {
            ctx.detail = reason ? reason : "physics impossible";
            sAntiCheatMgr->RecordViolation(m_player, AC_VIOLATION_PHYSICS, 30.0f, ctx);
        }
        else if (r == AC_PHYS_SUSPECT)
        {
            ctx.detail = reason ? reason : "physics suspect";
            sAntiCheatMgr->RecordViolation(m_player, AC_VIOLATION_PHYSICS, 10.0f, ctx);
        }
    }

    // Update the rolling baseline. Track last clean position for rubberband use
    // in the enforcement slice (a non-teleport, non-impossible packet).
    m_lastX = pos->x; m_lastY = pos->y; m_lastZ = pos->z; m_lastO = pos->o;
    m_lastMS = nowMS; m_lastFlags = mi.GetMovementFlags();
    if (!cheapTrip)
    {
        m_hasValid = true;
        m_validX = pos->x; m_validY = pos->y; m_validZ = pos->z; m_validO = pos->o;
    }
}
