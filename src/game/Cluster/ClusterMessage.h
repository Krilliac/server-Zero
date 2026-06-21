/*
 * Multi-node Cluster framework — inter-node message types + wire framing (Phase 2).
 *
 * Wire frame (little-endian, via ByteBuffer): [uint32 payloadLen][uint8 type][payload].
 * payloadLen counts only the payload bytes that follow the type byte.
 */

#ifndef MANGOS_CLUSTERMESSAGE_H
#define MANGOS_CLUSTERMESSAGE_H

#include "Common.h"
#include "ByteBuffer.h"

enum ClusterMessageType
{
    CLUSTER_MSG_HEARTBEAT      = 1, // peer liveness ping (payload: uint32 nodeId)
    CLUSTER_MSG_PLAYER_ENTER   = 2, // a player became owned by the sending node
    CLUSTER_MSG_PLAYER_LEAVE   = 3, // a player left the sending node
    CLUSTER_MSG_RELAY_MOVEMENT = 4, // relayed movement packet (guid+opcode+MovementInfo)
    CLUSTER_MSG_RELAY_CHAT     = 5, // relayed chat: whisper/guild/officer/channel (Phase 6)
    CLUSTER_MSG_SOCIAL_STATUS  = 6, // friend/guild online status (Phase 6)
    CLUSTER_MSG_PLAYER_TRANSFER= 7, // migration hand-off: uint32 guid + serialized player blob (Phase 4)
};

namespace ClusterFrame
{
    static const uint32 HEADER_SIZE = 5;         // uint32 len + uint8 type
    static const uint32 MAX_PAYLOAD = 0x100000;  // 1 MB sanity cap

    // Build a complete on-wire frame (header + payload) into `out`.
    inline void Build(ByteBuffer& out, uint8 type, ByteBuffer const& payload)
    {
        uint32 len = (uint32)payload.size();
        out << len;
        out << type;
        if (len)
            out.append(payload.contents(), payload.size());
    }
}

#endif // MANGOS_CLUSTERMESSAGE_H
