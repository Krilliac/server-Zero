#include "Hivemind.h"
#include "WorldNode.h"
#include "Player.h"
#include "WorldSession.h"
#include "Log.h"
#include "Config.h"
#include "ClusterDefines.h"
#include "ObjectAccessor.h"

Hivemind* Hivemind::instance()
{
    static Hivemind instance;
    return &instance;
}

void Hivemind::Initialize(uint32 masterNodeId)
{
    m_masterNodeId = masterNodeId;
    m_migrationTimeout = sConfig->GetIntDefault("Cluster.MigrationTimeout", 140);
    m_maxPlayersPerNode = sConfig->GetIntDefault("Cluster.MaxPlayersPerNode", 5000);
    m_rebalanceInterval = sConfig->GetIntDefault("Cluster.RebalanceInterval", 30000);
}

void Hivemind::AddNode(WorldNode* node)
{
    std::unique_lock lock(m_lock);
    m_nodes[node->GetNodeId()] = { node, 0, 0, 0, true };
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
    std::unique_lock lock(m_lock);
    auto targetIt = m_nodes.find(targetNodeId);
    if (targetIt == m_nodes.end() || !targetIt->second.online) return false;
    if (targetIt->second.playerCount >= m_maxPlayersPerNode) return false;

    WorldNode* targetNode = targetIt->second.node;
    TransferPlayerState(player, targetNode);

    uint32 sourceNodeId = m_playerNodeMap[player->GetGUID()];
    m_playerNodeMap[player->GetGUID()] = targetNodeId;

    // Schedule migration finalization (simulate atomic transfer)
    player->AddDelayedEvent(m_migrationTimeout, [this, player, sourceNodeId]() {
        FinalizeMigration(player, sourceNodeId);
    });

    return true;
}

void Hivemind::TransferPlayerState(Player* player, WorldNode* targetNode)
{
    // Serialize player state (compressed)
    ByteBuffer stateBuffer;
    player->SerializeForTransfer(stateBuffer);

    // In production, compress with Snappy/Zstd
    // Send to target node (network code abstracted)
    targetNode->TransferPlayer(player->GetGUID(), stateBuffer);

    // Notify player to reconnect
    WorldPacket data(SMSG_TRANSFER_PENDING, 4);
    data << uint32(targetNode->GetAddress());
    player->GetSession()->SendPacket(&data);
}

void Hivemind::FinalizeMigration(Player* player, uint32 sourceNodeId)
{
    std::unique_lock lock(m_lock);

    // Remove from source node
    if (auto sourceIt = m_nodes.find(sourceNodeId); sourceIt != m_nodes.end()) {
        if (sourceIt->second.playerCount > 0)
            sourceIt->second.playerCount--;
    }

    // Add to target node
    uint32 targetNodeId = m_playerNodeMap[player->GetGUID()];
    if (auto targetIt = m_nodes.find(targetNodeId); targetIt != m_nodes.end()) {
        targetIt->second.playerCount++;
        targetIt->second.migrations++;
    }
    // Visual feedback, metrics, etc. can be added here
}

void Hivemind::Update(uint32 diff)
{
    static uint32 rebalanceTimer = 0;
    rebalanceTimer += diff;

    if (rebalanceTimer >= m_rebalanceInterval) {
        // Auto-rebalance logic can be implemented here
        rebalanceTimer = 0;
    }
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

uint32 Hivemind::GetMigrationCount(uint32 nodeId) const
{
    std::shared_lock lock(m_lock);
    auto it = m_nodes.find(nodeId);
    return it != m_nodes.end() ? it->second.migrations : 0;
}

bool Hivemind::IsNodeOnline(uint32 nodeId) const
{
    std::shared_lock lock(m_lock);
    auto it = m_nodes.find(nodeId);
    return it != m_nodes.end() && it->second.online;
}
