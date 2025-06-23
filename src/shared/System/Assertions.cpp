#include "Assertions.h"
#include "Log.h"
#include "World.h"
#include "Player.h"
#include "Chat.h"
#include <cstdlib>
#include <cassert>
#include <ctime>

namespace Assert
{
    static Config s_config;
    static bool s_initialized = false;

    void Initialize()
    {
        if (s_initialized) return;
        
        // Load configuration
        s_config.defaultLevel = static_cast<Level>(sConfig.GetIntDefault("Assertions.DefaultLevel", 1));
        s_config.logToConsole = sConfig.GetBoolDefault("Assertions.LogToConsole", true);
        s_config.logToFile = sConfig.GetBoolDefault("Assertions.LogToFile", true);
        s_config.triggerDebugger = sConfig.GetBoolDefault("Assertions.TriggerDebugger", true);
        s_config.terminateOnFatal = sConfig.GetBoolDefault("Assertions.TerminateOnFatal", true);
        s_config.broadcastToWorld = sConfig.GetBoolDefault("Assertions.BroadcastToWorld", false);
        s_config.allowContinueAfterFatal = sConfig.GetBoolDefault("Assertions.AllowContinueAfterFatal", false);
        
        s_initialized = true;
        sLog.outString("Assertions system initialized");
    }

    void SetConfig(const Config& config)
    {
        s_config = config;
    }

    void Fail(const std::string& message, Level level, const char* file, int line)
    {
        const std::string fullMessage = fmt::format("{} [{}:{}]", message, file, line);
        
        // Add breadcrumb for context
        BreadcrumbLogger::Add("Assertion", fullMessage, {
            {"level", std::to_string(static_cast<int>(level))},
            {"file", file},
            {"line", std::to_string(line)}
        });
        
        // Log based on configuration
        if (s_config.logToConsole) {
            switch (level) {
                case Level::Warning:
                    sLog.outError("WARNING: %s", fullMessage.c_str());
                    break;
                case Level::Error:
                    sLog.outError("ERROR: %s", fullMessage.c_str());
                    break;
                case Level::Fatal:
                    sLog.outError("FATAL: %s", fullMessage.c_str());
                    break;
            }
        }
        
        if (s_config.logToFile) {
            // Additional file logging implementation
        }
        
        // World broadcast for critical errors
        if (s_config.broadcastToWorld && (level == Level::Error || level == Level::Fatal)) {
            sWorld.SendWorldText(fmt::format("|cFFFF0000[ASSERT]|r {}", fullMessage));
        }
        
        // Custom handler override
        if (s_config.customHandler) {
            s_config.customHandler(fullMessage);
            return;
        }
        
        // Default behaviors
        switch (level) {
            case Level::Warning:
                // Just log, no interruption
                break;
                
            case Level::Error:
                if (s_config.triggerDebugger) {
                    // Trigger debugger if attached
                    #ifdef _WIN32
                        __debugbreak();
                    #else
                        raise(SIGTRAP);
                    #endif
                }
                break;
                
            case Level::Fatal:
                if (s_config.triggerDebugger) {
                    #ifdef _WIN32
                        __debugbreak();
                    #else
                        raise(SIGTRAP);
                    #endif
                }
                
                if (s_config.terminateOnFatal) {
                    // Create crash dump
                    CrashHandler::GenerateCrashDump();
                    
                    // Only terminate if not in risky mode
                    if (!s_config.allowContinueAfterFatal) {
                        std::abort();
                    }
                }
                break;
        }
    }
}
