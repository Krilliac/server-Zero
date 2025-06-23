#pragma once
#include "WorldPacket.h"
#include <vector>
#include <chrono>
#include <mutex>

class PacketBundler
{
public:
    void AddPacket(const WorldPacket& packet);
    void Flush();
    void Update(uint32 diff);

    std::vector<uint8_t> GetBundle();

private:
    std::vector<WorldPacket> m_packets;
    std::mutex m_packetsMutex;
    std::chrono::steady_clock::time_point m_lastFlush;
    static constexpr size_t MAX_BUNDLE_SIZE = 8;
    static constexpr std::chrono::milliseconds FLUSH_INTERVAL{50};
};
