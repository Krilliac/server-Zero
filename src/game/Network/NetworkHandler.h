#ifndef _NETWORK_HANDLER_H
#define _NETWORK_HANDLER_H

#include "Common.h"
#include "WorldPacket.h"
#include <vector>

class WorldSession;

class NetworkHandler
{
public:
    static NetworkHandler* instance();
    
    void Initialize();
    void Update(uint32 diff);
    
    WorldPacket CompressPacket(const WorldPacket& source);
    bool ValidatePacket(WorldSession* session, WorldPacket& packet);

private:
    void InitializeUDPSocket();
    void ProcessUDPQueue();
    void HandleUDPPacket(const char* data, size_t size, sockaddr_in& fromAddr);
    void UpdateBandwidthLimits(uint32 diff);
    void LoadEncryptionKeys();
    
    int m_udpSocket = -1;
    bool m_enableUDP = false;
    uint16 m_udpPort = 3725;
    uint32 m_compressionThreshold = 128;
};

#define sNetworkHandler NetworkHandler::instance()

#endif