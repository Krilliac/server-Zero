#pragma once
#include "Player.h"
#include <string>

class AntiCheatVisuals
{
public:
    static void ShowViolation(Player* player, const std::string& reason);
    static void ShowSpeedHack(Player* player, float speedRatio);
    static void ShowFlyHack(Player* player, float verticalSpeed);
};
