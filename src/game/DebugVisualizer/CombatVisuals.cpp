#include "CombatVisuals.h"
#include "GameObject.h"

void CombatVisuals::ShowCombatRange(Player* player, Unit* attacker, Unit* target)
{
    if (!attacker || !target) return;
    
    const Position& aPos = attacker->GetPosition();
    const Position& tPos = target->GetPosition();
    float distance = attacker->GetDistance(target);
    
    // Connection line
    Position mid = {
        (aPos.x + tPos.x) / 2,
        (aPos.y + tPos.y) / 2,
        (aPos.z + tPos.z) / 2 + 2.0f,
        0
    };
    
    GameObject* line = player->SummonGameObject(
        50160,
        mid.x, mid.y, mid.z,
        0, 0, 0, 0, 0,
        15
    );
    if (line) {
        line->SetName(fmt::format("Range: {:.1f}yd", distance));
        line->SetVisibility(VISIBILITY_GM);
    }
}

void CombatVisuals::ShowAttackAngle(Player* player, Unit* attacker, Unit* target)
{
    if (!attacker || !target) return;
    
    const Position& aPos = attacker->GetPosition();
    const Position& tPos = target->GetPosition();
    
    // Calculate angle
    float angle = attacker->GetAngle(target);
    float visualAngle = angle - M_PI / 8; // Show 45 degree cone
    
    // Create cone
    const uint32 segments = 5;
    float length = 5.0f;
    
    for (uint32 i = 0; i <= segments; ++i) {
        float segAngle = visualAngle + (M_PI / 4 * i / segments);
        float endX = aPos.x + length * cos(segAngle);
        float endY = aPos.y + length * sin(segAngle);
        
        player->SummonGameObject(
            50161,
            endX, endY, aPos.z,
            0, 0, 0, 0, 0,
            15
        );
    }
}

void CombatVisuals::ShowCombatState(Player* player, Unit* unit)
{
    if (!unit) return;
    
    const Position& pos = unit->GetPosition();
    GameObject* marker = player->SummonGameObject(
        50162,
        pos.x, pos.y, pos.z + 3.0f,
        0, 0, 0, 0, 0,
        20
    );
    if (marker) {
        marker->SetName(fmt::format("Combat: {}", unit->IsInCombat() ? "Active" : "Inactive"));
        marker->SetVisibility(VISIBILITY_GM);
    }
}
