#pragma once
#include <cstdint>

class DynamicTick
{
public:
    static void Initialize();
    static void Update(uint32_t diff);
    static uint32_t GetCurrentTickInterval();
    
    // Configurable parameters
    static void SetBaseInterval(uint32_t interval) { m_baseInterval = interval; }
    static void SetScaleFactor(float factor) { m_scaleFactor = factor; }

private:
    static uint32_t m_baseInterval;          // 20ms default
    static float m_scaleFactor;              // 1.5x scale under load
    static uint32_t m_currentInterval;       // Actual tick interval
    static uint32_t m_updateTimer;           // Recalculation timer
};
