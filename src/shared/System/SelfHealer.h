#pragma once
#include "World.h"
#include "Database/DatabaseEnv.h"
#include "Cluster/Hivemind.h"

class SelfHealer
{
public:
    static void Initialize();
    
private:
    static void CheckSystem();
    static void ScheduleNextCheck();
    
    // Health check subsystems
    static void CheckMemoryLeaks();
    static void CheckThreadDeadlocks();
    static void CheckDatabaseConnection();
    static void CheckPlayerStates();
    static void CheckClusterNodes();
    
    // Helper functions
    static size_t GetCurrentMemoryUsage();
    static void ScheduleNextCheck();
    
    // State tracking
    static std::atomic<bool> s_checkInProgress;
    static uint32_t s_lastMainThreadActivity;
};

// Global activity tracker callback signature
typedef std::function<void()> ThreadActivityCallback;
