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
    CLUSTER_MSG_SOCIAL_STATUS  = 6, // friend/guild online status (Phase 6, reserved)
    CLUSTER_MSG_PLAYER_TRANSFER= 7, // migration hand-off: uint32 guid + serialized player blob (Phase 4)
    CLUSTER_MSG_RELAY_GROUP_CHAT = 8, // relayed party/raid chat (Phase 7): groupId-keyed
    CLUSTER_MSG_GROUP_STATE      = 9, // group roster/leader/state changed; peers re-read DB (Phase 7)
    CLUSTER_MSG_SERVICE_REQUEST  = 10, // Phase 8: a service-role op routed to the role-owner node
    CLUSTER_MSG_SERVICE_RESULT   = 11, // Phase 8: the role-owner's broadcast of a completed op
};

// Logical service roles (Phase 8). A role is owned by a configured node and
// consumed cluster-wide; every role degrades to local handling when its owner
// is unconfigured (0) or offline. Carried as a uint8 inside SERVICE_REQUEST/RESULT.
enum ClusterServiceRole
{
    CLUSTER_SERVICE_ANNOUNCE = 1, // centralized global announcement (the demonstrator)
    // CLUSTER_SERVICE_AUTH   = 2, // (future) centralized auth/name-reservation
    // CLUSTER_SERVICE_CHAT   = 3, // (future) centralized chat fan-out
    // CLUSTER_SERVICE_WARDEN = 4, // (future) centralized Warden signature evaluation
};

// Reasons carried by CLUSTER_MSG_GROUP_STATE. Kept here so the BG layer can add its
// own reasons (BG_QUEUE/BG_INVITE) on top of the same wire type later.
enum ClusterGroupStateReason
{
    CLUSTER_GROUP_STATE_ROSTER  = 0, // membership add/remove
    CLUSTER_GROUP_STATE_LEADER  = 1, // leader changed
    CLUSTER_GROUP_STATE_DISBAND = 2, // group disbanded
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
