/*
 * Cluster GM commands (Phase 0/1/3).
 *   .cluster status   — node id, enabled state, local player count
 *   .cluster selftest — serialize the selected/self player and verify the
 *                       migration blob round-trips (integrity + key fields),
 *                       without mutating the live player.
 */

#include "Chat.h"
#include "Player.h"
#include "World.h"
#include "ClusterMgr.h"
#include "ClusterMessage.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Map.h"
#include "GridMap.h"
#include "DebugVis.h"
#include "ByteBuffer.h"
#include "Auth/Sha1.h"

#include <cstring>
#include <cmath>
#include <vector>

bool ChatHandler::HandleClusterStatusCommand(char* /*args*/)
{
    PSendSysMessage("Cluster: %s | this node id = %u | local players = %u",
                    sClusterMgr->IsEnabled() ? "ENABLED" : "disabled",
                    sClusterMgr->GetNodeId(),
                    sClusterMgr->GetLocalPlayerCount());
    PSendSysMessage("Optimal (least-loaded online) node: %u", sClusterMgr->GetOptimalNode());
    return true;
}

bool ChatHandler::HandleClusterSelfTestCommand(char* /*args*/)
{
    Player* target = getSelectedPlayer();
    if (!target)
        target = m_session ? m_session->GetPlayer() : NULL;
    if (!target)
    {
        SendSysMessage("Cluster selftest: select a player (or run in-game).");
        SetSentErrorMessage(true);
        return false;
    }

    // Section ids must match Player.cpp's migration format.
    const uint32 MAGIC = 0x304C434D; // 'MCL0'
    enum { S_IDENTITY = 1, S_POSITION = 2, S_FIELDS = 3, S_MONEY = 4, S_END = 0xFFFF };

    ByteBuffer buf;
    target->SerializeForMigration(buf);

    if (buf.size() < 32)
    {
        PSendSysMessage("Cluster selftest: FAIL (blob too small: %u bytes).", (uint32)buf.size());
        SetSentErrorMessage(true);
        return false;
    }

    bool ok = true;
    std::string diffs;

    // 1) trailing SHA1 integrity
    size_t payloadLen = buf.size() - SHA_DIGEST_LENGTH;
    {
        Sha1Hash sha;
        sha.Initialize();
        sha.UpdateData(buf.contents(), payloadLen);
        sha.Finalize();
        if (memcmp(sha.GetDigest(), buf.contents() + payloadLen, SHA_DIGEST_LENGTH) != 0)
        {
            ok = false;
            diffs += " [SHA1 mismatch]";
        }
    }

    // 2) header + key sections vs live values
    buf.rpos(0);
    uint32 magic; buf >> magic;
    uint16 ver;   buf >> ver;
    uint32 guid;  buf >> guid;
    if (magic != MAGIC) { ok = false; diffs += " [bad magic]"; }
    if (guid != target->GetGUIDLow()) { ok = false; diffs += " [guid]"; }

    uint32 sLevel = 0, sMap = 0xFFFFFFFF, sMoney = 0, sFields = 0;
    float sx = 0, sy = 0, sz = 0, so = 0;
    while (buf.rpos() < payloadLen)
    {
        uint16 id; buf >> id;
        if (id == S_END)
            break;
        uint32 len; buf >> len;
        size_t end = buf.rpos() + len;
        if (end > payloadLen) { ok = false; diffs += " [truncated]"; break; }
        switch (id)
        {
            case S_IDENTITY:
            {
                std::string name; uint8 r, c, g;
                buf >> name >> r >> c >> g >> sLevel;
                break;
            }
            case S_POSITION:
                buf >> sMap >> sx >> sy >> sz >> so;
                break;
            case S_FIELDS:
                buf >> sFields;
                break;
            case S_MONEY:
                buf >> sMoney;
                break;
            default:
                break;
        }
        buf.rpos(end);
    }

    if (sLevel != target->getLevel())        { ok = false; diffs += " [level]"; }
    if (sMap   != target->GetMapId())        { ok = false; diffs += " [map]"; }
    if (sMoney != target->GetMoney())        { ok = false; diffs += " [money]"; }
    if (sFields != target->GetValuesCount()) { ok = false; diffs += " [fieldcount]"; }
    if (fabs(sx - target->GetPositionX()) > 0.01f ||
        fabs(sy - target->GetPositionY()) > 0.01f ||
        fabs(sz - target->GetPositionZ()) > 0.01f) { ok = false; diffs += " [pos]"; }

    // 3) tamper test: a single flipped payload byte must break the SHA1 check
    {
        std::vector<uint8> bytes(buf.contents(), buf.contents() + buf.size());
        bytes[10] ^= 0xFF; // flip a header/section byte
        Sha1Hash sha;
        sha.Initialize();
        sha.UpdateData(&bytes[0], payloadLen);
        sha.Finalize();
        if (memcmp(sha.GetDigest(), &bytes[0] + payloadLen, SHA_DIGEST_LENGTH) == 0)
        {
            ok = false;
            diffs += " [tamper-not-detected]";
        }
    }

    if (ok)
        PSendSysMessage("Cluster selftest: PASS — %u-byte blob, %u fields, lvl %u, map %u, money %u (SHA1 + tamper OK).",
                        (uint32)buf.size(), sFields, sLevel, sMap, sMoney);
    else
        PSendSysMessage("Cluster selftest: FAIL —%s", diffs.c_str());

    return true;
}

bool ChatHandler::HandleClusterMigrateCommand(char* args)
{
    if (!sClusterMgr->IsEnabled())
    {
        SendSysMessage("Cluster: disabled (Cluster.Enable = 0).");
        SetSentErrorMessage(true);
        return false;
    }
    if (!sClusterMgr->IsMigrationEnabled())
    {
        SendSysMessage("Cluster: migration disabled (Cluster.EnableMigration = 0).");
        SetSentErrorMessage(true);
        return false;
    }

    char* nameStr = ExtractArg(&args);
    char* nodeStr = ExtractArg(&args);
    if (!nameStr || !nodeStr)
    {
        SendSysMessage("Syntax: .cluster migrate <playerName> <nodeId>");
        SetSentErrorMessage(true);
        return false;
    }

    uint32 nodeId = (uint32)atoi(nodeStr);
    std::string name = nameStr;
    if (!normalizePlayerName(name))
    {
        SendSysMessage("Cluster: invalid player name.");
        SetSentErrorMessage(true);
        return false;
    }

    Player* target = sObjectAccessor.FindPlayerByName(name.c_str());
    if (!target)
    {
        PSendSysMessage("Cluster: player '%s' is not online on this node.", name.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    if (target->MigrateToNode(nodeId))
        PSendSysMessage("Cluster: migrating %s to node %u (client will reconnect).",
                        target->GetName(), nodeId);
    else
        PSendSysMessage("Cluster: migration of %s to node %u refused — check the server log "
                        "(CanMigrate gate / target node id).", name.c_str(), nodeId);
    return true;
}

bool ChatHandler::HandleClusterVisualCommand(char* args)
{
    if (!args || !*args)
    {
        PSendSysMessage("Cluster visual debug: %s (this node %u, indicator kit %u). Usage: .cluster visual <on|off>",
                        sClusterMgr->VisualDebugEnabled() ? "ON" : "off",
                        sClusterMgr->GetNodeId(),
                        sClusterMgr->GetNodeVisualKit(sClusterMgr->GetNodeId()));
        return true;
    }

    std::string a = args;
    bool on  = (a == "on"  || a == "1" || a == "enable");
    bool off = (a == "off" || a == "0" || a == "disable");
    if (!on && !off)
    {
        SendSysMessage("Usage: .cluster visual <on|off>");
        SetSentErrorMessage(true);
        return false;
    }

    sClusterMgr->SetVisualDebug(on);
    PSendSysMessage("Cluster visual debug %s on node %u (per-node indicator kit %u; migration burst kit %u). "
                    "Note: applies to this node only — run on each node to toggle cluster-wide.",
                    on ? "ENABLED" : "disabled", sClusterMgr->GetNodeId(),
                    sClusterMgr->GetNodeVisualKit(sClusterMgr->GetNodeId()),
                    sClusterMgr->GetMigrateVisualKit());
    return true;
}

namespace
{
    // Parse on/off/1/0/enable/disable. Returns true if recognized (sets *out).
    bool ParseOnOff(const char* a, bool* out)
    {
        std::string s = a ? a : "";
        if (s == "on" || s == "1" || s == "enable")  { *out = true;  return true; }
        if (s == "off" || s == "0" || s == "disable") { *out = false; return true; }
        return false;
    }
}

bool ChatHandler::HandleClusterAutoMigrateCommand(char* args)
{
    if (!args || !*args)
    {
        PSendSysMessage("Cluster auto-migrate: %s (this node %u). Usage: .cluster automigrate <on|off>",
                        sClusterMgr->AutoMigrateEnabled() ? "ON" : "off", sClusterMgr->GetNodeId());
        return true;
    }
    bool on;
    if (!ParseOnOff(args, &on))
    {
        SendSysMessage("Usage: .cluster automigrate <on|off>");
        SetSentErrorMessage(true);
        return false;
    }
    sClusterMgr->SetAutoMigrate(on);
    PSendSysMessage("Cluster auto-migrate %s on node %u (applies to this node only).",
                    on ? "ENABLED" : "disabled", sClusterMgr->GetNodeId());
    return true;
}

bool ChatHandler::HandleClusterMigrationCommand(char* args)
{
    if (!args || !*args)
    {
        PSendSysMessage("Cluster migration: %s (this node %u). Usage: .cluster migration <on|off>",
                        sClusterMgr->IsMigrationEnabled() ? "ON" : "off", sClusterMgr->GetNodeId());
        return true;
    }
    bool on;
    if (!ParseOnOff(args, &on))
    {
        SendSysMessage("Usage: .cluster migration <on|off>");
        SetSentErrorMessage(true);
        return false;
    }
    sClusterMgr->SetMigrationEnabled(on);
    PSendSysMessage("Cluster migration %s on node %u (applies to this node only).",
                    on ? "ENABLED" : "disabled", sClusterMgr->GetNodeId());
    return true;
}

bool ChatHandler::HandleClusterReloadZonesCommand(char* /*args*/)
{
    sClusterMgr->ReloadZoneMap();
    SendSysMessage("Cluster: reloaded the zone->node assignment map from cluster_zone_assignment.");
    return true;
}

bool ChatHandler::HandleClusterChatTagCommand(char* args)
{
    if (!args || !*args)
    {
        PSendSysMessage("Cluster chat-tag: %s (this node %u). Usage: .cluster chattag <on|off>",
                        sClusterMgr->ChatTagEnabled() ? "ON" : "off", sClusterMgr->GetNodeId());
        return true;
    }
    bool on;
    if (!ParseOnOff(args, &on))
    {
        SendSysMessage("Usage: .cluster chattag <on|off>");
        SetSentErrorMessage(true);
        return false;
    }
    sClusterMgr->SetChatTag(on);
    PSendSysMessage("Cluster chat-tag %s on node %u (prefixes outgoing chat with [N%u]).",
                    on ? "ENABLED" : "disabled", sClusterMgr->GetNodeId(), sClusterMgr->GetNodeId());
    return true;
}

bool ChatHandler::HandleClusterBoundariesCommand(char* args)
{
    Player* player = m_session ? m_session->GetPlayer() : NULL;
    if (!player)
        return false;
    if (!sClusterMgr->IsEnabled())
    {
        SendSysMessage("Cluster: disabled (Cluster.Enable=0).");
        SetSentErrorMessage(true);
        return false;
    }

    float radius = 80.0f, step = 16.0f;
    if (args && *args)
    {
        float r = 0.0f, s = 0.0f;
        int n = sscanf(args, "%f %f", &r, &s);
        if (n >= 1 && r > 0.0f) radius = r;
        if (n >= 2 && s > 0.0f) step = s;
    }
    if (step < 4.0f) step = 4.0f;
    if (radius < step) radius = step;
    int half = int(radius / step);
    if (half > 8) half = 8; // cap (2*8+1)^2 = 289 markers

    Map* map = player->GetMap();
    float px = player->GetPositionX(), py = player->GetPositionY(), pz = player->GetPositionZ();
    uint32 myNode = sClusterMgr->GetNodeId();

    uint32 placed = 0, otherNode = 0;
    for (int i = -half; i <= half; ++i)
    {
        for (int j = -half; j <= half; ++j)
        {
            float wx = px + i * step;
            float wy = py + j * step;
            float wz = map->GetHeight(wx, wy, pz + 50.0f);
            if (wz < -50000.0f)
                continue; // no ground here

            uint32 zone  = map->GetTerrain()->GetZoneId(wx, wy, wz);
            uint32 node  = sClusterMgr->GetNodeForZone(zone); // 0 = unassigned
            uint32 owner = node ? node : myNode;              // unassigned zones stay on the current node

            // Colours are RELATIVE to the player's current node: green = this node
            // (you stay), red = a different node (crossing there migrates you). So
            // the colours swap when you move across to the other node.
            bool sameNode = (owner == myNode);
            DebugVis::Category cat = sameNode ? DebugVis::DV_LOS_OK     // this node  -> green
                                              : DebugVis::DV_LOS_BLOCK; // other node -> red
            if (!sameNode)
                ++otherNode;

            char lbl[200];
            snprintf(lbl, sizeof(lbl),
                     "Cluster boundary\nZone %u -> Node %u%s\n(%.1f, %.1f)",
                     zone, owner,
                     (sameNode ? " (this node - green)" : " (OTHER node - red - crossing migrates you)"),
                     wx, wy);
            if (DebugVis::Marker(player, cat, wx, wy, wz, lbl))
                ++placed;
        }
    }

    PSendSysMessage("Cluster: drew %u boundary markers (%u on other nodes) around you on node %u. "
                    "Colour change = node boundary; hover a marker for zone/node. Despawn in %us "
                    "(or .debug vis clear).",
                    placed, otherNode, myNode, DebugVis::DespawnSeconds());
    return true;
}

bool ChatHandler::HandleClusterAnnounceCommand(char* args)
{
    if (!args || !*args)
    {
        SendSysMessage("Syntax: .cluster announce <text>");
        SetSentErrorMessage(true);
        return false;
    }

    std::string text = args;

    // Route through the ANNOUNCE service role. RouteAnnounce() decides:
    //  - cluster off / role unconfigured / owner offline -> local fallback (here & now)
    //  - we ARE the owner -> local + broadcast result to peers
    //  - live remote owner -> directed request; owner runs it and fans out the result
    // Either way the announcement happens; it never silently fails.
    bool routed = sClusterMgr->RouteAnnounce(text);

    uint32 owner = sClusterMgr->GetServiceOwner(CLUSTER_SERVICE_ANNOUNCE);
    if (!sClusterMgr->IsEnabled())
        PSendSysMessage("Cluster announce (local — cluster disabled): %s", text.c_str());
    else if (routed)
        PSendSysMessage("Cluster announce routed to ANNOUNCE-service owner node %u.", owner);
    else if (owner == sClusterMgr->GetNodeId())
        PSendSysMessage("Cluster announce performed as ANNOUNCE-service owner (node %u) and fanned out cluster-wide.",
                        owner);
    else
        PSendSysMessage("Cluster announce (local fallback — role owner %u unconfigured/offline): %s",
                        owner, text.c_str());
    return true;
}

// --- Cross-node relay test commands (console-runnable; for end-to-end testing) ---
// Each fires the real relay sender with a synthetic sender identity, so the peer
// nodes run their genuine receive/deliver paths. Watch the PEER node's log for the
// "Cluster: ... received relayed ..." line to confirm transport + delivery.

bool ChatHandler::HandleClusterSimWhisperCommand(char* args)
{
    if (!sClusterMgr->IsEnabled())
    {
        SendSysMessage("Cluster is disabled (Cluster.Enable=0); nothing to relay.");
        SetSentErrorMessage(true);
        return false;
    }
    char* to   = strtok(args, " ");
    char* text = strtok(NULL, "");
    if (!to || !text)
    {
        SendSysMessage("Syntax: .cluster simwhisper <toName> <text>");
        SetSentErrorMessage(true);
        return false;
    }
    sClusterMgr->SendChatRelay(CHAT_MSG_WHISPER, LANG_UNIVERSAL, 0, 0, "ClusterTest", to, text);
    PSendSysMessage("Broadcast a cross-node WHISPER relay to '%s'. Check the target node's log.", to);
    return true;
}

bool ChatHandler::HandleClusterSimGuildCommand(char* args)
{
    if (!sClusterMgr->IsEnabled())
    {
        SendSysMessage("Cluster is disabled (Cluster.Enable=0); nothing to relay.");
        SetSentErrorMessage(true);
        return false;
    }
    char* idStr = strtok(args, " ");
    char* text  = strtok(NULL, "");
    if (!idStr || !text)
    {
        SendSysMessage("Syntax: .cluster simguild <guildId> <text>");
        SetSentErrorMessage(true);
        return false;
    }
    uint32 guildId = (uint32)atoi(idStr);
    sClusterMgr->SendChatRelay(CHAT_MSG_GUILD, LANG_UNIVERSAL, 0, 0, "ClusterTest", "", text, guildId, 0);
    PSendSysMessage("Broadcast a cross-node GUILD relay for guild %u. Check the peer node's log.", guildId);
    return true;
}

bool ChatHandler::HandleClusterSimGroupCommand(char* args)
{
    if (!sClusterMgr->IsEnabled())
    {
        SendSysMessage("Cluster is disabled (Cluster.Enable=0); nothing to relay.");
        SetSentErrorMessage(true);
        return false;
    }
    char* idStr = strtok(args, " ");
    char* text  = strtok(NULL, "");
    if (!idStr || !text)
    {
        SendSysMessage("Syntax: .cluster simgroup <groupId> <text>");
        SetSentErrorMessage(true);
        return false;
    }
    uint32 groupId = (uint32)atoi(idStr);
    sClusterMgr->SendGroupChatRelay(CHAT_MSG_PARTY, LANG_UNIVERSAL, groupId, 0, 0, "ClusterTest", text, -1);
    PSendSysMessage("Broadcast a cross-node PARTY relay for group %u. Check the peer node's log.", groupId);
    return true;
}
