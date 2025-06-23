#include "SpellRadiusVisualizer.h"
#include "GameObject.h"
#include "Map.h"

void SpellRadiusVisualizer::ShowSpellRadius(Player* player, const Position& center, float radius, uint32 duration)
{
    const uint32 points = 24;
    for (uint32 i = 0; i < points; ++i) {
        float angle = 2.0f * M_PI * i / points;
        float x = center.x + radius * cos(angle);
        float y = center.y + radius * sin(angle);
        float z = player->GetMap()->GetHeight(x, y, center.z);
        
        player->SummonGameObject(
            50130,
            x, y, z + 0.5f,
            0, 0, 0, 0, 0,
            duration
        );
    }
}

void SpellRadiusVisualizer::ShowSpellCone(Player* player, const Position& origin, float angle, float length, float width)
{
    const uint32 segments = 12;
    const float halfAngle = angle / 2.0f;
    
    for (uint32 i = 0; i <= segments; ++i) {
        float segAngle = -halfAngle + (angle * i / segments);
        float endX = origin.x + length * cos(segAngle);
        float endY = origin.y + length * sin(segAngle);
        float z = player->GetMap()->GetHeight(endX, endY, origin.z);
        
        player->SummonGameObject(
            50131,
            endX, endY, z + 0.5f,
            0, 0, 0, 0, 0,
            20
        );
        
        // Connect to origin
        if (i > 0) {
            Position mid = {
                (origin.x + endX) / 2,
                (origin.y + endY) / 2,
                (origin.z + z) / 2 + 1.0f,
                0
            };
            
            player->SummonGameObject(
                50132,
                mid.x, mid.y, mid.z,
                0, 0, 0, 0, 0,
                20
            );
        }
    }
}

void SpellRadiusVisualizer::ShowSpellChain(Player* player, const std::vector<Position>& targets)
{
    for (size_t i = 0; i < targets.size(); ++i) {
        const Position& pos = targets[i];
        GameObject* marker = player->SummonGameObject(
            50133,
            pos.x, pos.y, pos.z + 1.0f,
            0, 0, 0, 0, 0,
            25
        );
        if (marker) {
            marker->SetName(fmt::format("Chain {}", i));
            marker->SetVisibility(VISIBILITY_GM);
        }
        
        // Connect to previous target
        if (i > 0) {
            const Position& prev = targets[i-1];
            Position mid = {
                (prev.x + pos.x) / 2,
                (prev.y + pos.y) / 2,
                (prev.z + pos.z) / 2 + 2.0f,
                0
            };
            
            GameObject* line = player->SummonGameObject(
                50134,
                mid.x, mid.y, mid.z,
                0, 0, 0, 0, 0,
                25
            );
            if (line) {
                line->SetName("Spell Chain");
                line->SetVisibility(VISIBILITY_GM);
            }
        }
    }
}
