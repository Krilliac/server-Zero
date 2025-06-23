#pragma once
#include "DynamicTick.h"
#include "Hivemind.h"
#include "WorldSession.h"
#include <unordered_map>
#include <vector>

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
    
    std::unordered_map<ObjectGuid, WorldSession*> m_sessions; // Session storage
    uint32_t m_lastWorldUpdate = 0;
    bool m_stopEvent = false;
    
    // Metrics
    uint32_t m_peakPlayers = 0;
    float m_avgPhysicsError = 0.0f;
};

#define sWorld World::instance()
