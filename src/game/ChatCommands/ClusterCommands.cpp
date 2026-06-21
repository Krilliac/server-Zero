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
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
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
