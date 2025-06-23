#include "PacketBundler.h"
#include "Log.h"
#include "ZstdCompression.h"
#include <algorithm>

void PacketBundler::AddPacket(const WorldPacket& packet)
{
    std::lock_guard<std::mutex> lock(m_packetsMutex);
    m_packets.push_back(packet);
    
    if (m_packets.size() >= MAX_BUNDLE_SIZE) {
        Flush();
    }
}

void PacketBundler::Flush()
{
    std::lock_guard<std::mutex> lock(m_packetsMutex);
    if (m_packets.empty()) return;

    // Serialize all packets into single bundle
    ByteBuffer bundle;
    for (auto& packet : m_packets) {
        bundle << static_cast<uint16_t>(packet.size());
        bundle.append(packet.contents(), packet.size());
    }
    
    // Compress the bundle
    WorldPacket compressed = ZstdCompress(WorldPacket(bundle), 1);
    
    // In real implementation: send compressed bundle
    sLog.outNetwork("Flushing %u packets as %u byte bundle", 
                    m_packets.size(), compressed.size());
    
    m_packets.clear();
    m_lastFlush = std::chrono::steady_clock::now();
}

void PacketBundler::Update(uint32 diff)
{
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastFlush);
    
    if (elapsed > FLUSH_INTERVAL && !m_packets.empty()) {
        Flush();
    }
}

std::vector<uint8_t> PacketBundler::GetBundle()
{
    std::lock_guard<std::mutex> lock(m_packetsMutex);
    ByteBuffer bundle;
    
    for (auto& packet : m_packets) {
        bundle << static_cast<uint16_t>(packet.size());
        bundle.append(packet.contents(), packet.size());
    }
    
    return std::vector<uint8_t>(bundle.contents(), bundle.contents() + bundle.size());
}
