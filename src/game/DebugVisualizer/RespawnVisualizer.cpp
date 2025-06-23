#include "RespawnVisualizer.h"
#include "GameObject.h"
#include "Map.h"

void RespawnVisualizer::ShowRespawnTimer(Player* player, GameObject* obj, uint32 respawnTime)
{
    if (!obj) return;
    
    GameObject* marker = player->SummonGameObject(
        50060,
        obj->GetPositionX(),
        obj->GetPositionY(),
        obj->GetPositionZ() + 2.0f,
        0, 0, 0, 0, 0,
        respawnTime / 1000  // Show until respawn
    );
    if (marker) {
        marker->SetName(fmt::format("Respawn: {}s", respawnTime / 1000));
        marker->SetVisibility(VISIBILITY_GM);
    }
}

void RespawnVisualizer::ShowRespawnTimer(Player* player, Creature* creature, uint32 respawnTime)
{
    if (!creature) return;
    
    GameObject* marker = player->SummonGameObject(
        50061,
        creature->GetPositionX(),
        creature->GetPositionY(),
        creature->GetPositionZ() + 2.0f,
        0, 0, 0, 0, 0,
        respawnTime / 1000  // Show until respawn
    );
    if (marker) {
        marker->SetName(fmt::format("Respawn: {}s", respawnTime / 1000));
        marker->SetVisibility(VISIBILITY_GM);
    }
}

void RespawnVisualizer::ShowRespawnArea(Player* player, float x, float y, float radius)
{
    const uint32 points = 12;
    for (uint32 i = 0; i < points; ++i) {
        float angle = 2.0f * M_PI * i / points;
        float px = x + radius * cos(angle);
        float py = y + radius * sin(angle);
        float z = player->GetMap()->GetHeight(px, py, player->GetPositionZ());
        
        player->SummonGameObject(
            50062,
            px, py, z + 0.5f,
            0, 0, 0, 0, 0,
            30
        );
    }
}
