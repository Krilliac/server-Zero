/*
 * Multi-node Cluster framework — central node manager (Phases 0-2).
 */

#include "ClusterMgr.h"
#include "ClusterNetwork.h"
#include "ClusterMessage.h"
#include "World.h"
#include "Log.h"
#include "Timer.h"
#include "Player.h"
#include "ObjectAccessor.h"
#include "Chat.h"
#include "WorldSession.h"
#include "WorldPacket.h"
#include "GuildMgr.h"
#include "Guild.h"
#include "ChannelMgr.h"
#include "Channel.h"
#include "ObjectMgr.h"
#include "Group.h"
#include "Config/Config.h"
#include "Database/DatabaseEnv.h"

ClusterMgr::ClusterMgr()
    : m_enabled(false), m_migrationEnabled(false), m_autoMigrate(false),
      m_autoFailover(false),
      m_visualDebug(false), m_chatTag(false), m_visualIntervalMs(6000),
      m_svcAnnounceNode(0),
      m_nodeId(1), m_port(0), m_peerPort(0), m_capacity(0),
      m_heartbeatSec(30), m_host("127.0.0.1"), m_net(NULL)
{
}

void ClusterMgr::LoadConfig()
{
    m_enabled          = sWorld.getConfig(CONFIG_BOOL_CLUSTER_ENABLE);
    m_migrationEnabled = sWorld.getConfig(CONFIG_BOOL_CLUSTER_MIGRATION);
    m_autoMigrate      = sWorld.getConfig(CONFIG_BOOL_CLUSTER_AUTOMIGRATE);
    // Auto-failover gate. Read straight from sConfig (no World.h enum needed); OFF by
    // default so a cluster never auto-mutates assignments until the operator opts in.
    m_autoFailover     = sConfig.GetBoolDefault("Cluster.AutoFailover", false);
    m_visualDebug      = sWorld.getConfig(CONFIG_BOOL_CLUSTER_VISUAL);
    m_visualIntervalMs = sWorld.getConfig(CONFIG_UINT32_CLUSTER_VISUAL_INTERVAL);
    m_chatTag          = sWorld.getConfig(CONFIG_BOOL_CLUSTER_CHATTAG);
    m_nodeId       = sWorld.getConfig(CONFIG_UINT32_CLUSTER_NODE_ID);
    m_port         = sWorld.getConfig(CONFIG_UINT32_CLUSTER_PORT);
    m_heartbeatSec = sWorld.getConfig(CONFIG_UINT32_CLUSTER_HEARTBEAT);
    if (m_heartbeatSec < 5)
        m_heartbeatSec = 5;
    m_peerPort     = sWorld.getConfig(CONFIG_UINT32_CLUSTER_PEER_PORT);
    m_capacity     = sWorld.GetPlayerAmountLimit();
    m_host         = sConfig.GetStringDefault("Cluster.Host", "127.0.0.1");

    // Phase 8: service-role owners. Read like every other Cluster.* option.
    LoadServiceConfig();

    // Load (or refresh, on .reload config) the zone->node assignment table.
    if (m_enabled)
        LoadZoneMap();
}

void ClusterMgr::LoadServiceConfig()
{
    // 0 (default) = role unconfigured -> every node handles it locally (no
    // decomposition). A non-zero value names the node that OWNS the role. Read
    // straight from sConfig so it can be set per node in mangosd.conf; cross-node
    // agreement on the value is the operator's responsibility (see design notes).
    m_svcAnnounceNode = (uint32)sConfig.GetIntDefault("Cluster.Service.AnnounceNode", 0);

    if (m_enabled && m_svcAnnounceNode)
        sLog.outString("Cluster: service role ANNOUNCE owned by node %u%s.",
                       m_svcAnnounceNode,
                       m_svcAnnounceNode == m_nodeId ? " (this node)" : "");
}

uint32 ClusterMgr::GetServiceOwner(uint8 role) const
{
    switch (role)
    {
        case CLUSTER_SERVICE_ANNOUNCE: return m_svcAnnounceNode;
        default:                       return 0; // unknown role -> local
    }
}

bool ClusterMgr::IsServiceOwnerOnline(uint32 owner) const
{
    if (!owner)
        return false;
    // The owner is "online" if its cluster_nodes row says so. MarkStaleOffline()
    // already flips dead peers to 'offline', so this reuses the heartbeat liveness
    // model — no separate health tracking. A NULL/empty result = treat as offline.
    QueryResult* result = LoginDatabase.PQuery(
        "SELECT 1 FROM `cluster_nodes` WHERE `node_id`=%u AND `status`='online' LIMIT 1",
        owner);
    bool online = (result != NULL);
    delete result;
    return online;
}

void ClusterMgr::LoadZoneMap()
{
    std::map<uint32, uint32> m;
    QueryResult* result = LoginDatabase.Query(
        "SELECT `zone_id`,`node_id` FROM `cluster_zone_assignment`");
    if (result)
    {
        do
        {
            Field* f = result->Fetch();
            uint32 z = f[0].GetUInt32();
            uint32 n = f[1].GetUInt32();
            if (z && n)
                m[z] = n;
        } while (result->NextRow());
        delete result;
    }

    std::lock_guard<std::mutex> guard(m_zoneLock);
    m_zoneMap.swap(m);
    sLog.outString("Cluster: loaded %u zone->node assignment(s).", (uint32)m_zoneMap.size());
}

uint32 ClusterMgr::GetNodeForZone(uint32 zoneId) const
{
    if (!m_enabled || !zoneId)
        return 0;
    std::lock_guard<std::mutex> guard(m_zoneLock);
    std::map<uint32, uint32>::const_iterator it = m_zoneMap.find(zoneId);
    return it != m_zoneMap.end() ? it->second : 0;
}

uint32 ClusterMgr::GetNodeVisualKit(uint32 nodeId) const
{
    // Curated, confirmed-visible SpellVisualKit ids — distinct per node, cycling.
    static const uint32 palette[] = { 179, 5670, 686, 451, 300, 1027 };
    static const uint32 count = sizeof(palette) / sizeof(palette[0]);
    if (nodeId == 0)
        nodeId = 1;
    return palette[(nodeId - 1) % count];
}

uint32 ClusterMgr::GetMigrateVisualKit() const
{
    return 224; // distinct burst played when a migration fires
}

void ClusterMgr::TagChatMessage(std::string& msg) const
{
    if (!m_enabled || !m_chatTag || msg.empty())
        return;
    // Prefix with the originating node id; tagged at the source so it survives a
    // cross-node relay (the receiving node delivers the already-tagged text).
    msg = "[N" + std::to_string(m_nodeId) + "] " + msg;
}

void ClusterMgr::Init()
{
    LoadConfig();

    if (!m_enabled)
    {
        sLog.outString("Cluster: disabled (Cluster.Enable = 0).");
        return;
    }

    sLog.outString("Cluster: node %u coming online (%s:%u, peer-port %u, capacity %u, heartbeat %us).",
                   m_nodeId, m_host.c_str(), m_port, m_peerPort, m_capacity, m_heartbeatSec);
    RegisterNode();

    // Start the dedicated inter-node network thread (own reactor, off the main one).
    m_net = new ClusterThread((uint16)m_peerPort, m_host.c_str());
    if (m_net->open(NULL) == -1)
    {
        sLog.outError("Cluster: inter-node network thread failed to start on port %u", m_peerPort);
        delete m_net;
        m_net = NULL;
    }
}

void ClusterMgr::RegisterNode()
{
    std::string safeHost = m_host;
    LoginDatabase.escape_string(safeHost);

    // Upsert: claim/refresh our row and mark ourselves online. Other nodes' rows
    // are untouched.
    LoginDatabase.PExecute(
        "INSERT INTO `cluster_nodes` "
        "(`node_id`,`host`,`port`,`peer_port`,`capacity`,`player_count`,`last_heartbeat`,`status`) "
        "VALUES (%u,'%s',%u,%u,%u,0,UNIX_TIMESTAMP(),'online') "
        "ON DUPLICATE KEY UPDATE "
        "`host`=VALUES(`host`),`port`=VALUES(`port`),`peer_port`=VALUES(`peer_port`),"
        "`capacity`=VALUES(`capacity`),"
        "`player_count`=0,`last_heartbeat`=UNIX_TIMESTAMP(),`status`='online'",
        m_nodeId, safeHost.c_str(), m_port, m_peerPort, m_capacity);
}

void ClusterMgr::Heartbeat()
{
    uint32 players = sWorld.GetActiveSessionCount();
    LoginDatabase.PExecute(
        "UPDATE `cluster_nodes` SET "
        "`last_heartbeat`=UNIX_TIMESTAMP(),`status`='online',`player_count`=%u "
        "WHERE `node_id`=%u",
        players, m_nodeId);
}

void ClusterMgr::MarkStaleOffline()
{
    // A peer that has not heartbeat within 3x its interval is considered down.
    // (Phase 0 uses our own interval as the grace yardstick; nodes share config.)
    uint32 grace = m_heartbeatSec * 3;
    LoginDatabase.PExecute(
        "UPDATE `cluster_nodes` SET `status`='offline',`player_count`=0 "
        "WHERE `status`<>'offline' AND `last_heartbeat` < (UNIX_TIMESTAMP() - %u)",
        grace);

    // Coordinator-driven auto-failover. We recompute the coordinator AFTER the flip
    // above, so a just-died coordinator is already excluded and the next-lowest
    // survivor takes over this same tick. The sweep is idempotent, so running it
    // every tick on the coordinator is cheap (no-op when nothing is orphaned).
    if (m_autoFailover && IsCoordinator())
        RunFailoverSweep();
}

uint32 ClusterMgr::GetCoordinatorNode()
{
    // Deterministic, stateless election: the lowest node_id currently online owns
    // failover. Recomputed each call, so coordinator death heals on the next tick.
    QueryResult* result = LoginDatabase.Query(
        "SELECT MIN(`node_id`) FROM `cluster_nodes` WHERE `status`='online'");
    if (!result)
        return 0;
    uint32 coord = (*result)[0].GetUInt32();
    delete result;
    return coord;
}

void ClusterMgr::GetOnlineSurvivors(std::vector<uint32>& out)
{
    out.clear();
    QueryResult* result = LoginDatabase.Query(
        "SELECT `node_id` FROM `cluster_nodes` WHERE `status`='online' ORDER BY `node_id` ASC");
    if (result)
    {
        do { out.push_back((*result)[0].GetUInt32()); } while (result->NextRow());
        delete result;
    }
}

void ClusterMgr::RunFailoverSweep()
{
    // Coordinator-only, gated. Reassign EVERY zone/affinity owned by a node that is
    // not currently online to a survivor — independent of WHEN it went down, so a
    // coordinator that itself dies mid-heal is finished by the next coordinator's
    // sweep (genuinely self-healing). Idempotent: with nothing orphaned this is a
    // couple of cheap SELECTs that return no rows.
    if (!m_enabled || !m_autoFailover)
        return;

    std::vector<uint32> survivors;
    GetOnlineSurvivors(survivors);
    if (survivors.empty())
        return; // can't happen on the world thread (we are online), but be safe

    // Comma-separated survivor id list for the NOT IN guards (node count is tiny,
    // <=255). "owned by a non-survivor" == owned by an offline/unknown node.
    std::string inList;
    for (size_t i = 0; i < survivors.size(); ++i)
        inList += (i ? "," : "") + std::to_string(survivors[i]);

    // 1) Zones owned by a dead node -> spread round-robin across survivors so load
    //    isn't piled onto one node. Each UPDATE is guarded by the old owner id, so
    //    it is a compare-and-set and a repeat sweep is a no-op.
    QueryResult* zres = LoginDatabase.PQuery(
        "SELECT `zone_id`,`node_id` FROM `cluster_zone_assignment` "
        "WHERE `node_id`<>0 AND `node_id` NOT IN (%s)", inList.c_str());
    uint32 zonesMoved = 0, idx = 0;
    if (zres)
    {
        do
        {
            uint32 zoneId  = (*zres)[0].GetUInt32();
            uint32 oldNode = (*zres)[1].GetUInt32();
            uint32 target  = survivors[idx % survivors.size()];
            ++idx;
            LoginDatabase.PExecute(
                "UPDATE `cluster_zone_assignment` SET `node_id`=%u "
                "WHERE `zone_id`=%u AND `node_id`=%u", target, zoneId, oldNode);
            ++zonesMoved;
        } while (zres->NextRow());
        delete zres;
    }

    // 2) Character affinities pinned to a dead node -> 0 (= any node), so the login
    //    affinity check stops refusing them and the router lands them on a survivor.
    //    Clear (not re-pin) so we never pin onto a node that may also be down/full.
    //    cluster_character_node is in the CHARACTER db and cluster_nodes in LOGIN, so
    //    we cannot JOIN — guard by the same survivor id list instead. Idempotent.
    CharacterDatabase.PExecute(
        "UPDATE `cluster_character_node` SET `node_id`=0 "
        "WHERE `node_id`<>0 AND `node_id` NOT IN (%s)", inList.c_str());

    if (zonesMoved)
    {
        LoadZoneMap(); // route newly-claimed zones immediately on this (coordinator) node
        sLog.outString("Cluster: auto-failover swept %u orphaned zone(s) onto %u survivor(s) "
                       "and cleared affinities of players pinned to a downed node.",
                       zonesMoved, (uint32)survivors.size());
        sWorld.SendServerMessage(SERVER_MSG_CUSTOM,
            "Cluster: a node went down; surviving nodes auto-healed zone/character assignments.");
    }
}

void ClusterMgr::RegisterPlayer(uint32 guidLow)
{
    if (!m_enabled)
        return;
    std::lock_guard<std::mutex> guard(m_playerLock);
    m_playerNodes[guidLow] = m_nodeId;
}

void ClusterMgr::UnregisterPlayer(uint32 guidLow)
{
    if (!m_enabled)
        return;
    std::lock_guard<std::mutex> guard(m_playerLock);
    m_playerNodes.erase(guidLow);
}

uint32 ClusterMgr::GetNodeForPlayer(uint32 guidLow) const
{
    if (!m_enabled)
        return 0;
    std::lock_guard<std::mutex> guard(m_playerLock);
    std::map<uint32, uint32>::const_iterator it = m_playerNodes.find(guidLow);
    return it != m_playerNodes.end() ? it->second : 0;
}

uint32 ClusterMgr::GetLocalPlayerCount() const
{
    std::lock_guard<std::mutex> guard(m_playerLock);
    return (uint32)m_playerNodes.size();
}

uint32 ClusterMgr::GetOptimalNode()
{
    // Least-loaded online node; falls back to this node if the registry is empty
    // or unreadable. Used later by realmd-style login routing.
    if (!m_enabled)
        return m_nodeId;

    QueryResult* result = LoginDatabase.PQuery(
        "SELECT `node_id` FROM `cluster_nodes` WHERE `status`='online' "
        "ORDER BY `player_count` ASC LIMIT 1");
    if (!result)
        return m_nodeId;

    uint32 nodeId = (*result)[0].GetUInt32();
    delete result;
    return nodeId ? nodeId : m_nodeId;
}

void ClusterMgr::RelayMovement(Player* mover, uint16 opcode, MovementInfo const& mi)
{
    if (!m_enabled || !mover)
        return;

    // Serialize {guid, opcode, MovementInfo} and enqueue for broadcast. Enqueue is
    // mutex-guarded and does no socket I/O, so this is safe on map threads.
    ByteBuffer payload;
    payload << (uint64)mover->GetObjectGuid().GetRawValue();
    payload << (uint16)opcode;
    mi.Write(payload);

    ByteBuffer frame;
    ClusterFrame::Build(frame, CLUSTER_MSG_RELAY_MOVEMENT, payload);
    EnqueueBroadcast(frame);
}

// ---- transport bridge: called from the ClusterThread (network thread) ----

bool ClusterMgr::PopOutbound(std::vector<ClusterOutFrame>& out)
{
    std::lock_guard<std::mutex> guard(m_outLock);
    if (m_outQueue.empty())
        return false;
    out.assign(m_outQueue.begin(), m_outQueue.end());
    m_outQueue.clear();
    return true;
}

void ClusterMgr::GetPeers(std::vector<ClusterPeer>& out)
{
    std::lock_guard<std::mutex> guard(m_peerLock);
    out = m_peers;
}

void ClusterMgr::PushInbound(uint8 type, const uint8* data, uint32 len)
{
    std::vector<uint8> frame;
    frame.reserve(len + 1);
    frame.push_back(type);
    if (len && data)
        frame.insert(frame.end(), data, data + len);

    std::lock_guard<std::mutex> guard(m_inLock);
    m_inQueue.push_back(frame);
}

void ClusterMgr::OnPeerHeartbeat(uint32 nodeId)
{
    if (!nodeId)
        return;
    std::lock_guard<std::mutex> guard(m_peerLock);
    m_peerLastSeen[nodeId] = getMSTime();
}

// ---- world-thread helpers ----

void ClusterMgr::EnqueueBroadcast(ByteBuffer const& frame)
{
    ClusterOutFrame f;
    f.target = 0; // broadcast
    if (frame.size())
        f.bytes.assign(frame.contents(), frame.contents() + frame.size());

    std::lock_guard<std::mutex> guard(m_outLock);
    m_outQueue.push_back(f);
}

void ClusterMgr::EnqueueDirected(uint32 target, ByteBuffer const& frame)
{
    ClusterOutFrame f;
    f.target = target; // single peer
    if (frame.size())
        f.bytes.assign(frame.contents(), frame.contents() + frame.size());

    std::lock_guard<std::mutex> guard(m_outLock);
    m_outQueue.push_back(f);
}

void ClusterMgr::SendPlayerTransfer(uint32 targetNode, uint32 guidLow, ByteBuffer const& blob)
{
    if (!m_enabled || !targetNode || targetNode == m_nodeId)
        return;

    ByteBuffer payload;
    payload << (uint32)guidLow;
    if (blob.size())
        payload.append(blob.contents(), blob.size());

    ByteBuffer frame;
    ClusterFrame::Build(frame, CLUSTER_MSG_PLAYER_TRANSFER, payload);
    EnqueueDirected(targetNode, frame);
}

void ClusterMgr::SendChatRelay(uint8 chatType, uint32 lang, uint64 fromGuid, uint8 fromTag,
                               std::string const& fromName, std::string const& toName,
                               std::string const& text, uint32 destId, uint32 team)
{
    if (!m_enabled)
        return;

    ByteBuffer payload;
    payload << uint8(chatType);
    payload << uint32(lang);
    payload << uint64(fromGuid);
    payload << uint8(fromTag);
    payload << fromName;
    payload << toName;
    payload << text;
    payload << uint32(destId); // guild id for guild/officer chat (0 otherwise)
    payload << uint32(team);   // sender Team for channel lookup (0 otherwise)

    ByteBuffer frame;
    ClusterFrame::Build(frame, CLUSTER_MSG_RELAY_CHAT, payload);
    EnqueueBroadcast(frame); // peers hosting the recipients deliver it
}

void ClusterMgr::SendGroupChatRelay(uint8 chatType, uint32 lang, uint32 groupId,
                                    uint64 fromGuid, uint8 fromTag, std::string const& fromName,
                                    std::string const& text, int32 subGroup)
{
    if (!m_enabled || !groupId)
        return;

    ByteBuffer payload;
    payload << uint8(chatType);
    payload << uint32(lang);
    payload << uint32(groupId);
    payload << uint64(fromGuid);
    payload << uint8(fromTag);
    payload << fromName;
    payload << text;
    payload << int32(subGroup);

    ByteBuffer frame;
    ClusterFrame::Build(frame, CLUSTER_MSG_RELAY_GROUP_CHAT, payload);
    EnqueueBroadcast(frame); // peers hosting this group's members deliver it
}

void ClusterMgr::SendGroupStateChange(uint32 groupId, uint8 reason)
{
    if (!m_enabled || !groupId)
        return;

    ByteBuffer payload;
    payload << uint32(groupId);
    payload << uint8(reason);

    ByteBuffer frame;
    ClusterFrame::Build(frame, CLUSTER_MSG_GROUP_STATE, payload);
    EnqueueBroadcast(frame); // peers re-read the shared DB for this group
}

// ---- Phase 8: service-role routing + graceful fallback ----

void ClusterMgr::DoLocalAnnounce(std::string const& text) const
{
    // The authoritative effect of the ANNOUNCE role: deliver to THIS node's
    // players. Used by the owner (after a routed request) and by every fallback.
    if (text.empty())
        return;
    sWorld.SendServerMessage(SERVER_MSG_CUSTOM, text.c_str());
}

bool ClusterMgr::RouteAnnounce(std::string const& text)
{
    // Decide local-vs-remote for the ANNOUNCE role. Returns true only when the op
    // was handed to a remote owner; in every other case it has ALREADY performed
    // the local fallback and returns false.
    uint32 owner = GetServiceOwner(CLUSTER_SERVICE_ANNOUNCE);

    // Fallback set: cluster disabled, role unconfigured (0), we ARE the owner, or
    // the owner is offline -> handle locally. This is the graceful-degradation core.
    if (!m_enabled || owner == 0 || owner == m_nodeId || !IsServiceOwnerOnline(owner))
    {
        DoLocalAnnounce(text);
        // When we are the configured owner and the cluster is up, also fan the
        // result out so peers deliver it cluster-wide (single authoritative source).
        if (m_enabled && owner == m_nodeId)
        {
            ByteBuffer payload;
            payload << uint8(CLUSTER_SERVICE_ANNOUNCE);
            payload << text;
            ByteBuffer frame;
            ClusterFrame::Build(frame, CLUSTER_MSG_SERVICE_RESULT, payload);
            EnqueueBroadcast(frame);
        }
        return false; // handled locally
    }

    // Owner is a live remote node: route the request to it (directed). The owner
    // performs the op and broadcasts the SERVICE_RESULT so all nodes deliver it.
    ByteBuffer payload;
    payload << uint8(CLUSTER_SERVICE_ANNOUNCE);
    payload << uint32(m_nodeId); // requester (for the owner's log/audit)
    payload << text;
    ByteBuffer frame;
    ClusterFrame::Build(frame, CLUSTER_MSG_SERVICE_REQUEST, payload);
    EnqueueDirected(owner, frame);
    return true; // routed to the role owner
}

void ClusterMgr::ProcessNetwork()
{
    if (!m_enabled)
        return;
    DrainInbound();
}

void ClusterMgr::RefreshPeers()
{
    std::vector<ClusterPeer> peers;
    QueryResult* result = LoginDatabase.PQuery(
        "SELECT `node_id`,`host`,`peer_port` FROM `cluster_nodes` "
        "WHERE `status`='online' AND `node_id`<>%u", m_nodeId);
    if (result)
    {
        do
        {
            Field* f = result->Fetch();
            ClusterPeer p;
            p.nodeId = f[0].GetUInt32();
            p.host   = f[1].GetCppString();
            p.port   = f[2].GetUInt32();
            if (p.port)
                peers.push_back(p);
        } while (result->NextRow());
        delete result;
    }

    std::lock_guard<std::mutex> guard(m_peerLock);
    m_peers.swap(peers);
}

void ClusterMgr::DrainInbound()
{
    std::deque<std::vector<uint8> > local;
    {
        std::lock_guard<std::mutex> guard(m_inLock);
        if (m_inQueue.empty())
            return;
        local.swap(m_inQueue);
    }

    for (size_t i = 0; i < local.size(); ++i)
    {
        std::vector<uint8>& frame = local[i];
        if (frame.empty())
            continue;
        uint8 type = frame[0];

        switch (type)
        {
            case CLUSTER_MSG_RELAY_MOVEMENT:
                // Phase 2: frame received. Re-broadcasting to nearby local clients
                // happens once cross-node players exist (Phase 4+); consume for now.
                break;
            case CLUSTER_MSG_PLAYER_TRANSFER:
            {
                // Phase 4: an incoming migration hand-off. Payload = uint32 guidLow +
                // serialized player blob. Validate integrity here; the player's state
                // is already in the shared DB (source saved before transfer), so the
                // character loads from DB when the client reconnects to this node.
                if (frame.size() >= 1 + 4)
                {
                    ByteBuffer buf;
                    buf.append(&frame[1], frame.size() - 1);
                    uint32 guidLow = 0;
                    buf >> guidLow;
                    ByteBuffer blob;
                    size_t rem = buf.size() - buf.rpos();
                    if (rem)
                        blob.append(buf.contents() + buf.rpos(), rem);
                    uint32 blobGuid = 0;
                    bool ok = Player::ValidateMigrationBlob(blob, blobGuid);
                    sLog.outString("Cluster: incoming player migration guid %u -> node %u: %s (blob guid %u)",
                                   guidLow, m_nodeId, ok ? "validated" : "INVALID", blobGuid);
                }
                break;
            }
            case CLUSTER_MSG_RELAY_CHAT:
            {
                // Phase 6: a chat message relayed from another node — deliver it to the
                // recipients that live on THIS node. Whisper -> single target by name;
                // guild/officer -> the local guild's online members; channel -> the
                // local same-named channel's members. The origin node already delivered
                // locally and does not process its own broadcast, so no double-delivery.
                if (frame.size() > 1)
                {
                    ByteBuffer buf;
                    buf.append(&frame[1], frame.size() - 1);
                    uint8 chatType = 0, fromTag = 0;
                    uint32 lang = 0, destId = 0, team = 0;
                    uint64 fromGuid = 0;
                    std::string fromName, toName, text;
                    buf >> chatType >> lang >> fromGuid >> fromTag >> fromName >> toName >> text;
                    // destId/team appended in newer frames; tolerate older 7-field frames.
                    if (buf.rpos() < buf.size())
                        buf >> destId;
                    if (buf.rpos() < buf.size())
                        buf >> team;

                    switch (chatType)
                    {
                        case CHAT_MSG_GUILD:
                        case CHAT_MSG_OFFICER:
                        {
                            if (Guild* guild = sGuildMgr.GetGuildById(destId))
                                guild->DeliverRelayedChat(chatType, lang, ObjectGuid(fromGuid),
                                    fromTag, fromName, text, chatType == CHAT_MSG_OFFICER);
                            break;
                        }
                        case CHAT_MSG_CHANNEL:
                        {
                            if (ChannelMgr* cMgr = channelMgr(Team(team)))
                                if (Channel* chn = cMgr->GetChannel(toName, NULL, false))
                                    chn->DeliverRelayedChat(lang, ObjectGuid(fromGuid),
                                        fromTag, fromName, text);
                            break;
                        }
                        default: // CHAT_MSG_WHISPER and any other single-target relay
                        {
                            Player* tgt = sObjectAccessor.FindPlayerByName(toName.c_str());
                            if (tgt && tgt->GetSession())
                            {
                                WorldPacket data;
                                ChatHandler::BuildChatPacket(data, ChatMsg(chatType), text.c_str(),
                                    Language(lang), ChatTagFlags(fromTag), ObjectGuid(fromGuid), fromName.c_str());
                                tgt->GetSession()->SendPacket(&data);
                            }
                            break;
                        }
                    }
                }
                break;
            }
            case CLUSTER_MSG_RELAY_GROUP_CHAT:
            {
                // Phase 7: a party/raid chat relayed from another node. Deliver to the
                // members of this group that live on THIS node. The origin already
                // delivered locally and does not process its own broadcast, so there is
                // no double-delivery.
                if (frame.size() > 1)
                {
                    ByteBuffer buf;
                    buf.append(&frame[1], frame.size() - 1);
                    uint8 chatType = 0, fromTag = 0;
                    uint32 lang = 0, groupId = 0;
                    uint64 fromGuid = 0;
                    int32 subGroup = -1;
                    std::string fromName, text;
                    buf >> chatType >> lang >> groupId >> fromGuid >> fromTag
                        >> fromName >> text >> subGroup;

                    if (Group* group = sObjectMgr.GetGroupById(groupId))
                        group->DeliverRelayedChat(chatType, lang, ObjectGuid(fromGuid),
                            fromTag, fromName, text, subGroup);
                }
                break;
            }
            case CLUSTER_MSG_GROUP_STATE:
            {
                // Phase 7: a group's roster/leader/state changed on another node. The
                // authoritative roster lives in the shared DB; here we just refresh the
                // local view and re-push the group frame to our local members.
                if (frame.size() >= 1 + 4 + 1)
                {
                    ByteBuffer buf;
                    buf.append(&frame[1], frame.size() - 1);
                    uint32 groupId = 0;
                    uint8 reason = 0;
                    buf >> groupId >> reason;

                    if (Group* group = sObjectMgr.GetGroupById(groupId))
                        group->OnRelayedStateChange(reason);
                }
                break;
            }
            case CLUSTER_MSG_SERVICE_REQUEST:
            {
                // Phase 8: we are the role owner and a peer routed an op to us. Run it
                // authoritatively here (world thread), then broadcast the result so
                // every node — including the requester — delivers it.
                if (frame.size() > 1)
                {
                    ByteBuffer buf;
                    buf.append(&frame[1], frame.size() - 1);
                    uint8 role = 0;
                    uint32 fromNode = 0;
                    buf >> role;
                    if (role == CLUSTER_SERVICE_ANNOUNCE)
                    {
                        std::string text;
                        buf >> fromNode >> text;

                        // Honour the request locally even if config drifted and we are
                        // no longer the owner (never drop a routed op).
                        DoLocalAnnounce(text);

                        ByteBuffer payload;
                        payload << uint8(CLUSTER_SERVICE_ANNOUNCE);
                        payload << text;
                        ByteBuffer out;
                        ClusterFrame::Build(out, CLUSTER_MSG_SERVICE_RESULT, payload);
                        EnqueueBroadcast(out);

                        sLog.outString("Cluster: ANNOUNCE service ran for node %u: \"%s\"",
                                       fromNode, text.c_str());
                    }
                }
                break;
            }
            case CLUSTER_MSG_SERVICE_RESULT:
            {
                // Phase 8: the role owner finished an op and broadcast the result.
                // Apply it to this node's players. The owner already delivered to its
                // own players when it ran the op and does NOT process its own broadcast
                // (origin skips self), so there is no double-delivery.
                if (frame.size() > 1)
                {
                    ByteBuffer buf;
                    buf.append(&frame[1], frame.size() - 1);
                    uint8 role = 0;
                    buf >> role;
                    if (role == CLUSTER_SERVICE_ANNOUNCE)
                    {
                        std::string text;
                        buf >> text;
                        DoLocalAnnounce(text);
                    }
                }
                break;
            }
            case CLUSTER_MSG_PLAYER_ENTER:
            case CLUSTER_MSG_PLAYER_LEAVE:
            case CLUSTER_MSG_SOCIAL_STATUS:
            default:
                break;
        }
    }
}

void ClusterMgr::Update(uint32 /*diff*/)
{
    if (!m_enabled)
        return;

    Heartbeat();
    MarkStaleOffline();
    RefreshPeers();
    // (inbound is drained every world tick via ProcessNetwork, not here)

    // Emit a heartbeat frame to peers so they register us as live on the wire.
    ByteBuffer payload;
    payload << (uint32)m_nodeId;
    ByteBuffer frame;
    ClusterFrame::Build(frame, CLUSTER_MSG_HEARTBEAT, payload);
    EnqueueBroadcast(frame);
}

void ClusterMgr::Shutdown()
{
    if (!m_enabled)
        return;

    // Stop the network thread first so no sends race the offline write.
    if (m_net)
    {
        m_net->Stop();
        m_net->wait();
        delete m_net;
        m_net = NULL;
    }

    // Synchronous: this runs during shutdown, so the write must commit before the
    // DB delay threads are halted (an async PExecute here would be dropped).
    LoginDatabase.DirectPExecute(
        "UPDATE `cluster_nodes` SET `status`='offline',`player_count`=0 WHERE `node_id`=%u",
        m_nodeId);
    sLog.outString("Cluster: node %u marked offline.", m_nodeId);
}
