#pragma once
#include "Player.h"
#include "Spell.h"

class SpellRadiusVisualizer
{
public:
    static void ShowSpellRadius(Player* player, const Position& center, float radius, uint32 duration = 20);
    static void ShowSpellCone(Player* player, const Position& origin, float angle, float length, float width);
    static void ShowSpellChain(Player* player, const std::vector<Position>& targets);
};
