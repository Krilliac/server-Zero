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
#include "Log.h"

#include <cmath>

namespace
{
    const float  VERT_CLIMB_SUSPECT = 5.0f;   // yd upward in one packet on ground
    const float  SPEED_SLACK_YD     = 2.0f;   // constant distance fudge per packet
    const uint32 GAP_RESET_MS       = 3000;   // packet gap implying load/teleport
    const float  FALL_SUPPRESS_YD   = 20.0f;  // drop beyond this should incur fall damage
    const uint32 BURST_PER_SEC      = 50;     // movement packets/sec beyond this = burst
    const uint32 CLIENT_TIME_BACK_MS = 500;   // client timestamp regression tolerance
    const uint32 SKIP_WINDOW_MS      = 10000;  // move-time-skip abuse counting window
    const uint32 SKIP_MAX_PER_WINDOW = 10;     // legit clients rarely skip this often
}

MovementAnticheat::MovementAnticheat(Player* owner)
    : m_player(owner), m_hasLast(false), m_trustNext(false),
      m_lastX(0.f), m_lastY(0.f), m_lastZ(0.f), m_lastO(0.f),
      m_lastMS(0), m_lastFlags(0),
      m_hasValid(false), m_validX(0.f), m_validY(0.f), m_validZ(0.f), m_validO(0.f),
      m_hasTrace(false), m_traceX(0.f), m_traceY(0.f), m_traceZ(0.f),
      m_airborne(false), m_fallApexZ(0.f),
      m_burstWinStartMS(0), m_burstCount(0), m_lastClientTime(0), m_hasClientTime(false),
      m_hasClockOffset(false), m_clockOffsetMs(0),
      m_timeSkipGraceUntilMS(0), m_desyncStreak(0), m_lastResyncMS(0),
      m_skipWinStartMS(0), m_skipCount(0), m_skipAccumMs(0)
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

        // Movement-sync clock-offset service: smoothed (serverMs - clientTime).
        // Absolute value is arbitrary (different epochs); its drift over time is
        // the desync signal, and it backs the optional relay correction.
        int64 sampleOffset = int64(nowMS) - int64(ct);
        if (!m_hasClockOffset) { m_clockOffsetMs = sampleOffset; m_hasClockOffset = true; }
        else { m_clockOffsetMs = (sampleOffset * 20 + m_clockOffsetMs * 80) / 100; }
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
    // The grace window after a client-reported MOVE_TIME_SKIPPED suppresses this
    // (the client already told us its clock jumped — not a cheat).
    if (haveClientDt && horiz > 1.0f && state != AC_MOVE_TRANSPORT &&
        nowMS >= m_timeSkipGraceUntilMS)
    {
        uint32 tol = sWorld.getConfig(CONFIG_UINT32_TIMESYNC_DESYNC) + latency;
        if (clientDt == 0)
        {
            ctx.detail = "zero client time while moving (time hack)";
            sAntiCheatMgr->RecordViolation(m_player, AC_VIOLATION_DESYNC, 20.0f, ctx);
            cheapTrip = true;
            ++m_desyncStreak;
        }
        else
        {
            uint32 div = clientDt > dtMS ? clientDt - dtMS : dtMS - clientDt;
            if (div > tol)
            {
                ctx.detail = "client/server time divergence (desync)";
                sAntiCheatMgr->RecordViolation(m_player, AC_VIOLATION_DESYNC, 8.0f, ctx);
                ++m_desyncStreak;          // feeds optional auto-resync
            }
            else if (m_desyncStreak > 0)
            {
                --m_desyncStreak;          // decay on clean, in-sync packets
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
    if (sAntiCheatMgr->IsExempt(m_player))
        return;

    // --- Desync auto-resync (gated, OFF by default) ---
    // When the per-packet desync detector has tripped repeatedly, the client clock
    // has drifted out of sync. There is no vanilla TIME_SYNC opcode to correct it,
    // so the only reliable lever is to rubberband the client to its current
    // server-authoritative position; NotifyServerRelocation re-baselines so the
    // correction itself isn't re-scored. Cooldown-limited to avoid yo-yoing.
    if (sWorld.getConfig(CONFIG_BOOL_TIMESYNC_AUTORESYNC) &&
        m_desyncStreak >= sWorld.getConfig(CONFIG_UINT32_TIMESYNC_RESYNC_TRIPS))
    {
        uint32 now = getMSTime();
        if (now - m_lastResyncMS >= sWorld.getConfig(CONFIG_UINT32_TIMESYNC_RESYNC_COOLDOWN))
        {
            m_player->NearTeleportTo(m_player->GetPositionX(), m_player->GetPositionY(),
                                     m_player->GetPositionZ(), m_player->GetOrientation());
            NotifyServerRelocation();
            m_lastResyncMS = now;
            m_desyncStreak = 0;
            sLog.outDetail("TimeSync: resync guid=%u (sustained desync)", m_player->GetGUIDLow());
        }
    }

    // Idle terrain re-validation needs the physics module enabled.
    if (!sAntiCheatMgr->PhysicsEnabled())
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

void MovementAnticheat::NotifyClientTimeSkip(uint32 skippedMs)
{
    if (!m_player)
        return;

    uint32 now = getMSTime();

    // --- Anti-cheat: CMSG_MOVE_TIME_SKIPPED abuse (time-based movement masking) ---
    // Cheats inflate or spam the reported skip to claim extra movement budget
    // (covering speed/teleport distance "in skipped time"). Score by magnitude,
    // frequency and accumulation within a rolling window.
    if (m_skipWinStartMS == 0 || now - m_skipWinStartMS > SKIP_WINDOW_MS)
    {
        m_skipWinStartMS = now;
        m_skipCount = 0;
        m_skipAccumMs = 0;
    }
    ++m_skipCount;
    m_skipAccumMs += skippedMs;

    if (sAntiCheatMgr->MovementEnabled() && !sAntiCheatMgr->IsExempt(m_player))
    {
        uint32 maxSkip = sWorld.getConfig(CONFIG_UINT32_TIMESYNC_MAX_SKIP);
        AntiCheatContext ctx;
        ctx.mapId = m_player->GetMapId();
        ctx.x = m_player->GetPositionX(); ctx.y = m_player->GetPositionY(); ctx.z = m_player->GetPositionZ();
        ctx.latency = m_player->GetSession() ? m_player->GetSession()->GetLatencyEWMA() : 0;

        if (skippedMs > maxSkip)
        {
            float ratio = float(skippedMs) / float(maxSkip ? maxSkip : 1);
            float weight = ratio * 8.0f;
            if (weight > 30.0f) weight = 30.0f;
            ctx.detail = "oversized move-time-skip (time hack)";
            sAntiCheatMgr->RecordViolation(m_player, AC_VIOLATION_DESYNC, weight, ctx);
        }
        if (m_skipCount > SKIP_MAX_PER_WINDOW)
        {
            ctx.detail = "move-time-skip spam";
            sAntiCheatMgr->RecordViolation(m_player, AC_VIOLATION_PACKETTIMING, 12.0f, ctx);
        }
        if (m_skipAccumMs > maxSkip * 3)
        {
            ctx.detail = "excessive accumulated time-skip";
            sAntiCheatMgr->RecordViolation(m_player, AC_VIOLATION_DESYNC, 10.0f, ctx);
        }
    }

    // --- Legit handling: a real skip means the client clock jumped, so re-baseline
    // the clock service and grace the per-packet desync detector so the same event
    // isn't double-counted as divergence. ---
    uint32 grace = skippedMs + 1000;
    if (grace > 5000) grace = 5000;
    m_timeSkipGraceUntilMS = now + grace;
    m_hasClockOffset = false;   // re-seed offset from the new client timebase
    m_hasClientTime  = false;   // re-seed the client-timestamp baseline
    m_trustNext      = true;    // the skip itself moves nothing; trust the next packet
}
