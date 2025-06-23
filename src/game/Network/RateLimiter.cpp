#include "RateLimiter.h"
#include <chrono>

RateLimiter::RateLimiter()
    : m_tokens(MAX_TOKENS), m_lastRefill(std::chrono::steady_clock::now())
{}

bool RateLimiter::AllowPacket()
{
    // Refill tokens based on elapsed time
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastRefill);
    uint32_t tokensToAdd = (elapsed.count() * REFILL_RATE) / 1000;
    
    if (tokensToAdd > 0) {
        m_tokens = std::min(MAX_TOKENS, m_tokens.load() + tokensToAdd);
        m_lastRefill = now;
    }
    
    // Check if we can allow the packet
    if (m_tokens > 0) {
        --m_tokens;
        return true;
    }
    return false;
}
