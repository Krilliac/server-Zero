/*
 * GM chat commands for the Anti-Cheat framework: .anticheat status/report/reload.
 */

#include "Chat.h"
#include "AntiCheatMgr.h"
#include "Player.h"
#include "World.h"
#include "ObjectMgr.h"
#include "Log.h"
#include "Config/Config.h"
#include "Database/DatabaseEnv.h"

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
        SendSysMessage("AntiCheat: select a player to warn.");
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
        SendSysMessage("AntiCheat: select an online player to jail.");
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
        SendSysMessage("AntiCheat: select an online player to release.");
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
        SendSysMessage("AntiCheat: select a player or pass a character name.");
        SetSentErrorMessage(true);
        return false;
    }

    CharacterDatabase.PExecute("DELETE FROM `character_anticheat_violation` WHERE `guid`=%u", guid);
    sAntiCheatMgr->RemovePlayer(guid);
    PSendSysMessage("AntiCheat: cleared violation records for %s (guid %u).", name.empty() ? "?" : name.c_str(), guid);
    return true;
}
