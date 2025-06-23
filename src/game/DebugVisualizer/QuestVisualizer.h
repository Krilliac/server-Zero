#pragma once
#include "Player.h"
#include "Quest.h"

class QuestVisualizer
{
public:
    static void ShowQuestArea(Player* player, uint32 questId, const Position& center, float radius);
    static void ShowQuestObjective(Player* player, uint32 questId, uint32 objectiveIndex, const Position& position);
    static void ShowQuestTurnIn(Player* player, uint32 questId, const Position& npcPosition);
};
