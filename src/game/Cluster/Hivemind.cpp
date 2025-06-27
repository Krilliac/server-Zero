#include "Hivemind.h"
#include "WorldNode.h"
#include "Player.h"
#include "WorldSession.h"
#include "Log.h"
#include "Config/Config.h"
#include "ClusterDefines.h"
#include "ObjectAccessor.h"
#include "Cluster/ClusterMgr.h"
#include <limits>

Hivemind* Hivemind::instance()
{
    static Hivemind instance;
    return &instance;
}

void Hivemind::Initialize(uint32 masterNodeId)
{
    m_masterNodeId = masterNodeId;
    m_migrationTimeout = sConfig.GetIntDefault("Cluster.MigrationTimeout", 5000);
    m_maxPlayersPerNode = sConfig.GetIntDefault("Cluster.MaxPlayersPerNode", 500);
    m_rebalanceInterval = sConfig.GetIntDefault("Cluster.RebalanceInterval", 30000);
    m_rebalanceBatchSize = sConfig.GetIntDefault("Cluster.RebalanceBatchSize", 5);
}

void Hivemind::AddNode(WorldNode* node)
{
    std::unique_lock lock(m_lock);
    m_nodes[node->GetNodeId()] = { node, 0, 0, true };
}

void Hivemind::RemoveNode(uint32 nodeId)
{
    std::unique_lock lock(m_lock);
    m_nodes.erase(nodeId);
}

uint32 Hivemind::GetOptimalNodeFor(Player* player) const
{
    std::shared_lock lock(m_lock);
    uint32 bestNode = m_masterNodeId;
    uint32 bestScore = std::numeric_limits<uint32>::max();

    for (const auto& [nodeId, data] : m_nodes) {
        if (!data.online || data.playerCount >= m_maxPlayersPerNode) continue;
        uint32 score = data.latency + (data.playerCount * 10);
        if (score < bestScore) {
            bestScore = score;
            bestNode = nodeId;
        }
    }
    return bestNode;
}

bool Hivemind::MigratePlayer(Player* player, uint32 targetNodeId)
{
    if (!player || !player->IsInWorld()) return false;

    // Update player-node mapping immediately
    {
        std::unique_lock lock(m_lock);
        m_playerNodeMap[player->GetGUID()] = targetNodeId;
    }

    // Initiate migration
    player->MigrateToNode(targetNodeId);
    return true;
}

void Hivemind::AddPlayer(Player* player)
{
    if (!player) return;

    std::unique_lock lock(m_lock);
    uint32 nodeId = player->GetNodeId();
    
    // Initialize player-node mapping
    m_playerNodeMap[player->GetGUID()] = nodeId;
    
    // Update node player count
    auto it = m_nodes.find(nodeId);
    if (it != m_nodes.end()) {
        it->second.playerCount++;
    }
}

void Hivemind::RemovePlayer(Player* player)
{
    if (!player) return;

    std::unique_lock lock(m_lock);
    ObjectGuid guid = player->GetGUID();
    
    // Update node player count
    auto nodeIt = m_nodes.find(player->GetNodeId());
    if (nodeIt != m_nodes.end() && nodeIt->second.playerCount > 0) {
        nodeIt->second.playerCount--;
    }
    
    // Remove player mapping
    m_playerNodeMap.erase(guid);
}

void Hivemind::Update(uint32 diff)
{
    static uint32 rebalanceTimer = 0;
    rebalanceTimer += diff;

    if (rebalanceTimer >= m_rebalanceInterval) {
        RebalancePlayers();
        rebalanceTimer = 0;
    }
}

uint32 Hivemind::RebalancePlayers()
{
    uint32 migrated = 0;
    uint32 targetNode = 0;
    std::vector<ObjectGuid> playersToMigrate;

    // Phase 1: Identify migration candidates while locked
    {
        std::shared_lock lock(m_lock);
        
        // Find the most underloaded node
        uint32 minLoad = std::numeric_limits<uint32>::max();
        for (const auto& [nodeId, data] : m_nodes) {
            if (!data.online) continue;
            if (data.playerCount < minLoad) {
                minLoad = data.playerCount;
                targetNode = nodeId;
            }
        }
        
        // No valid target node found
        if (targetNode == 0) return 0;
        
        // Identify overloaded nodes
        for (const auto& [nodeId, nodeData] : m_nodes) {
            if (!nodeData.online || nodeData.playerCount <= m_maxPlayersPerNode * 0.8) 
                continue;
                
            // Collect players from this node
            uint32 collected = 0;
            for (const auto& [guid, assignedNode] : m_playerNodeMap) {
                if (assignedNode == nodeId) {
                    playersToMigrate.push_back(guid);
                    if (++collected >= m_rebalanceBatchSize) break;
                }
            }
        }
    }

    // Phase 2: Migrate players without lock
    for (const ObjectGuid& guid : playersToMigrate) {
        if (Player* player = ObjectAccessor::FindPlayer(guid)) {
            // Skip players in invalid states
            if (player->IsInCombat() || player->IsInFlight() || player->IsDead()) 
                continue;
                
            if (MigratePlayer(player, targetNode)) {
                migrated++;
            }
        }
    }
    
    return migrated;
}

uint32 Hivemind::GetPlayerCount(uint32 nodeId) const
{
    std::shared_lock lock(m_lock);
    auto it = m_nodes.find(nodeId);
    return it != m_nodes.end() ? it->second.playerCount : 0;
}

std::vector<uint32> Hivemind::GetNodeIds() const
{
    std::shared_lock lock(m_lock);
    std::vector<uint32> ids;
    for (const auto& [id, _] : m_nodes)
        ids.push_back(id);
    return ids;
}

bool Hivemind::IsNodeOnline(uint32 nodeId) const
{
    std::shared_lock lock(m_lock);
    auto it = m_nodes.find(nodeId);
    return it != m_nodes.end() && it->second.online;
}

void Hivemind::UpdateNodeStatus(uint32 nodeId, bool online, uint32 latency)
{
    std::unique_lock lock(m_lock);
    auto it = m_nodes.find(nodeId);
    if (it != m_nodes.end()) {
        it->second.online = online;
        it->second.latency = latency;
    }
}