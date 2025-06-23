#include "AntiCheatVisuals.h"
#include "GameObject.h"

void AntiCheatVisuals::ShowViolation(Player* player, const std::string& reason)
{
    GameObject* marker = player->SummonGameObject(
        50020, 
        player->GetPositionX(), 
        player->GetPositionY(), 
        player->GetPositionZ() + 2.0f, 
        0, 0, 0, 0, 0, 
        10
    );
    if (marker) {
        marker->SetName("AntiCheat: " + reason);
        marker->SetVisibility(VISIBILITY_GM);
    }
}

void AntiCheatVisuals::ShowSpeedHack(Player* player, float speedRatio)
{
    GameObject* marker = player->SummonGameObject(
        50021, 
        player->GetPositionX(), 
        player->GetPositionY(), 
        player->GetPositionZ() + 3.0f, 
        0, 0, 0, 0, 0, 
        15
    );
    if (marker) {
        marker->SetName(fmt::format("SpeedHack: {:.2f}x", speedRatio));
        marker->SetVisibility(VISIBILITY_GM);
    }
}

void AntiCheatVisuals::ShowFlyHack(Player* player, float verticalSpeed)
{
    GameObject* marker = player->SummonGameObject(
        50022, 
        player->GetPositionX(), 
        player->GetPositionY(), 
        player->GetPositionZ() + 4.0f, 
        0, 0, 0, 0, 0, 
        15
    );
    if (marker) {
        marker->SetName(fmt::format("FlyHack: {:.1f}yd/s", verticalSpeed));
        marker->SetVisibility(VISIBILITY_GM);
    }
}
