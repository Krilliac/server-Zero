#include "ThreatVisualizer.h"
#include "GameObject.h"
#include "ThreatManager.h"

void ThreatVisualizer::ShowThreatDirection(Player* player, Unit* source, Unit* target)
{
    if (!source || !target) return;
    
    const Position& sPos = source->GetPosition();
    const Position& tPos = target->GetPosition();
    
    // Connection line
    Position mid = {
        (sPos.x + tPos.x) / 2,
        (sPos.y + tPos.y) / 2,
        (sPos.z + tPos.z) / 2 + 3.0f,
        0
    };
    
    GameObject* line = player->SummonGameObject(
        50120,
        mid.x, mid.y, mid.z,
        0, 0, 0, 0, 0,
        20
    );
    if (line) {
        line->SetName("Threat Direction");
        line->SetVisibility(VISIBILITY_GM);
    }
}

void ThreatVisualizer::ShowThreatValue(Player* player, Unit* source, Unit* target)
{
    if (!source || !target) return;
    
    ThreatManager& mgr = source->GetThreatManager();
    float threat = mgr.getThreat(target, false);
    
    const Position& pos = target->GetPosition();
    GameObject* marker = player->SummonGameObject(
        50121,
        pos.x, pos.y, pos.z + 2.0f,
        0, 0, 0, 0, 0,
        15
    );
    if (marker) {
        marker->SetName(fmt::format("Threat: {:.1f}%", threat * 100.0f));
        marker->SetVisibility(VISIBILITY_GM);
    }
}

void ThreatVisualizer::ShowThreatReset(Player* player, Unit* source)
{
    if (!source) return;
    
    const Position& pos = source->GetPosition();
    GameObject* marker = player->SummonGameObject(
        50122,
        pos.x, pos.y, pos.z + 5.0f,
        0, 0, 0, 0, 0,
        10
    );
    if (marker) {
        marker->SetName("Threat Reset");
        marker->SetVisibility(VISIBILITY_GM);
    }
}
