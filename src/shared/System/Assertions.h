#pragma once
#include "Config.h"
#include "BreadcrumbLogger.h"
#include "World.h"
#include <string>
#include <functional>
#include <csignal>

namespace Assert
{
    enum class Level {
        Warning,
        Error,
        Fatal
    };

    struct Config {
        Level defaultLevel = Level::Error;
        bool logToConsole = true;
        bool logToFile = true;
        bool triggerDebugger = true;
        bool terminateOnFatal = true;
        bool broadcastToWorld = false;   // Broadcast to all players
        bool allowContinueAfterFatal = false; // Risky mode
        std::function<void(const std::string&)> customHandler;
    };

    void Initialize();
    void SetConfig(const Config& config);
    
    void Fail(const std::string& message, 
              Level level = Level::Error,
              const char* file = __FILE__,
              int line = __LINE__);
    
    // Enhanced assertion macros
    #define MANGOS_ASSERT(expr, ...) \
        do { \
            if (!(expr)) { \
                std::string msg = fmt::format("Assertion failed: {} ({})", #expr, ##__VA_ARGS__); \
                Assert::Fail(msg, Assert::Level::Error, __FILE__, __LINE__); \
            } \
        } while (0)
    
    #define MANGOS_WARN(expr, ...) \
        do { \
            if (!(expr)) { \
                std::string msg = fmt::format("Warning: {}", ##__VA_ARGS__); \
                Assert::Fail(msg, Assert::Level::Warning, __FILE__, __LINE__); \
            } \
        } while (0)
    
    #define MANGOS_FATAL(expr, ...) \
        do { \
            if (!(expr)) { \
                std::string msg = fmt::format("Fatal: {}", ##__VA_ARGS__); \
                Assert::Fail(msg, Assert::Level::Fatal, __FILE__, __LINE__); \
            } \
        } while (0)
}
