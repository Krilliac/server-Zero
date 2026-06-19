/*
 * GM chat commands for the Anti-Cheat framework: .anticheat status/report/reload.
 */

#include "Chat.h"
#include "AntiCheatMgr.h"
#include "MovementAnticheat.h"
#include "Player.h"
#include "World.h"
#include "ObjectMgr.h"
#include "Log.h"
#include "Config/Config.h"
#include "Database/DatabaseEnv.h"

#include <cstring>
#include <cctype>
#include <cstdlib>

bool ChatHandler::HandleAntiCheatStatusCommand(char* /*args*/)
{
    Player* target = getSelectedPlayer();
    if (!target)
        target = m_session ? m_session->GetPlayer() : NULL;
    if (!target)
    {
        SendSysMessage("AntiCheat: select a player (or run in-game).");
        SetSentErrorMessage(true);
        return false;
    }

    std::string out;
    sAntiCheatMgr->BuildStatus(target, out);
    SendSysMessage(out.c_str());
    return true;
}

bool ChatHandler::HandleAntiCheatReportCommand(char* /*args*/)
{
    Player* target = getSelectedPlayer();
    if (!target)
        target = m_session ? m_session->GetPlayer() : NULL;
    if (!target)
    {
        SendSysMessage("AntiCheat: select a player (or run in-game).");
        SetSentErrorMessage(true);
        return false;
    }

    QueryResult* result = CharacterDatabase.PQuery(
        "SELECT `type`,`score`,`map`,`detail`,`time` FROM `character_anticheat_violation` "
        "WHERE `guid`=%u ORDER BY `id` DESC LIMIT 10", target->GetGUIDLow());

    if (!result)
    {
        PSendSysMessage("AntiCheat: no recorded violations for %s.", target->GetName());
        return true;
    }

    PSendSysMessage("AntiCheat: last violations for %s:", target->GetName());
    do
    {
        Field* f = result->Fetch();
        PSendSysMessage("  type=%u score=%u map=%u  %s  [%s]",
                        f[0].GetUInt32(), f[1].GetUInt32(), f[2].GetUInt32(),
                        f[3].GetCppString().c_str(), f[4].GetCppString().c_str());
    }
    while (result->NextRow());
    delete result;
    return true;
}

bool ChatHandler::HandleAntiCheatReloadCommand(char* /*args*/)
{
    sAntiCheatMgr->LoadConfig();
    SendSysMessage("AntiCheat: configuration reloaded.");
    return true;
}

bool ChatHandler::HandleAntiCheatWarnCommand(char* /*args*/)
{
    Player* target = getSelectedPlayer();
    if (!target)
    {
        SendSysMessage(".anticheat warn FAILED: no player selected. Select/target an online "
                       "player, then run .anticheat warn (sends them a warning).");
        SetSentErrorMessage(true);
        return false;
    }
    if (target->GetSession())
        target->GetSession()->SendNotification("[AntiCheat] You have been warned by a GM for suspicious activity.");
    PSendSysMessage("AntiCheat: warned %s.", target->GetName());
    sLog.outBasic("AntiCheat: GM %s warned %s (guid %u)",
                  m_session ? m_session->GetPlayerName() : "CONSOLE", target->GetName(), target->GetGUIDLow());
    return true;
}

bool ChatHandler::HandleAntiCheatJailCommand(char* /*args*/)
{
    Player* target = getSelectedPlayer();
    if (!target)
    {
        SendSysMessage(".anticheat jail FAILED: no player selected. Select/target an online "
                       "player, then run .anticheat jail (teleports them to the configured jail).");
        SetSentErrorMessage(true);
        return false;
    }
    // Configurable jail location (defaults to the GM transport map cell).
    uint32 jmap = uint32(sConfig.GetIntDefault("AntiCheat.Jail.Map", 13));
    float jx = sConfig.GetFloatDefault("AntiCheat.Jail.X", -109.0f);
    float jy = sConfig.GetFloatDefault("AntiCheat.Jail.Y", -1.0f);
    float jz = sConfig.GetFloatDefault("AntiCheat.Jail.Z", -2.4f);
    float jo = sConfig.GetFloatDefault("AntiCheat.Jail.O", 3.14f);
    target->TeleportTo(jmap, jx, jy, jz, jo);
    if (target->GetSession())
        target->GetSession()->SendNotification("[AntiCheat] You have been jailed by a GM.");
    PSendSysMessage("AntiCheat: jailed %s (map %u: %.1f, %.1f, %.1f).", target->GetName(), jmap, jx, jy, jz);
    sLog.outBasic("AntiCheat: GM %s jailed %s (guid %u)",
                  m_session ? m_session->GetPlayerName() : "CONSOLE", target->GetName(), target->GetGUIDLow());
    return true;
}

bool ChatHandler::HandleAntiCheatUnjailCommand(char* /*args*/)
{
    Player* target = getSelectedPlayer();
    if (!target)
    {
        SendSysMessage(".anticheat unjail FAILED: no player selected. Select/target an online "
                       "player, then run .anticheat unjail (returns them to their homebind).");
        SetSentErrorMessage(true);
        return false;
    }
    target->TeleportToHomebind();
    if (target->GetSession())
        target->GetSession()->SendNotification("[AntiCheat] You have been released.");
    PSendSysMessage("AntiCheat: released %s to homebind.", target->GetName());
    return true;
}

bool ChatHandler::HandleAntiCheatDeleteCommand(char* args)
{
    uint32 guid = 0;
    std::string name;
    if (Player* target = getSelectedPlayer())
    {
        guid = target->GetGUIDLow();
        name = target->GetName();
    }
    else if (args && *args)
    {
        name = args;
        ObjectGuid og = sObjectMgr.GetPlayerGuidByName(name);
        if (og)
            guid = og.GetCounter();
    }

    if (!guid)
    {
        SendSysMessage(".anticheat delete FAILED: no player. Select/target a player OR pass a "
                       "character name (.anticheat delete <name>) to clear their violation records.");
        SetSentErrorMessage(true);
        return false;
    }

    CharacterDatabase.PExecute("DELETE FROM `character_anticheat_violation` WHERE `guid`=%u", guid);
    sAntiCheatMgr->RemovePlayer(guid);
    PSendSysMessage("AntiCheat: cleared violation records for %s (guid %u).", name.empty() ? "?" : name.c_str(), guid);
    return true;
}

// Names <-> violation types for the `.anticheat test` dev/debug command.
namespace
{
    struct AcTypeName { const char* name; AntiCheatViolationType type; };
    static const AcTypeName s_acTypeNames[] =
    {
        { "speed",        AC_VIOLATION_SPEED },
        { "teleport",     AC_VIOLATION_TELEPORT },
        { "vertical",     AC_VIOLATION_VERTICAL },
        { "flag",         AC_VIOLATION_FLAG_CONTRADICT },
        { "physics",      AC_VIOLATION_PHYSICS },
        { "desync",       AC_VIOLATION_DESYNC },
        { "jump",         AC_VIOLATION_JUMP },
        { "fall",         AC_VIOLATION_FALL },
        { "burst",        AC_VIOLATION_BURST },
        { "packettiming", AC_VIOLATION_PACKETTIMING },
        { "spell",        AC_VIOLATION_SPELL },
        { "item",         AC_VIOLATION_ITEM },
    };
}

bool ChatHandler::HandleAntiCheatTestCommand(char* args)
{
    char* tok = strtok(args, " ");
    if (!tok)
    {
        SendSysMessage(".anticheat test FAILED: needs a subcommand. Usage:");
        SendSysMessage("  .anticheat test list                 - list violation type names");
        SendSysMessage("  .anticheat test config               - dump live AntiCheat config");
        SendSysMessage("  .anticheat test <type> [weight]      - inject one violation (default 25)");
        SendSysMessage("  .anticheat test all [weight]         - inject every type (default 10)");
        SendSysMessage("Injects on your target (or yourself). Bypasses enabled/exempt so the full");
        SendSysMessage("pipeline runs: scoring, decay, DB persist, marker, warn/rubberband/kick/autoban.");
        SetSentErrorMessage(true);
        return false;
    }

    std::string sub = tok;
    for (size_t i = 0; i < sub.size(); ++i) sub[i] = (char)tolower(sub[i]);

    const uint32 count = uint32(sizeof(s_acTypeNames) / sizeof(s_acTypeNames[0]));

    if (sub == "list")
    {
        SendSysMessage("AntiCheat violation types:");
        for (uint32 i = 0; i < count; ++i)
            PSendSysMessage("  %u = %s", uint32(s_acTypeNames[i].type), s_acTypeNames[i].name);
        return true;
    }

    if (sub == "config")
    {
        std::string out;
        sAntiCheatMgr->BuildDiag(out);
        SendSysMessage(out.c_str());
        return true;
    }

    Player* target = getSelectedPlayer();
    if (!target)
        target = m_session ? m_session->GetPlayer() : NULL;
    if (!target)
    {
        SendSysMessage(".anticheat test FAILED: no player. Select/target a player or run in-game.");
        SetSentErrorMessage(true);
        return false;
    }

    AntiCheatContext ctx;
    ctx.mapId = target->GetMapId();
    ctx.x = target->GetPositionX(); ctx.y = target->GetPositionY(); ctx.z = target->GetPositionZ();
    ctx.latency = target->GetSession() ? target->GetSession()->GetLatencyEWMA() : 0;
    ctx.detail = "manual test injection";

    if (!sAntiCheatMgr->IsEnabled())
        SendSysMessage("AntiCheat note: framework is DISABLED in config — live detection is off, "
                       "but this test still drives scoring/punishment (markers need it enabled).");

    if (sub == "all")
    {
        char* w = strtok(NULL, " ");
        float weight = w ? float(atof(w)) : 10.0f;
        for (uint32 i = 0; i < count; ++i)
            sAntiCheatMgr->TestInject(target, s_acTypeNames[i].type, weight, ctx);
        PSendSysMessage("AntiCheat test: injected all %u types at weight %.0f on %s.",
                        count, weight, target->GetName());
    }
    else
    {
        AntiCheatViolationType type = AC_VIOLATION_NONE;
        int numeric = atoi(sub.c_str());
        for (uint32 i = 0; i < count; ++i)
            if (sub == s_acTypeNames[i].name || (numeric && numeric == int(s_acTypeNames[i].type)))
            {
                type = s_acTypeNames[i].type;
                break;
            }
        if (type == AC_VIOLATION_NONE)
        {
            PSendSysMessage(".anticheat test FAILED: unknown type '%s'. Use .anticheat test list.", sub.c_str());
            SetSentErrorMessage(true);
            return false;
        }
        char* w = strtok(NULL, " ");
        float weight = w ? float(atof(w)) : 25.0f;
        sAntiCheatMgr->TestInject(target, type, weight, ctx);
        PSendSysMessage("AntiCheat test: injected type %u weight %.0f on %s.",
                        uint32(type), weight, target->GetName());
    }

    std::string st;
    sAntiCheatMgr->BuildStatus(target, st);
    SendSysMessage(st.c_str());
    return true;
}

// --- Dedicated AC-vector GM tools (manipulate the mechanics directly) ---

bool ChatHandler::HandleAntiCheatRubberbandCommand(char* /*args*/)
{
    Player* target = getSelectedPlayer();
    if (!target)
        target = m_session ? m_session->GetPlayer() : NULL;
    if (!target)
    {
        SendSysMessage(".anticheat rubberband FAILED: no player. Select/target a player or run "
                       "in-game. It yanks them back to their last AC-validated position.");
        SetSentErrorMessage(true);
        return false;
    }

    MovementAnticheat* mac = target->GetMovementAnticheat();
    if (mac && mac->HasValid())
    {
        target->NearTeleportTo(mac->ValidX(), mac->ValidY(), mac->ValidZ(), mac->ValidO());
        PSendSysMessage("AntiCheat: rubberbanded %s to last valid (%.1f, %.1f, %.1f).",
                        target->GetName(), mac->ValidX(), mac->ValidY(), mac->ValidZ());
    }
    else
    {
        SendSysMessage("AntiCheat: no validated position yet for that player (they haven't moved "
                       "since login). Nothing to rubberband to.");
    }
    return true;
}

bool ChatHandler::HandleAntiCheatResyncCommand(char* /*args*/)
{
    Player* target = getSelectedPlayer();
    if (!target)
        target = m_session ? m_session->GetPlayer() : NULL;
    if (!target)
    {
        SendSysMessage(".anticheat resync FAILED: no player. Select/target a player or run in-game. "
                       "It rubberbands them to their current authoritative position (clock resync).");
        SetSentErrorMessage(true);
        return false;
    }

    target->NearTeleportTo(target->GetPositionX(), target->GetPositionY(),
                           target->GetPositionZ(), target->GetOrientation());
    if (MovementAnticheat* mac = target->GetMovementAnticheat())
        mac->NotifyServerRelocation();
    PSendSysMessage("AntiCheat: resynced %s to current position.", target->GetName());
    return true;
}

bool ChatHandler::HandleAntiCheatTimeSkipCommand(char* args)
{
    char* tok = strtok(args, " ");
    if (!tok)
    {
        SendSysMessage(".anticheat timeskip FAILED: needs milliseconds. Usage: .anticheat timeskip "
                       "<ms>. Feeds a synthetic client time-skip to the target (drives the time-sync "
                       "service; large values score as a time hack).");
        SetSentErrorMessage(true);
        return false;
    }
    uint32 ms = uint32(atoi(tok));

    Player* target = getSelectedPlayer();
    if (!target)
        target = m_session ? m_session->GetPlayer() : NULL;
    if (!target)
    {
        SendSysMessage(".anticheat timeskip FAILED: no player. Select/target a player or run in-game.");
        SetSentErrorMessage(true);
        return false;
    }

    if (MovementAnticheat* mac = target->GetMovementAnticheat())
        mac->NotifyClientTimeSkip(ms);
    PSendSysMessage("AntiCheat: fed a %u ms time-skip to %s.", ms, target->GetName());
    return true;
}

bool ChatHandler::HandleAntiCheatScoreCommand(char* args)
{
    Player* target = getSelectedPlayer();
    if (!target)
        target = m_session ? m_session->GetPlayer() : NULL;
    if (!target)
    {
        SendSysMessage(".anticheat score FAILED: no player. Select/target a player or run in-game. "
                       "Usage: .anticheat score [value] (no value = show current).");
        SetSentErrorMessage(true);
        return false;
    }

    char* tok = strtok(args, " ");
    if (tok)
    {
        float val = float(atof(tok));
        sAntiCheatMgr->SetScore(target, val);
        PSendSysMessage("AntiCheat: set %s score to %.0f (escalation evaluated).", target->GetName(), val);
    }

    std::string st;
    sAntiCheatMgr->BuildStatus(target, st);
    SendSysMessage(st.c_str());
    return true;
}
