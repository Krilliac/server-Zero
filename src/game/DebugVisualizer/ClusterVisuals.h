#pragma once
#include "Player.h"

class ClusterVisuals
{
public:
    static void ShowMigration(Player* player, uint32 fromNode, uint32 toNode);
    static void ShowNodeStatus(Player* gm);
    static void ShowNetworkPath(Player* player);

private:
    static constexpr uint32 MIGRATION_BEAM_ID = 50000;
    static constexpr uint32 NODE_MARKER_ID = 50001;
};
