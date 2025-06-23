#include "MapGridVisualizer.h"
#include "GameObject.h"
#include "Map.h"
#include "GridNotifiers.h"
#include "CellImpl.h"

void MapGridVisualizer::ShowGrid(Player* player)
{
    Map* map = player->GetMap();
    uint32 cellX = player->GetPositionX() / SIZE_OF_GRID_CELL;
    uint32 cellY = player->GetPositionY() / SIZE_OF_GRID_CELL;
    
    // Visualize 3x3 grid around player
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            uint32 gridX = cellX + x;
            uint32 gridY = cellY + y;
            ShowCell(player, gridX, gridY);
        }
    }
}

void MapGridVisualizer::ShowCell(Player* player, uint32 cellX, uint32 cellY)
{
    // Calculate cell boundaries
    float minX = cellX * SIZE_OF_GRID_CELL;
    float maxX = minX + SIZE_OF_GRID_CELL;
    float minY = cellY * SIZE_OF_GRID_CELL;
    float maxY = minY + SIZE_OF_GRID_CELL;
    float z = player->GetPositionZ() + 1.0f;
    
    // Create markers at corners
    const uint32 markerId = 50050;
    player->SummonGameObject(markerId, minX, minY, z, 0, 0, 0, 0, 0, 30);
    player->SummonGameObject(markerId, minX, maxY, z, 0, 0, 0, 0, 0, 30);
    player->SummonGameObject(markerId, maxX, minY, z, 0, 0, 0, 0, 0, 30);
    player->SummonGameObject(markerId, maxX, maxY, z, 0, 0, 0, 0, 0, 30);
    
    // Create marker at center
    GameObject* center = player->SummonGameObject(
        markerId + 1,
        (minX + maxX) / 2,
        (minY + maxY) / 2,
        z + 2.0f,
        0, 0, 0, 0, 0,
        30
    );
    if (center) {
        center->SetName(fmt::format("Grid ({},{})", cellX, cellY));
        center->SetVisibility(VISIBILITY_GM);
    }
}

void MapGridVisualizer::ShowActiveObjects(Player* player)
{
    Map* map = player->GetMap();
    Cell cell(player->GetPosition());
    
    MaNGOS::GameObjectInRangeCheck check(player);
    MaNGOS::GameObjectListSearcher<MaNGOS::GameObjectInRangeCheck> searcher(player, check);
    
    TypeContainerVisitor<MaNGOS::GameObjectListSearcher<MaNGOS::GameObjectInRangeCheck>, GridTypeMapContainer> visitor(searcher);
    cell.Visit(cell, visitor, *map, *player, SIZE_OF_GRID_CELL);
    
    for (auto& obj : searcher.GetResults()) {
        GameObject* marker = player->SummonGameObject(
            50053,
            obj->GetPositionX(),
            obj->GetPositionY(),
            obj->GetPositionZ() + 3.0f,
            0, 0, 0, 0, 0,
            20
        );
        if (marker) {
            marker->SetName(obj->GetName());
            marker->SetVisibility(VISIBILITY_GM);
        }
    }
}
