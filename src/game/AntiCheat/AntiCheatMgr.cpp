/*
 * Anti-Cheat / Movement-Validation framework — central manager implementation.
 * Slice 1 (core detection pipeline).
 */

#include "AntiCheatMgr.h"
#include "DebugVisualizer.h"
#include "MovementAnticheat.h"
#include "Player.h"
#include "World.h"
#include "ObjectMgr.h"
#include "Log.h"
#include "Timer.h"
#include "Database/DatabaseEnv.h"

#include <cstdio>
#include <algorithm>

AntiCheatMgr::AntiCheatMgr()
    : m_enabled(false), m_testBypass(false), m_movementEnabled(false), m_physicsEnabled(false),
      m_exemptBots(true), m_persist(true), m_exemptGmLevel(1),
      m_actionCeiling(AC_ACTION_LOG), m_speedTolerancePct(110),
      m_teleportDistance(50), m_scoreWarn(30), m_scoreRubberband(60),
      m_scoreKick(120), m_decayPerSec(2),
      m_autobanEnable(false), m_autobanKickPoints(10), m_autobanThreshold(30),
      m_autobanDecayPerHour(1),
      m_migrationValidate(false), m_migrationSpeedTolPct(400), m_migrationMaxElapsedSec(30),
      m_autobanGmExempt(true), m_evasionFlagEnable(false)
{
    m_autobanDur[0] = 86400; m_autobanDur[1] = 604800; m_autobanDur[2] = 0;
    for (uint32 i = 0; i < AC_VIOLATION_MAX; ++i)
        m_autobanWeightMul[i] = 100;
}

void AntiCheatMgr::Init()
{
    LoadConfig();
    if (m_autobanEnable)
        LoadAccounts();
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

    m_autobanEnable      = sWorld.getConfig(CONFIG_BOOL_AC_AUTOBAN_ENABLE);
    m_autobanKickPoints  = sWorld.getConfig(CONFIG_UINT32_AC_AUTOBAN_KICKPOINTS);
    m_autobanThreshold   = sWorld.getConfig(CONFIG_UINT32_AC_AUTOBAN_THRESHOLD);
    m_autobanDecayPerHour = sWorld.getConfig(CONFIG_UINT32_AC_AUTOBAN_DECAY_PER_HOUR);
    m_autobanDur[0]      = sWorld.getConfig(CONFIG_UINT32_AC_AUTOBAN_DUR1);
    m_autobanDur[1]      = sWorld.getConfig(CONFIG_UINT32_AC_AUTOBAN_DUR2);
    m_autobanDur[2]      = sWorld.getConfig(CONFIG_UINT32_AC_AUTOBAN_DUR3);

    // Phase 4 cluster migration-seam validation.
    m_migrationValidate     = sWorld.getConfig(CONFIG_BOOL_ANTICHEAT_MIGRATION_VALIDATE);
    m_migrationSpeedTolPct  = sWorld.getConfig(CONFIG_UINT32_ANTICHEAT_MIGRATION_SPEED_TOL);
    m_migrationMaxElapsedSec = sWorld.getConfig(CONFIG_UINT32_ANTICHEAT_MIGRATION_MAX_ELAPSED);

    // Phase 6 autoban tuning. Reset all per-type multipliers to 100% then overwrite
    // the curated high-signal indices from config.
    m_autobanGmExempt    = sWorld.getConfig(CONFIG_BOOL_AC_AUTOBAN_GM_EXEMPT);
    m_evasionFlagEnable  = sWorld.getConfig(CONFIG_BOOL_AC_AUTOBAN_EVASION_FLAG);
    for (uint32 i = 0; i < AC_VIOLATION_MAX; ++i)
        m_autobanWeightMul[i] = 100;
    m_autobanWeightMul[AC_VIOLATION_TELEPORT] = sWorld.getConfig(CONFIG_UINT32_AC_AUTOBAN_WEIGHT_TELEPORT);
    m_autobanWeightMul[AC_VIOLATION_FLAG_CONTRADICT] = sWorld.getConfig(CONFIG_UINT32_AC_AUTOBAN_WEIGHT_FLY);
    m_autobanWeightMul[AC_VIOLATION_SPEED]    = sWorld.getConfig(CONFIG_UINT32_AC_AUTOBAN_WEIGHT_SPEED);
    m_autobanWeightMul[AC_VIOLATION_PROTOCOL] = sWorld.getConfig(CONFIG_UINT32_AC_AUTOBAN_WEIGHT_PROTOCOL);
    m_autobanWeightMul[AC_VIOLATION_GW_SPEED] = sWorld.getConfig(CONFIG_UINT32_AC_AUTOBAN_WEIGHT_SPEED);
}

bool AntiCheatMgr::IsExempt(Player* player) const
{
    if (!player)
        return true;

    // GMs at or above the configured security level are not validated.
    // ExemptGMLevel == 0 means "exempt nobody by level" (every account is >= 0,
    // so without this guard a value of 0 would exempt everyone).
    if (m_exemptGmLevel > 0 && player->GetSession() &&
        player->GetSession()->GetSecurity() >= (AccountTypes)m_exemptGmLevel)
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
    if (!player)
        return;
    // m_testBypass lets `.cheat` simulations score on an exempt GM / with AC off.
    if (!m_testBypass && (!m_enabled || IsExempt(player)))
        return;
    DoRecord(player, type, weight, ctx);
}

void AntiCheatMgr::RecordGatewayViolation(Player* player, AntiCheatViolationType type,
                                          float weight, const char* detail)
{
    // Pre-in-world gateway events have no Player to score against (Phase 1 does
    // not persist account-only offences); log and drop rather than crash.
    if (!player)
    {
        sLog.outError("AntiCheat: RecordGatewayViolation(type %u, weight %.0f, '%s') with no player; "
                      "pre-in-world gateway events are not scored in this phase.",
                      uint32(type), weight, detail ? detail : "");
        return;
    }

    // Build a context from the player's current game state. The gateway is the
    // neutral edge clock, so we carry no node-side speed/latency here (0).
    AntiCheatContext ctx;
    ctx.mapId   = player->GetMapId();
    ctx.x       = player->GetPositionX();
    ctx.y       = player->GetPositionY();
    ctx.z       = player->GetPositionZ();
    ctx.speed   = 0.f;
    ctx.latency = 0;
    ctx.detail  = detail ? detail : "gateway";

    // Reuse the full gate: enabled/exempt check, scoring, persist, escalation,
    // autoban. DoRecord clamps weight to [0,100]; clamp here too for clarity.
    if (weight < 0.0f) weight = 0.0f;
    if (weight > 100.0f) weight = 100.0f;

    RecordViolation(player, type, weight, ctx);
}

void AntiCheatMgr::TestInject(Player* player, AntiCheatViolationType type,
                              float weight, AntiCheatContext const& ctx)
{
    // Deliberately bypasses the enabled/exempt gate so `.anticheat test` can drive
    // the whole pipeline (scoring, decay, persist, marker, escalation) on a GM.
    if (!player)
        return;
    DoRecord(player, type, weight, ctx);
}

void AntiCheatMgr::DoRecord(Player* player, AntiCheatViolationType type,
                            float weight, AntiCheatContext const& ctx)
{
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

void AntiCheatMgr::GetTopScores(uint32 limit, std::vector<std::pair<uint32, float> >& out)
{
    uint32 nowMS = getMSTime();
    std::vector<std::pair<uint32, float> > all;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        for (std::map<uint32, ScoreState>::iterator it = m_scores.begin(); it != m_scores.end(); ++it)
        {
            float s = DecayedScore(it->second, nowMS);
            if (s > 0.0f)
                all.push_back(std::make_pair(it->first, s));
        }
    }
    std::sort(all.begin(), all.end(),
              [](std::pair<uint32, float> const& a, std::pair<uint32, float> const& b)
              { return a.second > b.second; });
    if (all.size() > limit)
        all.resize(limit);
    out.swap(all);
}

void AntiCheatMgr::SetScore(Player* player, float score)
{
    if (!player)
        return;
    if (score < 0.0f) score = 0.0f;

    uint32 nowMS = getMSTime();
    {
        std::lock_guard<std::mutex> guard(m_lock);
        ScoreState& s = m_scores[player->GetGUIDLow()];
        DecayedScore(s, nowMS);   // settle decay before overwriting
        s.score = score;
    }

    AntiCheatContext ctx;
    ctx.mapId = player->GetMapId();
    ctx.x = player->GetPositionX(); ctx.y = player->GetPositionY(); ctx.z = player->GetPositionZ();
    ctx.detail = "GM set score";
    Apply(player, score, AC_VIOLATION_NONE, ctx);
}

void AntiCheatMgr::BuildDiag(std::string& out)
{
    char buf[768];
    snprintf(buf, sizeof(buf),
             "AntiCheat config: enabled=%u movement=%u physics=%u accelCheck=%u | "
             "actionCeiling=%u warn=%u rubber=%u kick=%u decay/s=%u | "
             "speedTol=%u%% teleDist=%u | autoban=%u (kickPts=%u thr=%u gmExempt=%u "
             "evasionFlag=%u wTele=%u%% wFly=%u%% wSpeed=%u%% wProto=%u%%) | "
             "migration: validate=%u tol=%u%% maxElapsed=%us | "
             "noclipStrict=%u knockback=%u | persist=%u exemptGmLvl=%u exemptBots=%u",
             (uint32)m_enabled, (uint32)m_movementEnabled, (uint32)m_physicsEnabled,
             (uint32)sWorld.getConfig(CONFIG_BOOL_ANTICHEAT_ACCEL_CHECK),
             m_actionCeiling, m_scoreWarn, m_scoreRubberband, m_scoreKick, m_decayPerSec,
             m_speedTolerancePct, m_teleportDistance,
             (uint32)m_autobanEnable, m_autobanKickPoints, m_autobanThreshold,
             (uint32)m_autobanGmExempt, (uint32)m_evasionFlagEnable,
             m_autobanWeightMul[AC_VIOLATION_TELEPORT], m_autobanWeightMul[AC_VIOLATION_FLAG_CONTRADICT],
             m_autobanWeightMul[AC_VIOLATION_SPEED], m_autobanWeightMul[AC_VIOLATION_PROTOCOL],
             (uint32)m_migrationValidate, m_migrationSpeedTolPct, m_migrationMaxElapsedSec,
             (uint32)sWorld.getConfig(CONFIG_BOOL_ANTICHEAT_NOCLIP_STRICT),
             (uint32)sWorld.getConfig(CONFIG_BOOL_ANTICHEAT_KNOCKBACK_CHECK),
             (uint32)m_persist, m_exemptGmLevel, (uint32)m_exemptBots);
    out = buf;
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
        // Anti-gaming autoban: count this kick against the account first.
        if (m_autobanEnable)
            AccumulateKick(player, type);
        if (player->GetSession())
            player->GetSession()->KickPlayer();
        return;
    }

    if (m_actionCeiling >= AC_ACTION_RUBBERBAND && score >= float(m_scoreRubberband))
    {
        // Rubberband to the last server-validated position tracked by the
        // per-player movement validator.
        MovementAnticheat* mac = player->GetMovementAnticheat();
        if (mac && mac->HasValid())
        {
            player->NearTeleportTo(mac->ValidX(), mac->ValidY(), mac->ValidZ(), mac->ValidO());
            sLog.outDetail("AntiCheat: rubberband guid=%u score=%.0f -> (%.1f,%.1f,%.1f)",
                           player->GetGUIDLow(), score, mac->ValidX(), mac->ValidY(), mac->ValidZ());
        }
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

    // Apply any queued autobans here (world thread): AccumulateKick runs on the
    // map thread, but BanAccount touches the session list, so it is deferred.
    std::vector<PendingBan> bans;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        if (!m_pendingBans.empty())
            bans.swap(m_pendingBans);
    }
    for (std::vector<PendingBan>::const_iterator it = bans.begin(); it != bans.end(); ++it)
    {
        BanReturn r = sWorld.BanAccount(BAN_CHARACTER, it->charName, it->durationSecs, it->reason, "AntiCheat");
        sLog.outError("AntiCheat: AUTOBAN account of '%s' for %us (%s) -> result %u",
                      it->charName.c_str(), it->durationSecs, it->reason.c_str(), uint32(r));

        // Phase 6 ban-evasion flagging (report-only): if enabled, find non-GM
        // accounts sharing the banned account's last_ip and LOG them for GM review.
        // Never auto-bans alts — shared IPs (NAT/household/café) cause false matches.
        if (m_evasionFlagEnable && r == BAN_SUCCESS)
        {
            uint32 bannedAcc = sObjectMgr.GetPlayerAccountIdByPlayerName(it->charName);
            if (bannedAcc)
            {
                std::vector<AntiCheatAlt> alts;
                CorrelateByIp(bannedAcc, alts);
                for (std::vector<AntiCheatAlt>::const_iterator a = alts.begin(); a != alts.end(); ++a)
                    sLog.outBasic("AntiCheat: ban-evasion WATCH — account %u (%s) shares IP %s with "
                                  "just-banned %u; kick_score=%.0f banned=%u",
                                  a->accountId, a->username.c_str(), a->lastIp.c_str(),
                                  bannedAcc, a->kickScore, uint32(a->banned ? 1 : 0));
            }
        }
    }

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

float AntiCheatMgr::DecayedKickScore(AccountState& s, uint32 nowSec) const
{
    if (s.lastUpdate && m_autobanDecayPerHour && nowSec > s.lastUpdate)
    {
        float hours = float(nowSec - s.lastUpdate) / 3600.0f;
        float decay = hours * float(m_autobanDecayPerHour);
        s.kickScore = s.kickScore > decay ? s.kickScore - decay : 0.0f;
    }
    s.lastUpdate = nowSec;
    return s.kickScore;
}

bool AntiCheatMgr::ReadAccountRow(uint32 accountId, AccountState& out)
{
    out = AccountState();
    QueryResult* result = LoginDatabase.PQuery(
        "SELECT `kick_score`,`ban_count`,`last_update` FROM `account_anticheat` WHERE `account`=%u",
        accountId);
    if (!result)
        return false;
    Field* f = result->Fetch();
    out.kickScore  = f[0].GetFloat();
    out.banCount   = f[1].GetUInt32();
    out.lastUpdate = f[2].GetUInt32();
    delete result;
    return true;
}

float AntiCheatMgr::PerTypeKickWeight(AntiCheatViolationType type) const
{
    uint32 mul = 100;
    if (type > AC_VIOLATION_NONE && type < AC_VIOLATION_MAX)
        mul = m_autobanWeightMul[type];
    return float(m_autobanKickPoints) * float(mul) / 100.0f;
}

void AntiCheatMgr::AccumulateKick(Player* player, AntiCheatViolationType type)
{
    if (!player || !player->GetSession())
        return;

    // Phase 6 belt-and-suspenders: kicks already pass the IsExempt gate before
    // reaching here, but skip GM accounts explicitly so a GM can never accrue an
    // account-level autoban score even if a future caller bypasses Apply()'s gate.
    if (m_autobanGmExempt && m_exemptGmLevel > 0 &&
        player->GetSession()->GetSecurity() >= (AccountTypes)m_exemptGmLevel)
        return;

    uint32 accountId = player->GetSession()->GetAccountId();
    uint32 nowSec    = uint32(sWorld.GetGameTime());
    std::string charName = player->GetName();

    // Cluster correctness (Phase 6): re-read the authoritative shared-DB row BEFORE
    // incrementing, so kicks landing on different nodes accumulate instead of each
    // node clobbering the row from a stale local cache. The DB read is done WITHOUT
    // the score lock (DB calls must not hold the mutex); we reconcile under the lock
    // below. Identical threading to the PersistAccount write a few lines later.
    AccountState fresh;
    bool haveRow = ReadAccountRow(accountId, fresh);

    bool queueBan = false;
    uint32 duration = 0;
    AccountState snapshot;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        AccountState& s = m_accounts[accountId];
        // Adopt the authoritative row so we never build on a stale local value.
        if (haveRow)
        {
            s.kickScore  = fresh.kickScore;
            s.banCount   = fresh.banCount;
            s.lastUpdate = fresh.lastUpdate;
        }
        DecayedKickScore(s, nowSec);
        s.kickScore += PerTypeKickWeight(type);   // per-type weighting (Phase 6)

        if (s.kickScore >= float(m_autobanThreshold))
        {
            // Escalating duration by prior ban count (last tier is sticky).
            uint32 tier = s.banCount < 3 ? s.banCount : 2;
            duration = m_autobanDur[tier];
            ++s.banCount;
            s.kickScore = 0.0f; // reset accumulator after a ban
            queueBan = true;
        }
        snapshot = s;

        if (queueBan)
        {
            PendingBan pb;
            pb.charName = charName;
            pb.durationSecs = duration;
            pb.reason = "Automated: repeated anti-cheat kicks (cluster-wide)";
            m_pendingBans.push_back(pb);
        }
    }

    PersistAccount(accountId, snapshot);

    if (queueBan)
        sLog.outError("AntiCheat: account %u queued for autoban (%us) after repeated kicks (char '%s')",
                      accountId, duration, charName.c_str());
}

void AntiCheatMgr::PersistAccount(uint32 accountId, AccountState const& s)
{
    // Account-level aggregate lives in the realm DB (spans characters/realms).
    LoginDatabase.PExecute(
        "REPLACE INTO `account_anticheat` (`account`,`kick_score`,`ban_count`,`last_update`) "
        "VALUES (%u, %f, %u, %u)",
        accountId, s.kickScore, s.banCount, s.lastUpdate);
}

void AntiCheatMgr::LoadAccounts()
{
    std::lock_guard<std::mutex> guard(m_lock);
    m_accounts.clear();
    QueryResult* result = LoginDatabase.Query(
        "SELECT `account`,`kick_score`,`ban_count`,`last_update` FROM `account_anticheat`");
    if (!result)
        return;
    do
    {
        Field* f = result->Fetch();
        AccountState s;
        s.kickScore = f[1].GetFloat();
        s.banCount  = f[2].GetUInt32();
        s.lastUpdate = f[3].GetUInt32();
        m_accounts[f[0].GetUInt32()] = s;
    }
    while (result->NextRow());
    delete result;
    sLog.outString("AntiCheat: loaded %u account autoban records.", uint32(m_accounts.size()));
}

bool AntiCheatMgr::GetAccountAutobanState(uint32 accountId, float& kickScore,
                                          uint32& banCount, uint32& lastUpdate)
{
    // Read-through so a GM sees the authoritative CLUSTER-WIDE value (shared realm
    // DB), not this node's possibly-stale cache. Decay to "now" for display.
    AccountState s;
    bool have = ReadAccountRow(accountId, s);
    uint32 nowSec = uint32(sWorld.GetGameTime());
    DecayedKickScore(s, nowSec);
    kickScore  = s.kickScore;
    banCount   = s.banCount;
    lastUpdate = s.lastUpdate;
    // Keep the local cache consistent with what we just read.
    {
        std::lock_guard<std::mutex> guard(m_lock);
        m_accounts[accountId] = s;
    }
    return have;
}

void AntiCheatMgr::ResetAccount(uint32 accountId)
{
    LoginDatabase.PExecute("DELETE FROM `account_anticheat` WHERE `account`=%u", accountId);
    std::lock_guard<std::mutex> guard(m_lock);
    m_accounts.erase(accountId);
}

void AntiCheatMgr::CorrelateByIp(uint32 accountId, std::vector<AntiCheatAlt>& out)
{
    out.clear();

    // Look up the banned/target account's most-recent IP (shared realm DB).
    QueryResult* ipRes = LoginDatabase.PQuery(
        "SELECT `last_ip` FROM `account` WHERE `id`=%u", accountId);
    if (!ipRes)
        return;
    std::string ip = ipRes->Fetch()[0].GetCppString();
    delete ipRes;
    if (ip.empty())
        return;

    // Peers sharing the most-recent IP, excluding the account itself. last_ip
    // collisions are common+legitimate (NAT, households, cafés, dynamic-IP reuse),
    // so this is best-effort and FLAG-ONLY — never an automatic ban.
    QueryResult* peers = LoginDatabase.PQuery(
        "SELECT `id`,`username`,`last_ip`,`gmlevel` FROM `account` "
        "WHERE `last_ip`='%s' AND `id`<>%u", ip.c_str(), accountId);
    if (!peers)
        return;

    do
    {
        Field* pf = peers->Fetch();
        AntiCheatAlt alt;
        alt.accountId = pf[0].GetUInt32();
        alt.username  = pf[1].GetCppString();
        alt.lastIp    = pf[2].GetCppString();
        alt.isGm      = pf[3].GetUInt32() > 0;
        if (alt.isGm)               // never flag GM accounts
            continue;

        AccountState st;
        ReadAccountRow(alt.accountId, st);
        alt.kickScore = st.kickScore;
        alt.banCount  = st.banCount;

        QueryResult* banRes = LoginDatabase.PQuery(
            "SELECT 1 FROM `account_banned` WHERE `id`=%u AND `active`=1 LIMIT 1", alt.accountId);
        alt.banned = (banRes != NULL);
        if (banRes)
            delete banRes;

        out.push_back(alt);
    }
    while (peers->NextRow());
    delete peers;
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
