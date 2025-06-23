#pragma once

// Node IDs
#define MAIN_WORLD_NODE 1
#define INSTANCE_NODE_1 100
#define INSTANCE_NODE_2 101

// Packet opcodes
#define SMSG_TRANSFER_PENDING 0x7000

// Error codes
enum ClusterErrors {
    CLUSTER_ERROR_NONE,
    CLUSTER_ERROR_FULL,
    CLUSTER_ERROR_TIMEOUT
};
