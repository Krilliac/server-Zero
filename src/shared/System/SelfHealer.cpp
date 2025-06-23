#include "SelfHealer.h"
#include "World.h"
#include "Log.h"
#include "BreadcrumbLogger.h"
#include "Database/DatabaseEnv.h"
#include "Player.h"
#include "Map.h"
#include "Config.h"
#include <ctime>
#include <thread>
#include <atomic>

// Platform-specific memory monitoring
#ifdef _WIN32
#include <Psapi.h>
#else
#include <sys/resource.h>
#endif

namespace
{
    std::atomic<bool> s_checkInProgress(false);
    uint32_t s_lastMainThreadActivity = 0;
}

void SelfHealer::Initialize()
{
    // Register main thread activity tracker
    sWorld.RegisterThreadActivityCallback([]() {
        s_lastMainThreadActivity = static_cast<uint32_t>(std::time(nullptr));
    });
    
    // Start periodic checks
    ScheduleNextCheck();
}

void SelfHealer::ScheduleNextCheck()
{
    uint32_t interval = sConfig.GetIntDefault("SelfHealer.CheckInterval", 60000);
    sWorld.Schedule([this]() { CheckSystem(); }, interval);
}

void SelfHealer::CheckSystem()
{
    if (s_checkInProgress.exchange(true)) return;
    
    // 1. Memory leak detection
    CheckMemoryLeaks();
    
    // 2. Thread deadlock detection
    CheckThreadDeadlocks();
    
    // 3. Database connection health
    CheckDatabaseConnection();
    
    // 4. Player state consistency
    CheckPlayerStates();
    
    // 5. Cluster node health
    CheckClusterNodes();
    
    s_checkInProgress = false;
    ScheduleNextCheck();
}

void SelfHealer::CheckMemoryLeaks()
{
    static size_t lastMemoryUsage = 0;
    size_t currentUsage = GetCurrentMemoryUsage();
    size_t maxAllowed = sConfig.GetIntDefault("SelfHealer.MaxMemoryMB", 8000) * 1024 * 1024;
    
    if (currentUsage > maxAllowed) {
        sLog.outSelfHeal("Memory overuse detected: %.2f MB > %.2f MB",
                         currentUsage / (1024.0 * 1024.0),
                         maxAllowed / (1024.0 * 1024.0));
        
        // Mitigation: Clear caches
        sWorld.ClearNonCriticalCaches();
        BreadcrumbLogger::Add("SelfHeal", "Cleared caches due to memory overuse");
    }
    else if (currentUsage > lastMemoryUsage * 1.2) {
        sLog.outSelfHeal("Memory leak suspected: %.2f MB -> %.2f MB",
                         lastMemoryUsage / (1024.0 * 1024.0),
                         currentUsage / (1024.0 * 1024.0));
        BreadcrumbLogger::Add("SelfHeal", "Memory leak suspected");
    }
    
    lastMemoryUsage = currentUsage;
}

size_t SelfHealer::GetCurrentMemoryUsage()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return pmc.WorkingSetSize;
#else
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss * 1024; // Linux returns in KB
#endif
}

void SelfHealer::CheckThreadDeadlocks()
{
    const uint32_t now = static_cast<uint32_t>(std::time(nullptr));
    const uint32_t threshold = sConfig.GetIntDefault("SelfHealer.DeadlockThreshold", 30);
    
    if (now - s_lastMainThreadActivity > threshold) {
        sLog.outSelfHeal("Main thread deadlock suspected: No activity for %u seconds", 
                         now - s_lastMainThreadActivity);
        BreadcrumbLogger::Add("SelfHeal", "Main thread deadlock suspected");
        
        // Attempt recovery: Restart world thread
        sWorld.RestartWorldThread();
    }
}

void SelfHealer::CheckDatabaseConnection()
{
    if (!sDatabase->IsConnected()) {
        sLog.outSelfHeal("Database connection lost, attempting reconnect...");
        BreadcrumbLogger::Add("SelfHeal", "Database reconnect attempt");
        
        if (!sDatabase->Open()) {
            sLog.outSelfHeal("Database reconnect failed!");
            // If critical, initiate controlled shutdown
            if (sConfig.GetBoolDefault("SelfHealer.ShutdownOnDBFail", true)) {
                sWorld.Shutdown(10);
            }
        }
    }
}

void SelfHealer::CheckPlayerStates()
{
    uint32_t invalidCount = 0;
    auto sessions = sWorld.GetSessions();
    
    for (auto& [guid, session] : sessions) {
        Player* player = session->GetPlayer();
        if (!player) continue;
        
        // Check if player is in invalid state
        if ((player->IsInWorld() && !player->FindMap()) ||
            (!player->IsInWorld() && player->GetMap())) {
            sLog.outSelfHeal("Player %s in invalid state, resetting...", 
                            player->GetName());
            BreadcrumbLogger::Add("SelfHeal", "Player state reset", {
                {"player", player->GetName()},
                {"guid", std::to_string(player->GetGUID().GetCounter())}
            });
            
            player->TeleportToHome();
            invalidCount++;
        }
    }
    
    if (invalidCount > 0) {
        sLog.outSelfHeal("Reset %u players with invalid states", invalidCount);
    }
}

void SelfHealer::CheckClusterNodes()
{
    auto nodes = sHivemind->GetNodeIds();
    for (uint32_t nodeId : nodes) {
        if (!sHivemind->IsNodeOnline(nodeId)) {
            sLog.outSelfHeal("Cluster node %u offline, attempting restart...", nodeId);
            BreadcrumbLogger::Add("SelfHeal", "Cluster node restart", {
                {"node", std::to_string(nodeId)}
            });
            
            sHivemind->RestartNode(nodeId);
        }
    }
}
