#pragma once
#include <cstdint>
#include <string>
#include "ByteBuffer.h"
#include "ObjectGuid.h"

class WorldNode
{
public:
    WorldNode(uint32 nodeId, const std::string& address, uint32 port);

    uint32 GetNodeId() const { return m_nodeId; }
    const std::string& GetAddress() const { return m_address; }
    uint32 GetPort() const { return m_port; }

    void TransferPlayer(ObjectGuid playerGuid, const ByteBuffer& stateData);
    void HandlePlayerLogin(ObjectGuid playerGuid);
    void HandlePlayerLogout(ObjectGuid playerGuid);

private:
    uint32 m_nodeId;
    std::string m_address;
    uint32 m_port;
};
