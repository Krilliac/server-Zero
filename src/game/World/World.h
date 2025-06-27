#pragma once
#include "DynamicTick.h"
#include "Hivemind.h"
#include "WorldSession.h"
#include <unordered_map>
#include <vector>
#include <set>
#include <string>
#include <mutex>

// Forward declarations
class WorldNode;
class ClusterMgr;

class World
{
public:
    static World* instance();
    
    void Initialize();
    void Update(uint32_t diff);
    void Stop();
    
    uint32_t GetActiveSessionCount() const;
    uint32_t GetPlayerCount() const;
    
    // Cluster management
    void AddNode(WorldNode* node);
    void RemoveNode(uint32_t nodeId);
    
    // Configuration
    void ReloadConfig();
    
private:
    void UpdatePlayers(uint32_t diff);
    void UpdateMaps(uint32_t diff);
    void UpdateTransports(uint32_t diff);
    void CheckClusterState();
    void HandleNodeFailure(const std::string& nodeId);
    void HandleNodeRecovery(const std::string& nodeId);
    float CalculateAvgPhysicsError() const;
    
    std::unordered_map<ObjectGuid, WorldSession*> m_sessions; // Session storage
    uint32_t m_lastWorldUpdate = 0;
    bool m_stopEvent = false;
    
    // Cluster state tracking
    std::set<std::string> m_downNodes;          // Track failed nodes
    uint32_t m_clusterCheckTimer = 0;           // Timer for cluster checks
    std::mutex m_clusterMutex;                  // Thread safety for cluster operations
    
    // Metrics
    uint32_t m_peakPlayers = 0;
    float m_avgPhysicsError = 0.0f;
};

#define sWorld World::instance()