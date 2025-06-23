#pragma once
#include "Player.h"
#include "Spell.h"

class SpellVisualizer
{
public:
    static void ShowSpellTarget(Player* player, Spell* spell);
    static void ShowSpellEffectRadius(Player* player, Spell* spell, uint32 effIndex);
    static void ShowSpellCastTime(Player* player, Spell* spell);
};
