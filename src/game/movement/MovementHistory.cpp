#include "MovementHistory.h"
#include "Player.h"
#include "World.h"
#include "Log.h"

// Initialize static members
std::unordered_map<ObjectGuid, std::deque<MovementSnapshot>> MovementHistory::m_history;
std::mutex MovementHistory::m_historyMutex;

void MovementHistory::RecordMovement(Player* player, MovementInfo const& moveInfo)
{
    std::lock_guard<std::mutex> lock(m_historyMutex);
    ObjectGuid guid = player->GetGUID();
    
    MovementSnapshot snapshot{
        moveInfo.GetPos(),                  // Position
        moveInfo.time,                      // Timestamp
        moveInfo.GetMovementFlags(),        // Movement flags
        player->GetSession()->GetLatency()  // Network latency
    };

    auto& history = m_history[guid];
    history.push_back(snapshot);

    // Maintain history size limit
    if (history.size() > MAX_HISTORY) {
        history.pop_front();
    }
}

MovementSnapshot MovementHistory::GetLastValidPosition(Player* player)
{
    std::lock_guard<std::mutex> lock(m_historyMutex);
    ObjectGuid guid = player->GetGUID();
    
    auto it = m_history.find(guid);
    if (it == m_history.end() || it->second.empty()) {
        // Return current position if no history
        return { player->GetPosition(), WorldTimer::getMSTime(), 0, 0 };
    }
    
    return it->second.back();
}

void MovementHistory::HandleTeleport(Player* player)
{
    std::lock_guard<std::mutex> lock(m_historyMutex);
    m_history.erase(player->GetGUID());
}

std::vector<MovementSnapshot> MovementHistory::GetHistory(Player* player)
{
    std::lock_guard<std::mutex> lock(m_historyMutex);
    ObjectGuid guid = player->GetGUID();
    
    auto it = m_history.find(guid);
    if (it == m_history.end()) {
        return {};
    }
    
    // Convert deque to vector
    return {it->second.begin(), it->second.end()};
}

void MovementHistory::ClearHistory(Player* player)
{
    std::lock_guard<std::mutex> lock(m_historyMutex);
    m_history.erase(player->GetGUID());
}
