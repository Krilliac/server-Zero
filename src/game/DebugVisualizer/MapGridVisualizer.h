#pragma once
#include "Player.h"

class MapGridVisualizer
{
public:
    static void ShowGrid(Player* player);
    static void ShowCell(Player* player, uint32 cellX, uint32 cellY);
    static void ShowActiveObjects(Player* player);
};
