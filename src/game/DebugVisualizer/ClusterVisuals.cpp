#include "ClusterVisuals.h"
#include "GameObject.h"
#include "Hivemind.h"
#include "World.h"

void ClusterVisuals::ShowMigration(Player* player, uint32 fromNode, uint32 toNode)
{
    GameObject* beam = player->SummonGameObject(
        MIGRATION_BEAM_ID,
        player->GetPositionX(),
        player->GetPositionY(),
        player->GetPositionZ() + 10.0f,
        0, 0, 0, 0, 0,
        15
    );
    if (beam) {
        beam->SetName(fmt::format("Migration: {} → {}", fromNode, toNode));
        beam->SetVisibility(VISIBILITY_GM);
    }
}

void ClusterVisuals::ShowNodeStatus(Player* gm)
{
    // Example: show all nodes as markers
    // (Implementation would fetch node positions from config)
    uint32 nodeCount = sHivemind->GetNodeIds().size();
    for (uint32 i = 0; i < nodeCount; ++i) {
        Position pos = gm->GetPosition();
        pos.x += i * 5.0f; // Spread out markers
        
        GameObject* marker = gm->SummonGameObject(
            NODE_MARKER_ID,
            pos.x, pos.y, pos.z + 10.0f,
            0, 0, 0, 0, 0,
            30
        );
        if (marker) {
            marker->SetName(fmt::format("Node {}", i));
            marker->SetVisibility(VISIBILITY_GM);
        }
    }
}

void ClusterVisuals::ShowNetworkPath(Player* player)
{
    // Example: visualize network hops
    // (Implementation would fetch actual path from network system)
    std::vector<uint32> hops = {1, 3, 5};
    DebugVisualizer::ShowNetworkRoute(player, hops);
}
