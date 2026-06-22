/**
 * MaNGOS is a full featured server for World of Warcraft, supporting
 * the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
 *
 * Copyright (C) 2005-2026 MaNGOS <https://www.getmangos.eu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * World of Warcraft, and all World of Warcraft or Warcraft art, images,
 * and lore are copyrighted by Blizzard Entertainment, Inc.
 */

/**
 * @file NodeRegistry.cpp
 * @brief Gateway-side registry of backend node links (implementation).
 */

#include <ace/Guard_T.h>

#include "NodeRegistry.h"
#include "NodeLink.h"

#include "Config/Config.h"
#include "Log.h"

#include "Database/DatabaseEnv.h"

#include <sstream>

/// Shared-DB accessors (defined in Main.cpp). The gateway opens both the login
/// (realmd) and character (characters) databases at boot for affinity lookups.
extern DatabaseType LoginDatabase;
extern DatabaseType CharacterDatabase;

NodeRegistry::NodeRegistry()
    : m_nodes(), m_clients(), m_clientsMutex(), m_secret()
{
}

NodeRegistry::~NodeRegistry()
{
    StopAll();

    for (std::map<uint32, NodeLink*>::iterator it = m_nodes.begin(); it != m_nodes.end(); ++it)
    {
        delete it->second;
    }
    m_nodes.clear();
}

void NodeRegistry::LoadFromConfig()
{
    m_secret = sConfig.GetStringDefault("Gateway.Secret", "");

    uint32 count = (uint32)sConfig.GetIntDefault("Node.Count", 1);
    if (count == 0)
    {
        sLog.outString("NodeRegistry: Node.Count = 0 — no backend nodes configured");
        return;
    }

    for (uint32 i = 1; i <= count; ++i)
    {
        std::ostringstream hostKey;
        hostKey << "Node." << i << ".Host";
        std::ostringstream portKey;
        portKey << "Node." << i << ".IntakePort";
        std::ostringstream legacyPortKey;
        legacyPortKey << "Node." << i << ".GatewayPort";

        std::string host = sConfig.GetStringDefault(hostKey.str().c_str(), "127.0.0.1");

        // Prefer the Phase 2 key (IntakePort); fall back to the Phase 1 alias
        // (GatewayPort) for back-compat. The default (9100) only applies when
        // neither key is present.
        int port = sConfig.GetIntDefault(portKey.str().c_str(), 0);
        if (port == 0)
        {
            port = sConfig.GetIntDefault(legacyPortKey.str().c_str(), 9100);
        }

        if (host.empty() || port == 0)
        {
            sLog.outError("NodeRegistry: node %u has no usable Host/IntakePort; skipping", i);
            continue;
        }

        NodeLink* link = new NodeLink(i, host, (uint16)port, m_secret);
        m_nodes[i] = link;

        sLog.outString("NodeRegistry: configured node %u -> %s:%d", i, host.c_str(), port);
    }
}

void NodeRegistry::StartAll()
{
    // Fail-closed: an empty secret means an unauthenticated link, which the node
    // rejects anyway. Refuse to connect and make the misconfiguration explicit.
    if (m_secret.empty())
    {
        if (!m_nodes.empty())
        {
            sLog.outError("NodeRegistry: %u node(s) configured but Gateway.Secret is empty — refusing to connect any node link; set a shared secret on the gateway and every node",
                          (uint32)m_nodes.size());
        }
        return;
    }

    for (std::map<uint32, NodeLink*>::iterator it = m_nodes.begin(); it != m_nodes.end(); ++it)
    {
        if (it->second->Start() == -1)
        {
            sLog.outError("NodeRegistry: failed to start link thread for node %u", it->first);
        }
    }
}

void NodeRegistry::StopAll()
{
    for (std::map<uint32, NodeLink*>::iterator it = m_nodes.begin(); it != m_nodes.end(); ++it)
    {
        it->second->Stop();
    }
}

NodeLink* NodeRegistry::Get(uint32 nodeId)
{
    std::map<uint32, NodeLink*>::iterator it = m_nodes.find(nodeId);
    return it != m_nodes.end() ? it->second : NULL;
}

NodeLink* NodeRegistry::PreWorldNode()
{
    if (m_nodes.empty())
    {
        return NULL;
    }

    // Prefer the lowest node id that is actually connected (the map is ordered).
    for (std::map<uint32, NodeLink*>::iterator it = m_nodes.begin(); it != m_nodes.end(); ++it)
    {
        if (it->second->IsConnected())
        {
            return it->second;
        }
    }

    // None connected yet: fall back to the lowest configured node id.
    return m_nodes.begin()->second;
}

void NodeRegistry::RegisterClient(uint32 clientId, ClientSocket* sock)
{
    ACE_GUARD(ACE_Thread_Mutex, guard, m_clientsMutex);
    m_clients[clientId] = sock;
}

void NodeRegistry::UnregisterClient(uint32 clientId)
{
    ACE_GUARD(ACE_Thread_Mutex, guard, m_clientsMutex);
    m_clients.erase(clientId);
}

ClientSocket* NodeRegistry::FindClient(uint32 clientId)
{
    ACE_GUARD_RETURN(ACE_Thread_Mutex, guard, m_clientsMutex, NULL);
    std::map<uint32, ClientSocket*>::iterator it = m_clients.find(clientId);
    return it != m_clients.end() ? it->second : NULL;
}

uint32 NodeRegistry::NodeForCharacter(uint32 guidLow)
{
    // 1) Direct character->node affinity. node_id 0 (or no row) means "any node",
    //    matching CharacterHandler::HandlePlayerLoginOpcode's semantics.
    if (QueryResult* res = CharacterDatabase.PQuery(
            "SELECT `node_id` FROM `cluster_character_node` WHERE `guid`=%u", guidLow))
    {
        uint32 nodeId = (*res)[0].GetUInt32();
        delete res;
        if (nodeId != 0)
        {
            return nodeId;
        }
    }

    // 2) Zone-based fallback: the character's current zone -> owning node.
    uint32 zone = 0;
    if (QueryResult* res = CharacterDatabase.PQuery(
            "SELECT `zone` FROM `characters` WHERE `guid`=%u", guidLow))
    {
        zone = (*res)[0].GetUInt32();
        delete res;
    }

    if (zone != 0)
    {
        if (QueryResult* res = LoginDatabase.PQuery(
                "SELECT `node_id` FROM `cluster_zone_assignment` WHERE `zone_id`=%u", zone))
        {
            uint32 nodeId = (*res)[0].GetUInt32();
            delete res;
            if (nodeId != 0)
            {
                return nodeId;
            }
        }
    }

    // 3) Unknown: caller keeps the client on its current (pre-world) node.
    return 0;
}

NodeRegistry& sNodeRegistry()
{
    static NodeRegistry instance;
    return instance;
}
