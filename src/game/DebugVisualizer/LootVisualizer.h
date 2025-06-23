#pragma once
#include "Player.h"
#include "WorldObject.h"

class LootVisualizer
{
public:
    static void ShowLootTable(Player* player, WorldObject* source);
    static void ShowLootRoll(Player* player, uint32 itemId, uint32 rollResult);
    static void ShowLootDistribution(Player* player, uint32 itemId, const std::string& recipient);
};
