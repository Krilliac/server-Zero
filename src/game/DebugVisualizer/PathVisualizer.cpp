#include "PathVisualizer.h"
#include "GameObject.h"
#include "Creature.h"
#include "WaypointManager.h"

void PathVisualizer::ShowCreaturePath(Player* player, Creature* creature)
{
    if (!creature) return;
    
    const std::vector<Position>& path = creature->GetPath();
    for (size_t i = 0; i < path.size(); ++i) {
        const Position& pos = path[i];
        GameObject* marker = player->SummonGameObject(
            50090,
            pos.x, pos.y, pos.z + 0.5f,
            0, 0, 0, 0, 0,
            30
        );
        if (marker) {
            marker->SetName(fmt::format("Step {}", i));
            marker->SetVisibility(VISIBILITY_GM);
        }
        
        // Connect path points
        if (i > 0) {
            const Position& prev = path[i-1];
            Position mid = {
                (prev.x + pos.x) / 2,
                (prev.y + pos.y) / 2,
                (prev.z + pos.z) / 2 + 2.0f,
                0
            };
            
            GameObject* line = player->SummonGameObject(
                50091,
                mid.x, mid.y, mid.z,
                0, 0, 0, 0, 0,
                30
            );
            if (line) {
                line->SetName("Creature Path");
                line->SetVisibility(VISIBILITY_GM);
            }
        }
    }
}

void PathVisualizer::ShowWaypointPath(Player* player, const std::vector<Position>& path)
{
    for (size_t i = 0; i < path.size(); ++i) {
        const Position& pos = path[i];
        player->SummonGameObject(
            50092,
            pos.x, pos.y, pos.z + 0.5f,
            0, 0, 0, 0, 0,
            30
        );
        
        // Connect waypoints
        if (i > 0) {
            const Position& prev = path[i-1];
            Position mid = {
                (prev.x + pos.x) / 2,
                (prev.y + pos.y) / 2,
                (prev.z + pos.z) / 2 + 1.5f,
                0
            };
            
            GameObject* line = player->SummonGameObject(
                50093,
                mid.x, mid.y, mid.z,
                0, 0, 0, 0, 0,
                30
            );
            if (line) {
                line->SetName("Waypoint Path");
                line->SetVisibility(VISIBILITY_GM);
            }
        }
    }
}

void PathVisualizer::ShowFlightPath(Player* player, const std::vector<Position>& path)
{
    for (size_t i = 0; i < path.size(); ++i) {
        const Position& pos = path[i];
        GameObject* marker = player->SummonGameObject(
            50094,
            pos.x, pos.y, pos.z + 10.0f,
            0, 0, 0, 0, 0,
            30
        );
        if (marker) {
            marker->SetName(fmt::format("Flight {}", i));
            marker->SetVisibility(VISIBILITY_GM);
        }
        
        // Connect flight points
        if (i > 0) {
            const Position& prev = path[i-1];
            Position mid = {
                (prev.x + pos.x) / 2,
                (prev.y + pos.y) / 2,
                (prev.z + pos.z) / 2 + 15.0f,
                0
            };
            
            GameObject* line = player->SummonGameObject(
                50095,
                mid.x, mid.y, mid.z,
                0, 0, 0, 0, 0,
                30
            );
            if (line) {
                line->SetName("Flight Path");
                line->SetVisibility(VISIBILITY_GM);
            }
        }
    }
}
