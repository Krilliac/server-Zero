#pragma once
#include "WorldNode.h"
#include "ObjectGuid.h"
#include <unordered_map>
#include <shared_mutex>
#include <vector>
#include <limits>

class Player;
class WorldNode;

class Hivemind
{
public:
    static Hivemind* instance();

    void Initialize(uint32 masterNodeId);
    void Update(uint32 diff);

    void AddNode(WorldNode* node);
    void RemoveNode(uint32 nodeId);
    void UpdateNodeStatus(uint32 nodeId, bool online, uint32 latency);

    bool MigratePlayer(Player* player, uint32 targetNodeId);
    void AddPlayer(Player* player);
    void RemovePlayer(Player* player);
    uint32 RebalancePlayers();

    uint32 GetOptimalNodeFor(Player* player) const;
    uint32 GetPlayerCount(uint32 nodeId) const;
    std::vector<uint32> GetNodeIds() const;
    bool IsNodeOnline(uint32 nodeId) const;

private:
    struct NodeData {
        WorldNode* node = nullptr;
        uint32 playerCount = 0;
        uint32 latency = 0;  // ms to master
        bool online = false;
    };

    uint32 FindUnderloadedNode() const;

    std::unordered_map<uint32, NodeData> m_nodes;
    std::unordered_map<ObjectGuid, uint32> m_playerNodeMap;
    uint32 m_masterNodeId = 1;

    // Configurable values
    uint32 m_migrationTimeout = 5000;      // ms
    uint32 m_maxPlayersPerNode = 500;
    uint32 m_rebalanceInterval = 30000;    // ms
    uint32 m_rebalanceBatchSize = 5;

    mutable std::shared_mutex m_lock;
};

#define sHivemind Hivemind::instance()