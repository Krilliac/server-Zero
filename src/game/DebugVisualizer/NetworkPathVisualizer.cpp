#include "NetworkPathVisualizer.h"
#include "GameObject.h"

void NetworkPathVisualizer::ShowPacketRoute(Player* player, const std::vector<Position>& path)
{
    for (size_t i = 0; i < path.size(); ++i) {
        const Position& pos = path[i];
        GameObject* marker = player->SummonGameObject(
            50070,
            pos.x, pos.y, pos.z + 1.0f,
            0, 0, 0, 0, 0,
            30
        );
        if (marker) {
            marker->SetName(fmt::format("Hop {}", i));
            marker->SetVisibility(VISIBILITY_GM);
        }
        
        // Draw line to next point
        if (i < path.size() - 1) {
            const Position& next = path[i+1];
            Position mid = {
                (pos.x + next.x) / 2,
                (pos.y + next.y) / 2,
                (pos.z + next.z) / 2 + 2.0f,
                0
            };
            
            GameObject* line = player->SummonGameObject(
                50071,
                mid.x, mid.y, mid.z,
                0, 0, 0, 0, 0,
                30
            );
            if (line) {
                line->SetName("Network Path");
                line->SetVisibility(VISIBILITY_GM);
            }
        }
    }
}

void NetworkPathVisualizer::ShowLatency(Player* player, uint32 latency)
{
    GameObject* marker = player->SummonGameObject(
        50072,
        player->GetPositionX(),
        player->GetPositionY(),
        player->GetPositionZ() + 3.0f,
        0, 0, 0, 0, 0,
        15
    );
    if (marker) {
        marker->SetName(fmt::format("Latency: {}ms", latency));
        marker->SetVisibility(VISIBILITY_GM);
    }
}

void NetworkPathVisualizer::ShowPacketLoss(Player* player, float lossPercentage)
{
    GameObject* marker = player->SummonGameObject(
        50073,
        player->GetPositionX(),
        player->GetPositionY(),
        player->GetPositionZ() + 4.0f,
        0, 0, 0, 0, 0,
        20
    );
    if (marker) {
        marker->SetName(fmt::format("Packet Loss: {:.1f}%", lossPercentage));
        marker->SetVisibility(VISIBILITY_GM);
    }
}
