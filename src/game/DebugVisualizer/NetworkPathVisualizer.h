#pragma once
#include "Player.h"
#include <vector>
#include "Position.h"

class NetworkPathVisualizer
{
public:
    static void ShowPacketRoute(Player* player, const std::vector<Position>& path);
    static void ShowLatency(Player* player, uint32 latency);
    static void ShowPacketLoss(Player* player, float lossPercentage);
};
