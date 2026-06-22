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
#include <deque>
#include <vector>
#include <mutex>

class Player;
class MovementInfo;
class ByteBuffer;
class ClusterThread;

// A live peer node this node may connect to (Phase 2 transport).
struct ClusterPeer
{
    uint32      nodeId;
    std::string host;
    uint32      port;   // inter-node (peer) port
};

// A queued outbound frame. target==0 means broadcast to all connected peers.
struct ClusterOutFrame
{
    uint32             target;
    std::vector<uint8> bytes;
};

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

        // Phase 2: serialize the move and enqueue it for broadcast to peer nodes
        // so players on other nodes can see this mover. Enqueue-only (safe to call
        // from map threads); the network thread does the actual send.
        void   RelayMovement(Player* mover, uint16 opcode, MovementInfo const& mi);

        bool   IsMigrationEnabled() const { return m_migrationEnabled; }
        void   SetMigrationEnabled(bool on) { m_migrationEnabled = on; } // runtime toggle

        // --- Phase 7b: cross-node battleground queue (shared-DB-centric) ---
        // Gated on IsEnabled() && IsMigrationEnabled() && m_crossNodeBG; every method
        // below early-returns when off, so single-node BG is unaffected.
        bool   IsCrossNodeBG() const { return m_enabled && m_migrationEnabled && m_crossNodeBG; }
        // Write/refresh this player's row in cluster_bg_queue (on BG-queue join).
        void   PublishBgQueueJoin(uint32 guidLow, std::string const& name, uint32 team,
                                  uint32 bgTypeId, uint32 bracketId, bool asGroup,
                                  uint32 groupId, uint32 level);
        void   RemoveBgQueueEntry(uint32 guidLow);   // delete this player's row (leave/port-in)
        void   ClearOwnBgQueueEntries();             // delete every row this node owns (shutdown)
        // Phase 4: hand a serialized player blob to a specific target node (directed).
        void   SendPlayerTransfer(uint32 targetNode, uint32 guidLow, ByteBuffer const& blob);

        // Phase 6: relay a chat message to peer nodes; the node(s) hosting the
        // recipients deliver it. Used for cross-node whisper, guild/officer and
        // channel chat. destId = guild id for guild/officer chat (0 otherwise);
        // team = sender's Team for channel-manager selection (0 otherwise). For
        // whisper, toName is the target player name; for channel, toName is the
        // channel name; for guild, toName is unused.
        void   SendChatRelay(uint8 chatType, uint32 lang, uint64 fromGuid, uint8 fromTag,
                             std::string const& fromName, std::string const& toName,
                             std::string const& text, uint32 destId = 0, uint32 team = 0);

        // Phase 7: relay a PARTY/RAID/RAID_LEADER/RAID_WARNING message to peers; the
        // node(s) hosting that group's members deliver it locally. groupId is the
        // persistent group id (Group::GetId()); subGroup scopes plain party chat in a
        // raid (the sender's sub-group), or -1 for the whole group.
        void   SendGroupChatRelay(uint8 chatType, uint32 lang, uint32 groupId,
                                  uint64 fromGuid, uint8 fromTag, std::string const& fromName,
                                  std::string const& text, int32 subGroup = -1);

        // Phase 7: announce that a group's roster/leader/state changed so peers can
        // re-read the shared DB and refresh their local members. reason is a
        // ClusterGroupStateReason. Minimal, DB-backed; no phantom Players on the wire.
        void   SendGroupStateChange(uint32 groupId, uint8 reason);

        // --- Phase 8: service-role decomposition + graceful fallback ---
        // A logical service (role) is owned by a configured node and consumed
        // cluster-wide. Every role degrades to LOCAL handling when the cluster is
        // disabled, the role is unconfigured (owner 0), or the owner is offline.
        uint32 GetServiceOwner(uint8 role) const;        // configured owner node, 0 = unconfigured/local
        bool   IsServiceOwnerOnline(uint32 owner) const; // owner present+online in cluster_nodes
        // Route a global announcement through the ANNOUNCE role. Returns true if it
        // was routed to a live remote owner; false means it was handled LOCALLY here
        // (the graceful-degradation fallback), which this method already performed.
        bool   RouteAnnounce(std::string const& text);
        // Perform the authoritative announcement locally (deliver to this node's
        // players). Used both on the owner and on every fallback path.
        void   DoLocalAnnounce(std::string const& text) const;

        // Drain queued inbound peer messages. Called every world tick (cheap when
        // idle) so chat/migration relays have low latency, not the 30s heartbeat tick.
        void   ProcessNetwork();

        // Phase 5: zone-affinity auto-migration.
        bool   AutoMigrateEnabled() const { return m_autoMigrate; }
        void   SetAutoMigrate(bool on) { m_autoMigrate = on; }          // runtime toggle

        bool   AutoFailoverEnabled() const { return m_autoFailover; }
        void   SetAutoFailover(bool on) { m_autoFailover = on; }        // runtime toggle
        uint32 GetNodeForZone(uint32 zoneId) const;  // node owning a zone, 0 if unassigned
        void   ReloadZoneMap() { LoadZoneMap(); }    // re-read cluster_zone_assignment live

        // Visual debug: per-node spell-visual indicator + event bursts. The master
        // flag is runtime-toggleable (.cluster visual) independent of the config.
        bool   VisualDebugEnabled() const { return m_visualDebug; }
        void   SetVisualDebug(bool on) { m_visualDebug = on; }
        uint32 GetVisualIntervalMs() const { return m_visualIntervalMs; }
        uint32 GetNodeVisualKit(uint32 nodeId) const;  // per-node indicator SpellVisualKit
        uint32 GetMigrateVisualKit() const;            // migration-event SpellVisualKit

        // Chat node-tag: prefix outgoing chat with "[N<id>] " for cluster debugging.
        bool   ChatTagEnabled() const { return m_chatTag; }
        void   SetChatTag(bool on) { m_chatTag = on; }
        void   TagChatMessage(std::string& msg) const; // prepend "[N<id>] " if enabled

        // --- Phase 2: inter-node transport bridge (called by ClusterThread) ---
        bool PopOutbound(std::vector<ClusterOutFrame>& out);          // net thread: drain send queue
        void GetPeers(std::vector<ClusterPeer>& out);                 // net thread: current peer list
        void PushInbound(uint8 type, const uint8* data, uint32 len);  // net thread: queue an rx frame
        void OnPeerHeartbeat(uint32 nodeId);                          // net thread: note peer liveness

    private:
        ClusterMgr();
        ClusterMgr(ClusterMgr const&);
        ClusterMgr& operator=(ClusterMgr const&);

        void RegisterNode();     // upsert this node's row, status='online'
        void LoadZoneMap();      // load cluster_zone_assignment into m_zoneMap
        void Heartbeat();        // refresh last_heartbeat + player_count
        void MarkStaleOffline(); // flip peers that missed heartbeats to 'offline'

        // --- Automatic failover (coordinator-driven; gated by Cluster.AutoFailover) ---
        // The coordinator = the lowest node_id currently online; only it heals, to
        // avoid races. Each tick it sweeps EVERY assignment owned by a non-online node
        // and reassigns it to a survivor, so a coordinator that dies mid-heal is
        // finished by the next coordinator (self-healing, idempotent).
        uint32 GetCoordinatorNode();                 // lowest online node_id (0 if none)
        bool   IsCoordinator() { return GetCoordinatorNode() == m_nodeId; }
        void   GetOnlineSurvivors(std::vector<uint32>& out); // online node ids, ascending
        void   RunFailoverSweep();                   // reassign offline-owned zones/affinities

        // Phase 7b matchmaker (coordinator-only): read cluster_bg_queue, form matches
        // per (bg_type_id,bracket_id) at the template min-per-team, pick a host via
        // GetOptimalNode(), and mark chosen rows status='matched',host_node=host.
        void   RunBgMatchmaker();
        // Phase 7b convergence (every node): migrate matched local players whose host
        // is a different node to the host (reuses Player::MigrateToNode).
        void   RunBgConvergence();

        void RefreshPeers();     // world thread: rebuild m_peers from cluster_nodes
        void DrainInbound();     // world thread: process queued inbound frames
        void LoadServiceConfig(); // Phase 8: read Cluster.Service.* role owners
        void EnqueueBroadcast(ByteBuffer const& frame);              // queue a wire frame for all peers
        void EnqueueDirected(uint32 target, ByteBuffer const& frame); // queue a wire frame for one peer

        bool        m_enabled;
        bool        m_migrationEnabled;
        bool        m_autoMigrate;
        bool        m_autoFailover; // Cluster.AutoFailover: heal cluster when a peer dies
        bool        m_visualDebug;
        bool        m_chatTag;
        bool        m_crossNodeBG; // Phase 7b: cross-node BG queue master gate
        uint32      m_visualIntervalMs;
        uint32      m_svcAnnounceNode; // Phase 8: owner of CLUSTER_SERVICE_ANNOUNCE (0=local)
        uint32      m_nodeId;
        uint32      m_port;
        uint32      m_peerPort;  // inter-node listen/connect port
        uint32      m_capacity;
        uint32      m_heartbeatSec;
        std::string m_host;

        ClusterThread* m_net;    // dedicated network thread (NULL when disabled)

        // guidLow -> owning nodeId for players currently on this node. Touched from
        // map threads (RegisterPlayer) and the main thread (Unregister), so guarded.
        mutable std::mutex       m_playerLock;
        std::map<uint32, uint32> m_playerNodes;

        // Phase 2 transport queues/state, shared with the network thread.
        std::mutex                       m_outLock;
        std::deque<ClusterOutFrame>      m_outQueue;   // frames awaiting send
        std::mutex                       m_inLock;
        std::deque<std::vector<uint8> >  m_inQueue;    // each = [type][payload], processed on world thread
        std::mutex                       m_peerLock;
        std::vector<ClusterPeer>         m_peers;      // current online peers
        std::map<uint32, uint32>         m_peerLastSeen; // nodeId -> last heartbeat (ms)

        // Phase 5: zone -> owning node, loaded from cluster_zone_assignment.
        mutable std::mutex       m_zoneLock;
        std::map<uint32, uint32> m_zoneMap;
};

#define sClusterMgr ClusterMgr::instance()

#endif // MANGOS_CLUSTERMGR_H
