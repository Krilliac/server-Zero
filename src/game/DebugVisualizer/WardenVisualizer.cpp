#include "WardenVisualizer.h"
#include "GameObject.h"

void WardenVisualizer::ShowViolation(Player* player, int type)
{
    GameObject* marker = player->SummonGameObject(
        50040, 
        player->GetPositionX(), 
        player->GetPositionY(), 
        player->GetPositionZ() + 4.0f, 
        0, 0, 0, 0, 0, 
        15
    );
    if (marker) {
        marker->SetName(fmt::format("Warden: Violation Type {}", type));
        marker->SetVisibility(VISIBILITY_GM);
    }
}

void WardenVisualizer::ShowScanResult(Player* player, const std::string& result)
{
    GameObject* marker = player->SummonGameObject(
        50041, 
        player->GetPositionX(), 
        player->GetPositionY(), 
        player->GetPositionZ() + 5.0f, 
        0, 0, 0, 0, 0, 
        20
    );
    if (marker) {
        marker->SetName("Warden: " + result);
        marker->SetVisibility(VISIBILITY_GM);
    }
}

void WardenVisualizer::ShowMemoryCheck(Player* player, uint32 address)
{
    GameObject* marker = player->SummonGameObject(
        50042, 
        player->GetPositionX(), 
        player->GetPositionY(), 
        player->GetPositionZ() + 6.0f, 
        0, 0, 0, 0, 0, 
        25
    );
    if (marker) {
        marker->SetName(fmt::format("Memory Check: 0x{:X}", address));
        marker->SetVisibility(VISIBILITY_GM);
    }
}
