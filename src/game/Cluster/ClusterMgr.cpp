/*
 * Multi-node Cluster framework — central node manager (Phase 0).
 */

#include "ClusterMgr.h"
#include "World.h"
#include "Log.h"
#include "Config/Config.h"
#include "Database/DatabaseEnv.h"

ClusterMgr::ClusterMgr()
    : m_enabled(false), m_nodeId(1), m_port(0), m_capacity(0),
      m_heartbeatSec(30), m_host("127.0.0.1")
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

    sLog.outString("Cluster: node %u coming online (%s:%u, capacity %u, heartbeat %us).",
                   m_nodeId, m_host.c_str(), m_port, m_capacity, m_heartbeatSec);
    RegisterNode();
}

void ClusterMgr::RegisterNode()
{
    std::string safeHost = m_host;
    LoginDatabase.escape_string(safeHost);

    // Upsert: claim/refresh our row and mark ourselves online. Other nodes' rows
    // are untouched.
    LoginDatabase.PExecute(
        "INSERT INTO `cluster_nodes` "
        "(`node_id`,`host`,`port`,`capacity`,`player_count`,`last_heartbeat`,`status`) "
        "VALUES (%u,'%s',%u,%u,0,UNIX_TIMESTAMP(),'online') "
        "ON DUPLICATE KEY UPDATE "
        "`host`=VALUES(`host`),`port`=VALUES(`port`),`capacity`=VALUES(`capacity`),"
        "`player_count`=0,`last_heartbeat`=UNIX_TIMESTAMP(),`status`='online'",
        m_nodeId, safeHost.c_str(), m_port, m_capacity);
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

void ClusterMgr::RelayMovement(Player* /*mover*/, uint16 /*opcode*/, MovementInfo const& /*mi*/)
{
    if (!m_enabled)
        return;
    // Phase 1: no-op scaffold. Phase 2 serializes (guid + MovementInfo) and sends
    // to peer nodes over ClusterSocket so off-node observers see this mover.
}

void ClusterMgr::Update(uint32 /*diff*/)
{
    if (!m_enabled)
        return;

    Heartbeat();
    MarkStaleOffline();
}

void ClusterMgr::Shutdown()
{
    if (!m_enabled)
        return;

    // Synchronous: this runs during shutdown, so the write must commit before the
    // DB delay threads are halted (an async PExecute here would be dropped).
    LoginDatabase.DirectPExecute(
        "UPDATE `cluster_nodes` SET `status`='offline',`player_count`=0 WHERE `node_id`=%u",
        m_nodeId);
    sLog.outString("Cluster: node %u marked offline.", m_nodeId);
}
