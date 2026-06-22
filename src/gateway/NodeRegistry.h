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
 * @file NodeRegistry.h
 * @brief Gateway-side registry of backend node links + shared client routing map.
 *
 * Phase 2 replaces the single fixed NodeLink with a pool of links — one per
 * configured backend node (Node.1, Node.2, ...). The registry owns those
 * NodeLink instances and the SINGLE clientId -> ClientSocket* routing table.
 *
 * The client map lives at registry level (not per-link) on purpose: a client's
 * inbound server packets may arrive from whichever node currently fronts it,
 * and across phases a session can be re-homed from one node to another. Any
 * NodeLink that receives a GW_CLIENT_PACKET asks the registry for the owning
 * ClientSocket and delivers to it. The map is guarded by a mutex because both
 * the reactor threads (register/unregister at auth/close) and the link threads
 * (inbound delivery) touch it.
 *
 * Config keys (read in LoadFromConfig):
 *   Node.Count          uint32, default 1
 *   Node.<i>.Host       string, default 127.0.0.1
 *   Node.<i>.IntakePort  uint16 — the node's gateway intake port
 *                         (Node.<i>.GatewayPort is accepted as a back-compat
 *                          alias for the Phase 1 key)
 *   Gateway.Secret      string — pre-shared link secret (fail-closed if empty)
 *
 * A process-wide singleton is reached via sNodeRegistry().
 */

#ifndef GATEWAY_H_NODEREGISTRY
#define GATEWAY_H_NODEREGISTRY

#include <ace/Thread_Mutex.h>

#include "Common.h"
#include "ByteBuffer.h"

#include <map>
#include <string>

class ClientSocket;
class NodeLink;

/**
 * @brief Owns the per-node link pool and the shared clientId -> socket map.
 */
class NodeRegistry
{
    public:
        NodeRegistry();
        ~NodeRegistry();

        /// Read Node.Count + per-node Host/IntakePort + Gateway.Secret from the
        /// config and build a NodeLink for each configured node. Does NOT start
        /// the link threads (call StartAll for that). Safe to call once at boot.
        void LoadFromConfig();

        /// Start every configured node link (each connects + sends GW_HELLO).
        /// Fail-closed: if Gateway.Secret is empty, links are NOT started and an
        /// error is logged (an unauthenticated link would be rejected anyway).
        void StartAll();

        /// Stop + join every node link thread. Safe to call on shutdown.
        void StopAll();

        /// Look up a link by node id. Returns NULL if no such node is configured.
        NodeLink* Get(uint32 nodeId);

        /// The node a freshly-authed (player-less) client attaches to: the lowest
        /// node id that is currently connected. If none are connected yet, falls
        /// back to the lowest configured node id. Returns NULL only when no nodes
        /// are configured at all.
        NodeLink* PreWorldNode();

        /// Register / unregister a client by its gateway-assigned id so inbound
        /// node packets (from ANY link) can be routed back to the right socket.
        void RegisterClient(uint32 clientId, ClientSocket* sock);
        void UnregisterClient(uint32 clientId);

        /// Look up the ClientSocket fronting @p clientId (NULL if gone). Called
        /// from a link thread on inbound GW_CLIENT_PACKET.
        ClientSocket* FindClient(uint32 clientId);

        /// Resolve the backend node that owns the character @p guidLow (the low
        /// 32 bits of the player guid). Mirrors the mangosd login-affinity check
        /// in CharacterHandler::HandlePlayerLoginOpcode:
        ///   1) cluster_character_node(guid -> node_id) in the character DB; a
        ///      row with node_id > 0 wins.
        ///   2) else characters.zone (character DB) -> cluster_zone_assignment
        ///      (login DB: zone_id -> node_id); a row with node_id > 0 wins.
        ///   3) else 0 (unknown -> caller keeps the client on its current node).
        /// Runs DB queries inline; call only from a context that may block on DB.
        uint32 NodeForCharacter(uint32 guidLow);

    private:
        // nodeId -> link. std::map keeps ids ordered so PreWorldNode/lowest-id
        // queries are a simple begin()/ordered scan.
        std::map<uint32, NodeLink*> m_nodes;

        // The ONE shared clientId -> ClientSocket* routing table, guarded by the
        // mutex below (touched by reactor threads + link threads).
        std::map<uint32, ClientSocket*> m_clients;
        ACE_Thread_Mutex m_clientsMutex;

        std::string m_secret; // pre-shared link secret sent in each link's GW_HELLO
};

/// Process-wide NodeRegistry instance (replaces the Phase 1 sNodeLink).
NodeRegistry& sNodeRegistry();

#endif /* GATEWAY_H_NODEREGISTRY */
