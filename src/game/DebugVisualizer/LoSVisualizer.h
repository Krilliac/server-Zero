#pragma once
#include "Player.h"
#include "Unit.h"

class LoSVisualizer
{
public:
    static void ShowLineOfSight(Player* player, Unit* source, Unit* target);
    static void ShowLoSFailure(Player* player, const Position& from, const Position& to);
    static void ShowLoSHeightCheck(Player* player, const Position& position, float height);
};
