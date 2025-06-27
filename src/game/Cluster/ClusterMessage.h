#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <stdexcept>

enum ClusterMessageType : uint8_t {
    CLUSTER_MSG_HEARTBEAT = 0,
    CLUSTER_MSG_CHAT,
    CLUSTER_MSG_PLAYER_MIGRATION,
    CLUSTER_MSG_NODE_STATUS,
    CLUSTER_MSG_WORLD_STATE
};

struct ClusterMessage {
    uint8_t type;
    std::string sourceNode;
    std::string targetNode;
    std::vector<uint8_t> payload;

    std::vector<uint8_t> Serialize() const;
    static ClusterMessage Deserialize(const uint8_t* data, size_t size);
};