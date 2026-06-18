/*
 * Anti-Cheat / Movement-Validation framework — per-player movement validator.
 * Slice 1 (core detection pipeline).
 */

#include "MovementAnticheat.h"
#include "AntiCheatMgr.h"
#include "PhysicsValidator.h"
#include "DebugVisualizer.h"
#include "Player.h"
#include "World.h"
#include "Unit.h"
#include "Opcodes.h"
#include "Timer.h"

#include <cmath>

namespace
{
    const float  VERT_CLIMB_SUSPECT = 5.0f;   // yd upward in one packet on ground
    const float  SPEED_SLACK_YD     = 2.0f;   // constant distance fudge per packet
    const uint32 GAP_RESET_MS       = 3000;   // packet gap implying load/teleport
    const float  FALL_SUPPRESS_YD   = 20.0f;  // drop beyond this should incur fall damage
    const uint32 BURST_PER_SEC      = 50;     // movement packets/sec beyond this = burst
    const uint32 CLIENT_TIME_BACK_MS = 500;   // client timestamp regression tolerance
}

MovementAnticheat::MovementAnticheat(Player* owner)
    : m_player(owner), m_hasLast(false), m_trustNext(false),
      m_lastX(0.f), m_lastY(0.f), m_lastZ(0.f), m_lastO(0.f),
      m_lastMS(0), m_lastFlags(0),
      m_hasValid(false), m_validX(0.f), m_validY(0.f), m_validZ(0.f), m_validO(0.f),
      m_hasTrace(false), m_traceX(0.f), m_traceY(0.f), m_traceZ(0.f),
      m_airborne(false), m_fallApexZ(0.f),
      m_burstWinStartMS(0), m_burstCount(0), m_lastClientTime(0), m_hasClientTime(false)
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

void MovementAnticheat::HandlePositionUpdate(uint16 opcode, MovementInfo const& mi)
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
        m_airborne = (state == AC_MOVE_FALL);
        m_fallApexZ = pos->z;
        m_hasClientTime = false;
        m_burstWinStartMS = nowMS;
        m_burstCount = 0;
        return;
    }

    // --- Detector: movement-packet burst (flood / timing manipulation) ---
    if (getMSTimeDiff(m_burstWinStartMS, nowMS) >= 1000)
    {
        m_burstWinStartMS = nowMS;
        m_burstCount = 0;
    }
    ++m_burstCount;
    if (m_burstCount == BURST_PER_SEC + 1) // fire once when first exceeding the cap
    {
        AntiCheatContext bctx;
        bctx.mapId = m_player->GetMapId();
        bctx.x = pos->x; bctx.y = pos->y; bctx.z = pos->z;
        bctx.latency = m_player->GetSession() ? m_player->GetSession()->GetLatencyEWMA() : 0;
        bctx.detail = "movement packet burst";
        sAntiCheatMgr->RecordViolation(m_player, AC_VIOLATION_BURST, 15.0f, bctx);
    }

    // --- Detector: client movement-timestamp regression --- (also capture the
    // client-reported time delta for the time-sync divergence check below)
    uint32 clientDt = 0;
    bool   haveClientDt = false;
    {
        uint32 ct = mi.GetTime();
        if (m_hasClientTime)
        {
            haveClientDt = true;
            clientDt = (ct >= m_lastClientTime) ? (ct - m_lastClientTime) : 0;
            if (m_lastClientTime > ct && (m_lastClientTime - ct) > CLIENT_TIME_BACK_MS)
            {
                AntiCheatContext tctx;
                tctx.mapId = m_player->GetMapId();
                tctx.x = pos->x; tctx.y = pos->y; tctx.z = pos->z;
                tctx.latency = m_player->GetSession() ? m_player->GetSession()->GetLatencyEWMA() : 0;
                tctx.detail = "client timestamp regression";
                sAntiCheatMgr->RecordViolation(m_player, AC_VIOLATION_PACKETTIMING, 10.0f, tctx);
            }
        }
        m_lastClientTime = ct;
        m_hasClientTime = true;
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

    // --- Detector: time-sync divergence (client vs server elapsed time) ---
    // The client's reported elapsed time should track the server's measured
    // elapsed time within latency jitter. Zero client-time while moving, or a
    // large divergence, is time-manipulation desync (fake-slow movement / speed
    // via clock control) — the vanilla-compatible equivalent of WotLK time sync.
    if (haveClientDt && horiz > 1.0f && state != AC_MOVE_TRANSPORT)
    {
        uint32 tol = sWorld.getConfig(CONFIG_UINT32_TIMESYNC_DESYNC) + latency;
        if (clientDt == 0)
        {
            ctx.detail = "zero client time while moving (time hack)";
            sAntiCheatMgr->RecordViolation(m_player, AC_VIOLATION_DESYNC, 20.0f, ctx);
            cheapTrip = true;
        }
        else
        {
            uint32 div = clientDt > dtMS ? clientDt - dtMS : dtMS - clientDt;
            if (div > tol)
            {
                ctx.detail = "client/server time divergence (desync)";
                sAntiCheatMgr->RecordViolation(m_player, AC_VIOLATION_DESYNC, 8.0f, ctx);
            }
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

    // --- Detectors: illegal jump (infinite/double jump) + fall-damage suppression ---
    if (opcode == MSG_MOVE_JUMP)
    {
        // A jump issued while already airborne (no FALL_LAND since the last jump
        // or fall) is an illegal mid-air / infinite jump.
        if (m_airborne)
        {
            ctx.detail = "mid-air / infinite jump";
            sAntiCheatMgr->RecordViolation(m_player, AC_VIOLATION_JUMP, 30.0f, ctx);
            cheapTrip = true;
        }
        m_airborne = true;
        m_fallApexZ = pos->z;
    }
    else if (state == AC_MOVE_FALL)
    {
        // In a fall/jump arc: track the episode and its apex.
        m_airborne = true;
        if (pos->z > m_fallApexZ)
            m_fallApexZ = pos->z;
    }
    else if (m_airborne)
    {
        // Episode ended this packet. A legit landing sends MSG_MOVE_FALL_LAND and
        // the core applies fall damage. Becoming grounded WITHOUT a FALL_LAND after
        // a damaging drop (and not into water) means the client suppressed fall damage.
        float drop = m_fallApexZ - pos->z;
        if (opcode != MSG_MOVE_FALL_LAND && state != AC_MOVE_SWIM && drop >= FALL_SUPPRESS_YD)
        {
            ctx.detail = "fall-damage suppressed (no FALL_LAND)";
            sAntiCheatMgr->RecordViolation(m_player, AC_VIOLATION_FALL, 25.0f, ctx);
            cheapTrip = true;
        }
        m_airborne = false;
        m_fallApexZ = pos->z;
    }
    else
    {
        // Grounded: keep the apex tracking current so the next fall measures from here.
        m_fallApexZ = pos->z;
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

    // Debug visualizer: drop a movement-trace marker, rate-limited by distance.
    if (DebugVisualizer::TraceEnabled())
    {
        float tdx = pos->x - m_traceX, tdy = pos->y - m_traceY, tdz = pos->z - m_traceZ;
        float minD = float(sWorld.getConfig(CONFIG_UINT32_ACDBG_TRACE_MINDIST));
        if (!m_hasTrace || (tdx * tdx + tdy * tdy + tdz * tdz) >= minD * minD)
        {
            DebugVisualizer::Trace(m_player, state, pos->x, pos->y, pos->z);
            m_hasTrace = true;
            m_traceX = pos->x; m_traceY = pos->y; m_traceZ = pos->z;
        }
    }
}

void MovementAnticheat::PeriodicCheck()
{
    if (!m_player || !m_player->IsInWorld())
        return;
    if (!sAntiCheatMgr->PhysicsEnabled() || sAntiCheatMgr->IsExempt(m_player))
        return;

    // Re-validate the player's current (idle) position against terrain — catches
    // static exploits with no movement packets. Only the grounded case is judged.
    AntiCheatMoveState state = NormalizeState(m_player->m_movementInfo);
    if (state != AC_MOVE_GROUND)
        return;

    const char* reason = NULL;
    if (PhysicsValidator::Validate(m_player, state, m_player->m_movementInfo, &reason) == AC_PHYS_IMPOSSIBLE)
    {
        AntiCheatContext ctx;
        ctx.mapId = m_player->GetMapId();
        ctx.x = m_player->GetPositionX(); ctx.y = m_player->GetPositionY(); ctx.z = m_player->GetPositionZ();
        ctx.latency = m_player->GetSession() ? m_player->GetSession()->GetLatencyEWMA() : 0;
        ctx.detail = reason ? reason : "physics impossible (idle)";
        sAntiCheatMgr->RecordViolation(m_player, AC_VIOLATION_PHYSICS, 20.0f, ctx);
    }
}
