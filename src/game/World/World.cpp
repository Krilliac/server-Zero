#include "World.h"
#include "MapManager.h"
#include "Player.h"
#include "Transport.h"
#include "Log.h"
#include "Config.h"
#include "MetricTracker.h"
#include "AntiCheatMgr.h"

World* World::instance()
{
    static World instance;
    return &instance;
}

void World::Initialize()
{
    // Initialize core systems
    sHivemind->Initialize(MAIN_WORLD_NODE);
    sMapMgr->Initialize();
    sTransportMgr->Initialize();
    
    // Load configuration
    ReloadConfig();
    
    sLog.outString("World initialized");
}

void World::Update(uint32_t diff)
{
    DynamicTick::Update(diff);
    const uint32_t interval = DynamicTick::GetCurrentTickInterval();
    
    m_lastWorldUpdate += diff;
    while (m_lastWorldUpdate >= interval)
    {
        // Core world updates
        UpdateMaps(interval);
        UpdatePlayers(interval);
        UpdateTransports(interval);
        
        // Cluster updates
        sHivemind->Update(interval);
        sTimeSyncMgr->Update(interval);
        
        m_lastWorldUpdate -= interval;
        
        // Update physics metrics
        m_avgPhysicsError = 0.9f * m_avgPhysicsError + 0.1f * CalculateAvgPhysicsError();
        sMetricTracker.RecordPhysicsError(m_avgPhysicsError);
    }
}

void World::UpdatePlayers(uint32_t diff)
{
    for (auto& [guid, session] : m_sessions)
    {
        Player* player = session->GetPlayer();
        if (player)
        {
            // Update player state
            player->Update(diff);
            
            // Auto-migration check
            if (player->GetPosition().DistanceToSquared(player->m_lastNodeCheckPos) > 10000.0f) // 100 yards
            {
                uint32_t newNode = sHivemind->GetOptimalNodeFor(player);
                if (newNode != player->GetNodeId())
                {
                    sHivemind->MigratePlayer(player, newNode);
                }
                player->m_lastNodeCheckPos = player->GetPosition();
            }
        }
    }
}

void World::UpdateMaps(uint32_t diff)
{
    sMapMgr->Update(diff);
}

void World::UpdateTransports(uint32_t diff)
{
    sTransportMgr->Update(diff);
}

uint32_t World::GetActiveSessionCount() const
{
    return m_sessions.size();
}

uint32_t World::GetPlayerCount() const
{
    uint32_t count = 0;
    for (const auto& [guid, session] : m_sessions)
    {
        if (session->GetPlayer())
            count++;
    }
    return count;
}

void World::AddNode(WorldNode* node)
{
    sHivemind->AddNode(node);
}

void World::RemoveNode(uint32_t nodeId)
{
    sHivemind->RemoveNode(nodeId);
}

void World::ReloadConfig()
{
    // Reinitialize systems with new config
    sTimeSyncMgr->Initialize();
    sAntiCheatMgr->Initialize();
    DynamicTick::Initialize();
}

float World::CalculateAvgPhysicsError() const
{
    float totalError = 0.0f;
    uint32_t playerCount = 0;
    
    for (const auto& [guid, session] : m_sessions)
    {
        if (Player* player = session->GetPlayer())
        {
            totalError += player->GetPhysicsError();
            playerCount++;
        }
    }
    
    return playerCount > 0 ? totalError / playerCount : 0.0f;
}
