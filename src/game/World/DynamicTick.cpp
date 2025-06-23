#include "DynamicTick.h"
#include "Config.h"
#include "Log.h"

uint32_t DynamicTick::m_baseInterval = 20;
float DynamicTick::m_scaleFactor = 1.5f;
uint32_t DynamicTick::m_currentInterval = 20;
uint32_t DynamicTick::m_updateTimer = 0;

void DynamicTick::Initialize()
{
    m_baseInterval = sConfig.GetIntDefault("DynamicTick.BaseInterval", 20);
    m_scaleFactor = sConfig.GetFloatDefault("DynamicTick.ScaleFactor", 1.5f);
    m_currentInterval = m_baseInterval;
}

void DynamicTick::Update(uint32_t diff)
{
    m_updateTimer += diff;
    if (m_updateTimer >= 5000) // Recalculate every 5 seconds
    {
        const uint32_t playerCount = sWorld.GetPlayerCount();
        
        // Scale interval based on player load
        if (playerCount > 1000)
            m_currentInterval = static_cast<uint32_t>(m_baseInterval * m_scaleFactor);
        else
            m_currentInterval = m_baseInterval;
        
        m_updateTimer = 0;
    }
}

uint32_t DynamicTick::GetCurrentTickInterval()
{
    return m_currentInterval;
}
