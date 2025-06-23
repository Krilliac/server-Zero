#include "LoSVisualizer.h"
#include "GameObject.h"
#include "MapManager.h"

void LoSVisualizer::ShowLineOfSight(Player* player, Unit* source, Unit* target)
{
    if (!source || !target) return;
    
    const Position& sPos = source->GetPosition();
    const Position& tPos = target->GetPosition();
    
    // Create markers
    player->SummonGameObject(50140, sPos.x, sPos.y, sPos.z + 1.0f, 0, 0, 0, 0, 0, 20);
    player->SummonGameObject(50141, tPos.x, tPos.y, tPos.z + 1.0f, 0, 0, 0, 0, 0, 20);
    
    // Connection line
    Position mid = {
        (sPos.x + tPos.x) / 2,
        (sPos.y + tPos.y) / 2,
        (sPos.z + tPos.z) / 2 + 3.0f,
        0
    };
    
    GameObject* line = player->SummonGameObject(
        50142,
        mid.x, mid.y, mid.z,
        0, 0, 0, 0, 0,
        20
    );
    if (line) {
        line->SetName("Line of Sight");
        line->SetVisibility(VISIBILITY_GM);
    }
}

void LoSVisualizer::ShowLoSFailure(Player* player, const Position& from, const Position& to)
{
    // Create markers
    player->SummonGameObject(50143, from.x, from.y, from.z + 1.0f, 0, 0, 0, 0, 0, 25);
    player->SummonGameObject(50144, to.x, to.y, to.z + 1.0f, 0, 0, 0, 0, 0, 25);
    
    // Connection line (red)
    Position mid = {
        (from.x + to.x) / 2,
        (from.y + to.y) / 2,
        (from.z + to.z) / 2 + 5.0f,
        0
    };
    
    GameObject* line = player->SummonGameObject(
        50145,
        mid.x, mid.y, mid.z,
        0, 0, 0, 0, 0,
        25
    );
    if (line) {
        line->SetName("LoS Blocked");
        line->SetVisibility(VISIBILITY_GM);
    }
}

void LoSVisualizer::ShowLoSHeightCheck(Player* player, const Position& position, float height)
{
    // Ground marker
    player->SummonGameObject(50146, position.x, position.y, position.z, 0, 0, 0, 0, 0, 30);
    
    // Height marker
    player->SummonGameObject(
        50147,
        position.x, position.y, position.z + height,
        0, 0, 0, 0, 0,
        30
    );
    
    // Connection line
    Position mid = {
        position.x,
        position.y,
        position.z + height / 2,
        0
    };
    
    GameObject* line = player->SummonGameObject(
        50148,
        mid.x, mid.y, mid.z,
        0, 0, 0, 0, 0,
        30
    );
    if (line) {
        line->SetName(fmt::format("Height: {:.1f}", height));
        line->SetVisibility(VISIBILITY_GM);
    }
}
