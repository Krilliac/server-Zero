#include "DebugVisualizer.h"
#include "GameObject.h"
#include "World.h"
#include "Log.h"

// Core visualizations
void DebugVisualizer::ShowPath(Player* player, const std::vector<Position>& path)
{
    for (const auto& pos : path)
    {
        GameObject* marker = player->SummonGameObject(50010, pos.x, pos.y, pos.z + 0.5f, 0, 0, 0, 0, 0, 30);
        if (marker) {
            marker->SetName("Path Node");
            marker->SetVisibility(VISIBILITY_GM);
        }
    }
}

void DebugVisualizer::ShowAIState(Player* player, const std::string& aiState)
{
    GameObject* marker = player->SummonGameObject(50011, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ() + 2.0f, 0, 0, 0, 0, 0, 10);
    if (marker) {
        marker->SetName("AI: " + aiState);
        marker->SetVisibility(VISIBILITY_GM);
    }
}

void DebugVisualizer::ShowSyncStatus(Player* player, float drift, float latency)
{
    GameObject* marker = player->SummonGameObject(50012, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ() + 3.0f, 0, 0, 0, 0, 0, 10);
    if (marker) {
        marker->SetName(fmt::format("Drift: {:.1f}ms Latency: {:.1f}ms", drift, latency));
        marker->SetVisibility(VISIBILITY_GM);
    }
}

// Cluster visualization
void DebugVisualizer::ShowClusterNode(Player* player, uint32 nodeId, uint32 playerCount)
{
    GameObject* marker = player->SummonGameObject(50014, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ() + 5.0f, 0, 0, 0, 0, 0, 15);
    if (marker) {
        marker->SetName(fmt::format("Node {}: {} players", nodeId, playerCount));
        marker->SetVisibility(VISIBILITY_GM);
    }
}

// Network visualization
void DebugVisualizer::ShowNetworkRoute(Player* player, const std::vector<uint32>& hops)
{
    float z = player->GetPositionZ() + 6.0f;
    for (auto hop : hops)
    {
        GameObject* marker = player->SummonGameObject(50015, player->GetPositionX(), player->GetPositionY(), z, 0, 0, 0, 0, 0, 10);
        if (marker) {
            marker->SetName(fmt::format("Hop: {}", hop));
            marker->SetVisibility(VISIBILITY_GM);
        }
        z += 1.0f;
    }
}
