#include "AIVisualizer.h"
#include "GameObject.h"
#include "ThreatManager.h"

void AIVisualizer::ShowAIState(Player* player, Creature* creature)
{
    if (!creature) return;
    
    const Position& pos = creature->GetPosition();
    GameObject* marker = player->SummonGameObject(
        50100,
        pos.x, pos.y, pos.z + 3.0f,
        0, 0, 0, 0, 0,
        20
    );
    if (marker) {
        marker->SetName(fmt::format("AI: {}", creature->GetAIStateString()));
        marker->SetVisibility(VISIBILITY_GM);
    }
}

void AIVisualizer::ShowThreatList(Player* player, Unit* target)
{
    if (!target) return;
    
    ThreatManager& mgr = target->GetThreatManager();
    auto const& threatList = mgr.GetThreatList();
    
    float zOffset = 4.0f;
    for (auto const* ref : threatList) {
        if (Unit* unit = ref->getTarget()) {
            const Position& pos = unit->GetPosition();
            GameObject* marker = player->SummonGameObject(
                50101,
                pos.x, pos.y, pos.z + zOffset,
                0, 0, 0, 0, 0,
                25
            );
            if (marker) {
                marker->SetName(fmt::format("Threat: {:.1f}%", ref->getThreat() * 100.0f));
                marker->SetVisibility(VISIBILITY_GM);
                zOffset += 1.0f;
            }
        }
    }
}

void AIVisualizer::ShowAITarget(Player* player, Creature* creature)
{
    if (!creature || !creature->GetVictim()) return;
    
    const Position& cPos = creature->GetPosition();
    const Position& tPos = creature->GetVictim()->GetPosition();
    
    // Marker on creature
    player->SummonGameObject(
        50102,
        cPos.x, cPos.y, cPos.z + 2.0f,
        0, 0, 0, 0, 0,
        20
    );
    
    // Marker on target
    GameObject* targetMarker = player->SummonGameObject(
        50103,
        tPos.x, tPos.y, tPos.z + 2.0f,
        0, 0, 0, 0, 0,
        20
    );
    if (targetMarker) {
        targetMarker->SetName(fmt::format("Target: {}", creature->GetVictim()->GetName()));
        targetMarker->SetVisibility(VISIBILITY_GM);
    }
    
    // Connection line
    Position mid = {
        (cPos.x + tPos.x) / 2,
        (cPos.y + tPos.y) / 2,
        (cPos.z + tPos.z) / 2 + 5.0f,
        0
    };
    
    GameObject* line = player->SummonGameObject(
        50104,
        mid.x, mid.y, mid.z,
        0, 0, 0, 0, 0,
        20
    );
    if (line) {
        line->SetName("AI Target");
        line->SetVisibility(VISIBILITY_GM);
    }
}
