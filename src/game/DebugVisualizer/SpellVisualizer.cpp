#include "SpellVisualizer.h"
#include "GameObject.h"
#include "SpellAuras.h"
#include "SpellTarget.h"

void SpellVisualizer::ShowSpellTarget(Player* player, Spell* spell)
{
    if (!spell) return;
    
    WorldObject* target = spell->m_targets.getUnitTarget();
    if (!target) target = spell->m_targets.getGameObjectTarget();
    if (!target) return;
    
    const Position& pos = target->GetPosition();
    GameObject* marker = player->SummonGameObject(
        50110,
        pos.x, pos.y, pos.z + 2.0f,
        0, 0, 0, 0, 0,
        15
    );
    if (marker) {
        marker->SetName(fmt::format("Spell Target: {}", spell->GetSpellInfo()->SpellName[0]));
        marker->SetVisibility(VISIBILITY_GM);
    }
}

void SpellVisualizer::ShowSpellEffectRadius(Player* player, Spell* spell, uint32 effIndex)
{
    if (!spell || effIndex >= MAX_EFFECT_INDEX) return;
    
    const SpellEntry* info = spell->GetSpellInfo();
    float radius = GetSpellRadius(sSpellRadiusStore.LookupEntry(info->EffectRadiusIndex[effIndex]));
    
    if (radius <= 0.1f) return;
    
    const Position& center = spell->m_targets.getDestination();
    const uint32 points = 24;
    
    for (uint32 i = 0; i < points; ++i) {
        float angle = 2.0f * M_PI * i / points;
        float x = center.x + radius * cos(angle);
        float y = center.y + radius * sin(angle);
        float z = player->GetMap()->GetHeight(x, y, center.z);
        
        player->SummonGameObject(
            50111,
            x, y, z + 0.5f,
            0, 0, 0, 0, 0,
            20
        );
    }
}

void SpellVisualizer::ShowSpellCastTime(Player* player, Spell* spell)
{
    if (!spell) return;
    
    const Position& pos = player->GetPosition();
    GameObject* marker = player->SummonGameObject(
        50112,
        pos.x, pos.y, pos.z + 3.0f,
        0, 0, 0, 0, 0,
        spell->GetCastTime() / 1000
    );
    if (marker) {
        marker->SetName(fmt::format("Cast: {}ms", spell->GetCastTime()));
        marker->SetVisibility(VISIBILITY_GM);
    }
}
