#pragma once
#include "Player.h"
#include "Creature.h"
#include "Unit.h"

class AIVisualizer
{
public:
    static void ShowAIState(Player* player, Creature* creature);
    static void ShowThreatList(Player* player, Unit* target);
    static void ShowAITarget(Player* player, Creature* creature);
};
