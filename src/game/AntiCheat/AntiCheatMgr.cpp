/*
 * Anti-Cheat / Movement-Validation framework — central manager implementation.
 * Slice 1 (core detection pipeline).
 */

#include "AntiCheatMgr.h"
#include "DebugVisualizer.h"
#include "Player.h"
#include "World.h"
#include "Log.h"
#include "Timer.h"
#include "Database/DatabaseEnv.h"

#include <cstdio>

AntiCheatMgr::AntiCheatMgr()
    : m_enabled(false), m_movementEnabled(false), m_physicsEnabled(false),
      m_exemptBots(true), m_persist(true), m_exemptGmLevel(1),
      m_actionCeiling(AC_ACTION_LOG), m_speedTolerancePct(110),
      m_teleportDistance(50), m_scoreWarn(30), m_scoreRubberband(60),
      m_scoreKick(120), m_decayPerSec(2)
{
}

void AntiCheatMgr::Init()
{
    LoadConfig();
    if (m_enabled)
        sLog.outString("AntiCheat: enabled (action ceiling=%u, movement=%u, physics=%u)",
                       m_actionCeiling, MovementEnabled() ? 1 : 0, PhysicsEnabled() ? 1 : 0);
    else
        sLog.outString("AntiCheat: disabled (AntiCheat.Enable = 0)");
}

void AntiCheatMgr::LoadConfig()
{
    m_enabled         = sWorld.getConfig(CONFIG_BOOL_ANTICHEAT_ENABLE);
    m_movementEnabled = sWorld.getConfig(CONFIG_BOOL_ANTICHEAT_MOVEMENT);
    m_physicsEnabled  = sWorld.getConfig(CONFIG_BOOL_ANTICHEAT_PHYSICS);
    m_exemptBots      = sWorld.getConfig(CONFIG_BOOL_ANTICHEAT_EXEMPT_BOTS);
    m_persist         = sWorld.getConfig(CONFIG_BOOL_ANTICHEAT_PERSIST);
    m_exemptGmLevel   = sWorld.getConfig(CONFIG_UINT32_ANTICHEAT_EXEMPT_GM);
    m_actionCeiling   = sWorld.getConfig(CONFIG_UINT32_ANTICHEAT_ACTION);
    m_speedTolerancePct = sWorld.getConfig(CONFIG_UINT32_ANTICHEAT_SPEED_TOL);
    m_teleportDistance  = sWorld.getConfig(CONFIG_UINT32_ANTICHEAT_TELE_DIST);
    m_scoreWarn       = sWorld.getConfig(CONFIG_UINT32_ANTICHEAT_SCORE_WARN);
    m_scoreRubberband = sWorld.getConfig(CONFIG_UINT32_ANTICHEAT_SCORE_RUBBER);
    m_scoreKick       = sWorld.getConfig(CONFIG_UINT32_ANTICHEAT_SCORE_KICK);
    m_decayPerSec     = sWorld.getConfig(CONFIG_UINT32_ANTICHEAT_DECAY);
}

bool AntiCheatMgr::IsExempt(Player* player) const
{
    if (!player)
        return true;

    // GMs at or above the configured security level are not validated.
    if (player->GetSession() && player->GetSession()->GetSecurity() >= (AccountTypes)m_exemptGmLevel)
        return true;

    // A GM with .gm on is also exempt regardless of level.
    if (player->isGameMaster())
        return true;

#ifdef ENABLE_PLAYERBOTS
    if (m_exemptBots && player->GetPlayerbotAI())
        return true;
#endif

    return false;
}

float AntiCheatMgr::DecayedScore(ScoreState& s, uint32 nowMS) const
{
    if (s.lastUpdateMS && m_decayPerSec)
    {
        uint32 elapsedMS = getMSTimeDiff(s.lastUpdateMS, nowMS);
        float decay = (float(elapsedMS) / 1000.0f) * float(m_decayPerSec);
        s.score = s.score > decay ? s.score - decay : 0.0f;
    }
    s.lastUpdateMS = nowMS;
    return s.score;
}

void AntiCheatMgr::RecordViolation(Player* player, AntiCheatViolationType type,
                                   float weight, AntiCheatContext const& ctx)
{
    if (!m_enabled || !player || IsExempt(player))
        return;

    // Clamp weight defensively so a single buggy detector can't spike the score.
    if (weight < 0.0f) weight = 0.0f;
    if (weight > 100.0f) weight = 100.0f;

    uint32 nowMS = getMSTime();
    uint32 lowGuid = player->GetGUIDLow();
    float score;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        ScoreState& s = m_scores[lowGuid];
        DecayedScore(s, nowMS);
        s.score += weight;
        ++s.violations;
        score = s.score;
    }

    if (m_persist)
        Persist(player, type, score, ctx);

    // Debug visualizer: drop a colour-coded marker for this violation (no-op
    // unless the visualizer is enabled in config).
    DebugVisualizer::Mark(player, type, ctx.x, ctx.y, ctx.z);

    Apply(player, score, type, ctx);
}

void AntiCheatMgr::Apply(Player* player, float score, AntiCheatViolationType type,
                         AntiCheatContext const& ctx)
{
    // Highest warranted action, capped by the configured ceiling. This is the
    // ONLY place a countermeasure is applied; detectors never punish directly.
    if (m_actionCeiling >= AC_ACTION_KICK && score >= float(m_scoreKick))
    {
        sLog.outError("AntiCheat: KICK guid=%u type=%u score=%.0f map=%u pos=(%.1f,%.1f,%.1f) %s",
                      player->GetGUIDLow(), type, score, ctx.mapId, ctx.x, ctx.y, ctx.z, ctx.detail);
        AlertGMs(player, type, score, ctx);
        if (player->GetSession())
            player->GetSession()->KickPlayer();
        return;
    }

    if (m_actionCeiling >= AC_ACTION_RUBBERBAND && score >= float(m_scoreRubberband))
    {
        // Rubberband target (last validated position) is provided by the
        // per-player movement validator, wired in the movement-hook slice.
        // Until then we escalate to a GM alert rather than teleport blindly.
        AlertGMs(player, type, score, ctx);
        return;
    }

    if (m_actionCeiling >= AC_ACTION_GM_ALERT && score >= float(m_scoreWarn))
    {
        AlertGMs(player, type, score, ctx);
        return;
    }

    if (m_actionCeiling >= AC_ACTION_LOG)
    {
        sLog.outDetail("AntiCheat: log guid=%u type=%u score=%.0f map=%u pos=(%.1f,%.1f,%.1f) speed=%.1f lat=%u %s",
                       player->GetGUIDLow(), type, score, ctx.mapId, ctx.x, ctx.y, ctx.z,
                       ctx.speed, ctx.latency, ctx.detail);
    }
}

void AntiCheatMgr::AlertGMs(Player* player, AntiCheatViolationType type, float score,
                            AntiCheatContext const& ctx)
{
    // Slice 1: log to the server console/log. Broadcasting to online GMs in-game
    // is a planned enhancement (needs the GM-notify helper) — kept out for now
    // so this slice stays low-risk.
    sLog.outBasic("AntiCheat: ALERT guid=%u type=%u score=%.0f map=%u pos=(%.1f,%.1f,%.1f) speed=%.1f lat=%u %s",
                  player->GetGUIDLow(), type, score, ctx.mapId, ctx.x, ctx.y, ctx.z,
                  ctx.speed, ctx.latency, ctx.detail);
}

void AntiCheatMgr::Persist(Player* player, AntiCheatViolationType type, float score,
                           AntiCheatContext const& ctx)
{
    uint32 account = player->GetSession() ? player->GetSession()->GetAccountId() : 0;
    // detail is always a static string literal from the detectors — safe to embed.
    CharacterDatabase.PExecute(
        "INSERT INTO `character_anticheat_violation` "
        "(`guid`,`account`,`type`,`score`,`map`,`x`,`y`,`z`,`speed`,`latency`,`detail`) "
        "VALUES (%u,%u,%u,%u,%u,%f,%f,%f,%f,%u,'%s')",
        player->GetGUIDLow(), account, uint32(type), uint32(score), ctx.mapId,
        ctx.x, ctx.y, ctx.z, ctx.speed, ctx.latency, ctx.detail ? ctx.detail : "");
}

void AntiCheatMgr::Update(uint32 /*diff*/)
{
    if (!m_enabled)
        return;

    // Prune fully-decayed idle entries so the score map doesn't grow unbounded
    // for players who never offend again.
    uint32 nowMS = getMSTime();
    std::lock_guard<std::mutex> guard(m_lock);
    for (std::map<uint32, ScoreState>::iterator it = m_scores.begin(); it != m_scores.end();)
    {
        if (DecayedScore(it->second, nowMS) <= 0.0f)
            m_scores.erase(it++);
        else
            ++it;
    }
}

void AntiCheatMgr::RemovePlayer(uint32 lowGuid)
{
    std::lock_guard<std::mutex> guard(m_lock);
    m_scores.erase(lowGuid);
}

void AntiCheatMgr::BuildStatus(Player* target, std::string& out)
{
    if (!target)
    {
        out = "AntiCheat: no target.";
        return;
    }

    uint32 nowMS = getMSTime();
    float score = 0.0f;
    uint32 violations = 0;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        std::map<uint32, ScoreState>::iterator it = m_scores.find(target->GetGUIDLow());
        if (it != m_scores.end())
        {
            score = DecayedScore(it->second, nowMS);
            violations = it->second.violations;
        }
    }

    char buf[256];
    snprintf(buf, sizeof(buf),
             "AntiCheat status for %s: score=%.0f, lifetime violations=%u, %s",
             target->GetName(), score, violations,
             m_enabled ? "framework ENABLED" : "framework disabled");
    out = buf;
}
