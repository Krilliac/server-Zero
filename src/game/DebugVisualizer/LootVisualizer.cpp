#include "LootVisualizer.h"
#include "GameObject.h"
#include "LootMgr.h"

void LootVisualizer::ShowLootTable(Player* player, WorldObject* source)
{
    if (!source) return;
    
    Position pos = source->GetPosition();
    pos.z += 2.0f;
    
    GameObject* marker = player->SummonGameObject(
        50080,
        pos.x, pos.y, pos.z,
        0, 0, 0, 0, 0,
        30
    );
    if (marker) {
        marker->SetName(fmt::format("Loot Table: {}", source->GetName()));
        marker->SetVisibility(VISIBILITY_GM);
    }
}

void LootVisualizer::ShowLootRoll(Player* player, uint32 itemId, uint32 rollResult)
{
    GameObject* marker = player->SummonGameObject(
        50081,
        player->GetPositionX(),
        player->GetPositionY(),
        player->GetPositionZ() + 3.0f,
        0, 0, 0, 0, 0,
        20
    );
    if (marker) {
        marker->SetName(fmt::format("Roll: {} -> {}", itemId, rollResult));
        marker->SetVisibility(VISIBILITY_GM);
    }
}

void LootVisualizer::ShowLootDistribution(Player* player, uint32 itemId, const std::string& recipient)
{
    GameObject* marker = player->SummonGameObject(
        50082,
        player->GetPositionX(),
        player->GetPositionY(),
        player->GetPositionZ() + 4.0f,
        0, 0, 0, 0, 0,
        25
    );
    if (marker) {
        marker->SetName(fmt::format("Loot: {} -> {}", itemId, recipient));
        marker->SetVisibility(VISIBILITY_GM);
    }
}
