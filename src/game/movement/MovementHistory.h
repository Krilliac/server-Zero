#pragma once
#include "Player.h"
#include "MovementInfo.h"
#include <deque>
#include <unordered_map>
#include <mutex>

/**
 * @brief Stores movement snapshots for position validation and anti-cheat
 */
struct MovementSnapshot
{
    Position pos;           // Position at time of snapshot
    uint32 timestamp;       // Game time when snapshot was taken
    uint16 opcode;          // Movement opcode (for validation)
    uint32 latency;         // Network latency at time of snapshot
};

class MovementHistory
{
public:
    /**
     * Records a movement in the player's history
     * 
     * @param player Player object
     * @param moveInfo Movement information
     */
    static void RecordMovement(Player* player, MovementInfo const& moveInfo);
    
    /**
     * Gets the last valid position from history
     * 
     * @param player Player object
     * @return Last valid position snapshot
     */
    static MovementSnapshot GetLastValidPosition(Player* player);
    
    /**
     * Handles player teleport event (clears history)
     * 
     * @param player Player object
     */
    static void HandleTeleport(Player* player);
    
    /**
     * Gets full movement history for a player
     * 
     * @param player Player object
     * @return Vector of movement snapshots
     */
    static std::vector<MovementSnapshot> GetHistory(Player* player);
    
    /**
     * Clears movement history for a player
     * 
     * @param player Player object
     */
    static void ClearHistory(Player* player);

private:
    static constexpr size_t MAX_HISTORY = 10; // Last 10 movements
    
    // Thread-safe storage
    static std::unordered_map<ObjectGuid, std::deque<MovementSnapshot>> m_history;
    static std::mutex m_historyMutex;
};
