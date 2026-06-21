/*
 * Multi-node Cluster framework — central node manager.
 * Phase 0: node identity + a shared DB registry with periodic heartbeats.
 *
 * Config-gated and OFF by default (Cluster.Enable = 0): when disabled this
 * manager does nothing and the server behaves exactly as a single node. When
 * enabled, the node registers itself in the `cluster_nodes` table (login DB) and
 * keeps a liveness heartbeat so other nodes and realmd can discover live peers.
 *
 * Later phases add inter-node transport, player-state serialization, migration
 * and cross-node social/group/BG — this manager is the hub they build on.
 */

#ifndef MANGOS_CLUSTERMGR_H
#define MANGOS_CLUSTERMGR_H

#include "Common.h"

#include <string>
#include <map>
#include <mutex>

class Player;
class MovementInfo;

class ClusterMgr
{
    public:
        static ClusterMgr* instance()
        {
            static ClusterMgr inst;
            return &inst;
        }

        // World startup: read config and, if enabled, register this node.
        void Init();
        // Re-read config (on .reload config). Safe to call repeatedly.
        void LoadConfig();
        // Heartbeat tick, driven from World::Update via WUPDATE_CLUSTER.
        void Update(uint32 diff);
        // Clean shutdown: flag this node offline so peers stop routing to it.
        void Shutdown();

        bool   IsEnabled() const { return m_enabled; }
        uint32 GetNodeId() const { return m_nodeId; }
        uint32 GetPort() const { return m_port; }
        uint32 GetHeartbeatSeconds() const { return m_heartbeatSec; }
        std::string const& GetHost() const { return m_host; }

        // --- Phase 1: per-node player registry + routing helpers ---
        // Records which players this node currently owns. No inter-node transport
        // yet (Phase 2), so these only track local ownership and read the shared
        // registry; all are inert when the framework is disabled.
        void   RegisterPlayer(uint32 guidLow);    // player entered this node
        void   UnregisterPlayer(uint32 guidLow);  // player left this node
        uint32 GetNodeForPlayer(uint32 guidLow) const; // node owning a guid, 0 if unknown
        uint32 GetLocalPlayerCount() const;       // players currently owned by this node
        uint32 GetOptimalNode();                  // least-loaded online node (login routing)

        // Phase 1: gated no-op. Phase 2 serializes + sends the move to peer nodes
        // so players on other nodes can see this mover.
        void   RelayMovement(Player* mover, uint16 opcode, MovementInfo const& mi);

    private:
        ClusterMgr();
        ClusterMgr(ClusterMgr const&);
        ClusterMgr& operator=(ClusterMgr const&);

        void RegisterNode();     // upsert this node's row, status='online'
        void Heartbeat();        // refresh last_heartbeat + player_count
        void MarkStaleOffline(); // flip peers that missed heartbeats to 'offline'

        bool        m_enabled;
        uint32      m_nodeId;
        uint32      m_port;
        uint32      m_capacity;
        uint32      m_heartbeatSec;
        std::string m_host;

        // guidLow -> owning nodeId for players currently on this node. Touched from
        // map threads (RegisterPlayer) and the main thread (Unregister), so guarded.
        mutable std::mutex       m_playerLock;
        std::map<uint32, uint32> m_playerNodes;
};

#define sClusterMgr ClusterMgr::instance()

#endif // MANGOS_CLUSTERMGR_H
