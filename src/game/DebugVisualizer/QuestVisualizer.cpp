#include "QuestVisualizer.h"
#include "GameObject.h"
#include "QuestDef.h"

void QuestVisualizer::ShowQuestArea(Player* player, uint32 questId, const Position& center, float radius)
{
    const uint32 points = 32;
    for (uint32 i = 0; i < points; ++i) {
        float angle = 2.0f * M_PI * i / points;
        float x = center.x + radius * cos(angle);
        float y = center.y + radius * sin(angle);
        float z = player->GetMap()->GetHeight(x, y, center.z);
        
        player->SummonGameObject(
            50150,
            x, y, z + 0.5f,
            0, 0, 0, 0, 0,
            60
        );
    }
    
    // Center marker
    GameObject* marker = player->SummonGameObject(
        50151,
        center.x, center.y, center.z + 3.0f,
        0, 0, 0, 0, 0,
        60
    );
    if (marker) {
        Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
        marker->SetName(quest ? quest->GetTitle() : "Unknown Quest");
        marker->SetVisibility(VISIBILITY_GM);
    }
}

void QuestVisualizer::ShowQuestObjective(Player* player, uint32 questId, uint32 objectiveIndex, const Position& position)
{
    GameObject* marker = player->SummonGameObject(
        50152,
        position.x, position.y, position.z + 1.0f,
        0, 0, 0, 0, 0,
        45
    );
    if (marker) {
        marker->SetName(fmt::format("Objective {}: Quest {}", objectiveIndex, questId));
        marker->SetVisibility(VISIBILITY_GM);
    }
}

void QuestVisualizer::ShowQuestTurnIn(Player* player, uint32 questId, const Position& npcPosition)
{
    GameObject* marker = player->SummonGameObject(
        50153,
        npcPosition.x, npcPosition.y, npcPosition.z + 2.0f,
        0, 0, 0, 0, 0,
        60
    );
    if (marker) {
        Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
        marker->SetName(quest ? "Turn In: " + quest->GetTitle() : "Unknown Quest Turn-in");
        marker->SetVisibility(VISIBILITY_GM);
    }
}
