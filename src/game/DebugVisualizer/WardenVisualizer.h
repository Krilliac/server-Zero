#pragma once
#include "Player.h"

class WardenVisualizer
{
public:
    static void ShowViolation(Player* player, int type);
    static void ShowScanResult(Player* player, const std::string& result);
    static void ShowMemoryCheck(Player* player, uint32 address);
};
