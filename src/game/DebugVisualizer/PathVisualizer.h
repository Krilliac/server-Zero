#pragma once
#include "Player.h"
#include <vector>
#include "Position.h"

class PathVisualizer
{
public:
    static void ShowCreaturePath(Player* player, Creature* creature);
    static void ShowWaypointPath(Player* player, const std::vector<Position>& path);
    static void ShowFlightPath(Player* player, const std::vector<Position>& path);
};
