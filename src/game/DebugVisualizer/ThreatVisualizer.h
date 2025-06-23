#pragma once
#include "Player.h"
#include "Unit.h"

class ThreatVisualizer
{
public:
    static void ShowThreatDirection(Player* player, Unit* source, Unit* target);
    static void ShowThreatValue(Player* player, Unit* source, Unit* target);
    static void ShowThreatReset(Player* player, Unit* source);
};
