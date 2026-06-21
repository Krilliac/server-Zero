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
#include "Config/Config.h"
#include "Database/DatabaseEnv.h"

ClusterMgr::ClusterMgr()
    : m_enabled(false), m_nodeId(1), m_port(0), m_peerPort(0), m_capacity(0),
      m_heartbeatSec(30), m_host("127.0.0.1"), m_net(NULL)
{
}

void ClusterMgr::LoadConfig()
{
    m_enabled      = sWorld.getConfig(CONFIG_BOOL_CLUSTER_ENABLE);
    m_nodeId       = sWorld.getConfig(CONFIG_UINT32_CLUSTER_NODE_ID);
    m_port         = sWorld.getConfig(CONFIG_UINT32_CLUSTER_PORT);
    m_heartbeatSec = sWorld.getConfig(CONFIG_UINT32_CLUSTER_HEARTBEAT);
    if (m_heartbeatSec < 5)
        m_heartbeatSec = 5;
    m_peerPort     = sWorld.getConfig(CONFIG_UINT32_CLUSTER_PEER_PORT);
    m_capacity     = sWorld.GetPlayerAmountLimit();
    m_host         = sConfig.GetStringDefault("Cluster.Host", "127.0.0.1");
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
            case CLUSTER_MSG_PLAYER_ENTER:
            case CLUSTER_MSG_PLAYER_LEAVE:
            case CLUSTER_MSG_RELAY_CHAT:
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
    DrainInbound();

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
