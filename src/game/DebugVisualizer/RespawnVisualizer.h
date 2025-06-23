#pragma once
#include "Player.h"
#include "GameObject.h"
#include "Creature.h"

class RespawnVisualizer
{
public:
    static void ShowRespawnTimer(Player* player, GameObject* obj, uint32 respawnTime);
    static void ShowRespawnTimer(Player* player, Creature* creature, uint32 respawnTime);
    static void ShowRespawnArea(Player* player, float x, float y, float radius);
};
