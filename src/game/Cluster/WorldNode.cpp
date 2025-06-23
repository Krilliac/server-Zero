#include "WorldNode.h"
#include "Hivemind.h"
#include "Player.h"
#include "WorldSession.h"
#include "Log.h"

WorldNode::WorldNode(uint32 nodeId, const std::string& address, uint32 port)
    : m_nodeId(nodeId), m_address(address), m_port(port) {}

void WorldNode::TransferPlayer(ObjectGuid playerGuid, const ByteBuffer& stateData)
{
    // In real implementation: send stateData to node via network
    sLog.outDebug("Node %d: Received player %s transfer", m_nodeId, playerGuid.ToString().c_str());
}

void WorldNode::HandlePlayerLogin(ObjectGuid playerGuid)
{
    sHivemind->MigratePlayer(sObjectAccessor.GetPlayer(playerGuid), m_nodeId);
}

void WorldNode::HandlePlayerLogout(ObjectGuid playerGuid)
{
    // Notify cluster about logout
}
