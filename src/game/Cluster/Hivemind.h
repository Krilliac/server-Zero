#pragma once
#include "WorldNode.h"
#include <unordered_map>
#include <shared_mutex>
#include <vector>

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

    bool MigratePlayer(Player* player, uint32 targetNodeId);
    void FinalizeMigration(Player* player, uint32 sourceNodeId);

    uint32 GetOptimalNodeFor(Player* player) const;
    uint32 GetPlayerCount(uint32 nodeId) const;
    std::vector<uint32> GetNodeIds() const;
    uint32 GetMigrationCount(uint32 nodeId) const;
    bool IsNodeOnline(uint32 nodeId) const;

private:
    struct NodeData {
        WorldNode* node;
        uint32 playerCount;
        uint32 latency; // ms to master
        uint32 migrations;
        bool online;
    };

    void TransferPlayerState(Player* player, WorldNode* targetNode);

    std::unordered_map<uint32, NodeData> m_nodes;
    std::unordered_map<ObjectGuid, uint32> m_playerNodeMap;
    uint32 m_masterNodeId = 1;

    // Config defaults (overridable)
    uint32 m_migrationTimeout = 140; // ms
    uint32 m_maxPlayersPerNode = 5000;
    uint32 m_rebalanceInterval = 30000; // ms

    mutable std::shared_mutex m_lock;
};

#define sHivemind Hivemind::instance()
