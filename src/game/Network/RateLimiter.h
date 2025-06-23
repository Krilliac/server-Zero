#pragma once
#include <atomic>
#include <chrono>

class RateLimiter
{
public:
    RateLimiter();
    bool AllowPacket();

private:
    std::atomic<uint32_t> m_tokens;
    std::chrono::steady_clock::time_point m_lastRefill;
    static constexpr uint32_t MAX_TOKENS = 100;
    static constexpr uint32_t REFILL_RATE = 10; // tokens per second
};
