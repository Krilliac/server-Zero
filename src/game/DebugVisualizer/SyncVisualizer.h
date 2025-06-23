#pragma once
#include "Player.h"

class SyncVisualizer
{
public:
    static void ShowDrift(Player* player, float drift, float latency);
    static void ShowDesync(Player* player, float desyncLevel);
    static void ShowTimeSkip(Player* player, uint32 skippedTime);
};
