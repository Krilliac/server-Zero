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
 * @file GatewayProtocol.h
 * @brief Shared wire protocol for the gateway <-> node tunnel.
 *
 * This header is shared by BOTH the cluster gateway daemon and the game
 * server (mangosd) node intake, so it lives under src/shared and pulls in
 * only the shared ByteBuffer (no game opcode/session headers).
 *
 * Wire frame (little-endian, via ByteBuffer): [uint32 payloadLen][uint8 type][payload].
 * payloadLen counts only the payload bytes that follow the type byte. This is
 * intentionally identical in shape to the inter-node ClusterFrame so both
 * transports parse the same way.
 *
 * Per-message payloads:
 *   GW_SESSION_OPEN    : uint32 clientId, uint32 accountId, uint32 security,
 *                        uint8 locale, string accountName
 *   GW_CLIENT_PACKET   : uint32 clientId, uint16 opcode, raw packet bytes.
 *                        Direction is implied by the sender:
 *                          gateway -> node = the client's (decrypted) packet
 *                          node -> gateway = the server's packet for that client
 *   GW_SESSION_RELEASE : uint32 clientId
 *   GW_HELLO           : string secret, uint32 protocolVersion
 *                        Link-authentication handshake. The gateway MUST send
 *                        this as the very first frame after connecting; the node
 *                        compares the secret (constant-time) against its
 *                        configured Gateway.Secret before honoring any other
 *                        frame. Until this succeeds the link is unauthenticated
 *                        and every other frame type is rejected.
 *
 * Phase 3 (transparent migration) payloads:
 *   GW_MIGRATE_REQUEST : uint32 clientId, uint32 destNode, uint32 charGuid
 *                        node A -> gateway: flip this client's backend to destNode.
 *   GW_SESSION_PREPARE : uint32 clientId, uint32 accountId, uint32 charGuid,
 *                        uint8 locale, uint32 security, uint32 destNode
 *                        gateway -> node B: stage (load) the migrating character.
 *   GW_SESSION_READY   : uint32 clientId, uint8 ok
 *                        node B -> gateway: 1 = staged, 0 = failed.
 *   GW_MIGRATE_ABORT   : uint32 clientId
 *                        gateway -> node A: B failed; un-quiesce the saved session.
 */

#ifndef MANGOS_GATEWAYPROTOCOL_H
#define MANGOS_GATEWAYPROTOCOL_H

#include "Common.h"
#include "ByteBuffer.h"

enum GatewayMsg
{
    GW_SESSION_OPEN    = 1, // gateway -> node: a client authed; open a backend session
    GW_CLIENT_PACKET   = 2, // both ways: a single plaintext game packet for a client
    GW_SESSION_RELEASE = 3, // gateway -> node: a client disconnected; tear the session down
    GW_SESSION_PREPARE = 4, // (reserved, later phase) pre-stage a migrating session on a target node
    GW_SESSION_READY   = 5, // (reserved, later phase) target node signals the session is staged
    GW_MIGRATE_REQUEST = 6, // (reserved, later phase) ask the gateway to flip a client to a new node
    GW_MIGRATE_ABORT   = 7, // (reserved, later phase) cancel an in-flight migration
    GW_HELLO           = 8, // gateway -> node: link authentication (pre-shared secret + protocol version); MUST be the first frame
};

// Protocol version carried in GW_HELLO; bump if the wire format changes.
static const uint32 GW_PROTOCOL_VERSION = 1;

namespace GatewayFrame
{
    static const uint32 HEADER_SIZE = 5;         // uint32 len + uint8 type
    static const uint32 MAX_PAYLOAD = 0x100000;  // 1 MB sanity cap

    // Build a complete on-wire frame (header + payload) into `out`.
    // Mirrors ClusterFrame::Build so both transports share the same layout.
    inline void Build(ByteBuffer& out, uint8 type, ByteBuffer const& payload)
    {
        uint32 len = (uint32)payload.size();
        out << len;
        out << type;
        if (len)
        {
            out.append(payload.contents(), payload.size());
        }
    }
}

#endif // MANGOS_GATEWAYPROTOCOL_H
