#pragma once
#include "Player.h"
#include "Unit.h"

class CombatVisuals
{
public:
    static void ShowCombatRange(Player* player, Unit* attacker, Unit* target);
    static void ShowAttackAngle(Player* player, Unit* attacker, Unit* target);
    static void ShowCombatState(Player* player, Unit* unit);
};
