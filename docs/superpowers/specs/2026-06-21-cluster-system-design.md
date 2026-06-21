# MaNGOS Zero — Multi-Node Cluster System: Design Specification

**Status:** Draft for team review · **Target:** MaNGOS Zero (vanilla WoW 1.12.1, build 5875)
**Author:** Krill · **Date:** 2026-06-21
**Implementation state:** Phase 0 + Phase 1 complete & boot-tested; Phases 2–3 in progress.

---

## 1. Summary

This proposes a **multi-node world-server cluster** for MaNGOS Zero: several `mangosd`
world nodes serving a single realm, coordinated through a shared registry, an
inter-node message bus, and atomic player hand-off (migration) between nodes.

The design is delivered as **config-gated, OFF-by-default, incrementally-shippable
slices**. With `Cluster.Enable = 0` (the default), every code path is inert and the
server behaves exactly as today's single node. Each phase compiles and boot-tests
on its own, so the cluster can land in `master` progressively without destabilizing
single-node operation.

### 1.1 Goals
- Horizontal scale: distribute players/zones across multiple `mangosd` processes.
- Single-realm illusion: players experience one seamless world (cross-node visibility,
  chat, social, grouping).
- Zero regression when disabled; opt-in per operator.
- Incremental, reviewable phases that each stand alone.

### 1.2 Non-Goals (for this design)
- Cross-*realm* play. Scope is one realm spread across nodes.
- Rewriting the client. Vanilla 1.12.1 client is fixed; we work within its protocol.
- Database sharding. All nodes share one logon DB and one world/characters DB set
  (sharding is a possible later optimization, explicitly out of scope here).

### 1.3 Honest framing
The "recovered" cluster material in `docs/` (AI-chat transcripts) is **stubs** — empty
method bodies and class signatures only. This is therefore a **greenfield design**, not
a port. Where the recovered material is useful is the *shape* (a node table, gated hook
points, a message-type enum); the algorithms are ours to write.

---

## 2. Background: why the current server resists clustering

MaNGOS Zero today is a single `mangosd` world process plus a `realmd` login process.
Three couplings make clustering non-trivial and shape the phase ordering:

| Coupling | Where | Consequence |
|---|---|---|
| Session ↔ Socket | `WorldSession` holds a raw `WorldSocket*` (same-process TCP) | A live session is bound to one process's network reactor; moving it requires either a client reconnect or a TCP relay. |
| Player registry is per-process | `ObjectAccessor::FindPlayer/FindPlayerByName/DoForAllPlayers` iterate a per-node map | Cross-node "find player" (whisper/who/guild) needs a DB lookup or an RPC. |
| In-memory managers | `Group`, `Guild`, `Channel`, `BattleGround` live on the node's heap | Cross-node membership requires DB-backed or message-bus-backed state. |

**Player lifecycle today** (the seams clustering hooks into):
- Login: `WorldSocket::HandleAuthSession` → `WorldSession` → `CMSG_PLAYER_LOGIN` →
  `Player::LoadFromDB` → `Map::Add(Player*)`.
- Logout: `WorldSession::LogoutPlayer` → `Player::SaveToDB` → remove from map → delete.
- Map ownership: `MapManager` holds `MapID → Map*`; a `Player` belongs to exactly one
  `Map` via `MapReference`. `Map::Add(Player*)` is the entry seam.
- Existing inter-process networking: RA (telnet admin) and SOAP only — **no UDP, no
  message queue, no node-to-node socket exists**. We add one.

---

## 3. Architecture overview

```
                         ┌──────────────┐
                         │   realmd     │  routes each login to the least-loaded
                         │  (login)     │  online node (reads cluster_nodes)
                         └──────┬───────┘
                                │ realm address = chosen node host:port
              ┌─────────────────┼─────────────────┐
              ▼                 ▼                  ▼
        ┌──────────┐     ┌──────────┐       ┌──────────┐
        │ node 1   │◄───►│ node 2   │◄─────►│ node 3   │   inter-node message bus
        │ mangosd  │     │ mangosd  │       │ mangosd  │   (TCP, length-framed)
        └────┬─────┘     └────┬─────┘       └────┬─────┘
             └────────────────┴──── shared DBs ──┘
                    realmd (cluster_nodes) · characters · world
```

- **Every node is equal.** There is no dedicated master; `realmd` is the only
  coordinator, and only at login time (route by load). A node failing only loses its
  own players; the rest continue.
- **Shared databases.** All nodes use the same logon/characters/world DBs. The
  `cluster_nodes` table (logon DB) is the source of truth for node liveness and load.
- **Message bus.** Nodes maintain TCP connections to each other for real-time events
  (movement relay, chat, social status, migration hand-off).
- **`ClusterMgr`** is the hub singleton on each node: owns node identity, the registry
  heartbeat, the local player→node map, peer connections, and migration orchestration.

---

## 4. Data model

### 4.1 `cluster_nodes` (logon/realmd DB) — live node registry
```sql
CREATE TABLE IF NOT EXISTS `cluster_nodes` (
  `node_id`        INT UNSIGNED NOT NULL,                      -- Cluster.NodeID
  `host`           VARCHAR(64)  NOT NULL DEFAULT '127.0.0.1',  -- address clients/peers reach this node
  `port`           INT UNSIGNED NOT NULL DEFAULT 0,            -- world port (advertised to clients)
  `capacity`       INT UNSIGNED NOT NULL DEFAULT 0,            -- max players this node accepts
  `player_count`   INT UNSIGNED NOT NULL DEFAULT 0,            -- current load (for routing)
  `last_heartbeat` INT UNSIGNED NOT NULL DEFAULT 0,            -- unix seconds
  `status`         ENUM('online','offline','draining') NOT NULL DEFAULT 'offline',
  PRIMARY KEY (`node_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
```
- `realmd` selects routing target: `WHERE status='online' ORDER BY player_count ASC LIMIT 1`.
- A node missing heartbeats for `3 × HeartbeatSeconds` is flipped to `offline` by any
  live node's maintenance tick. `draining` = accepting no new players, used for graceful
  rolling restarts.

### 4.2 `characters.cluster_node` (characters DB) — per-character assignment
```sql
ALTER TABLE `characters`
  ADD COLUMN `cluster_node` INT UNSIGNED NOT NULL DEFAULT 0;  -- 0 = unassigned (single-node)
```
- Set when a character is migrated/assigned, read by `realmd` so a reconnecting client
  lands on the node that holds its state.

### 4.3 Inter-node message protocol
Length-framed binary over TCP:
```
[ uint32 length ][ uint8 type ][ payload (ByteBuffer) ]
```
Message types (`ClusterMessage.h`):
| Type | Purpose |
|---|---|
| `HEARTBEAT` | peer liveness / RTT |
| `PLAYER_ENTER_NODE` / `PLAYER_LEAVE_NODE` | global presence updates |
| `RELAY_MOVEMENT` | broadcast a mover's `MovementInfo` to nodes with nearby observers |
| `RELAY_CHAT` | cross-node chat/whisper/guild/channel |
| `SOCIAL_STATUS` | friend online/offline propagation |
| `PLAYER_TRANSFER` | migration payload (serialized player + SHA1) |
| `PLAYER_FIND_BY_NAME` / `PLAYER_FOUND_ACK` | cross-node name lookup (whisper/who) |

---

## 5. `ClusterMgr` — the per-node hub

Singleton (`#define sClusterMgr`), mirroring the established `AntiCheatMgr` pattern.

**Implemented (Phase 0–1):**
```cpp
void   Init();                 // read config; register node row if enabled
void   LoadConfig();           // re-read on .reload config
void   Update(uint32 diff);    // heartbeat + stale-peer sweep (WUPDATE_CLUSTER timer)
void   Shutdown();             // flag this node offline (synchronous DB write)

bool   IsEnabled();  uint32 GetNodeId();  uint32 GetPort();
uint32 GetHeartbeatSeconds();  std::string const& GetHost();

void   RegisterPlayer(uint32 guidLow);     // player entered this node (Map::Add hook)
void   UnregisterPlayer(uint32 guidLow);   // player left (LogoutPlayer hook)
uint32 GetNodeForPlayer(uint32 guidLow);   // owning node, 0 if unknown
uint32 GetLocalPlayerCount();
uint32 GetOptimalNode();                   // least-loaded online node (login routing)
void   RelayMovement(Player*, uint16 opcode, MovementInfo const&);  // no-op until Phase 2
```

**Planned (Phase 2+):** peer connection table, `ClusterSocket`/network thread, message
encode/dispatch, `MigratePlayer`, cross-node social/group dispatch.

---

## 6. Configuration (all gated under `Cluster.*`, OFF by default)

| Key | Default | Meaning |
|---|---|---|
| `Cluster.Enable` | `0` | master switch; 0 = pure single node |
| `Cluster.NodeID` | `1` | unique node id (1–255) |
| `Cluster.Host` | `127.0.0.1` | address peers/clients use to reach this node |
| `Cluster.Port` | `0` | world port advertised in the registry |
| `Cluster.HeartbeatSeconds` | `30` | liveness heartbeat interval (5–3600) |
| `Cluster.PeerPort` | `8086` | inter-node message-bus listen port *(Phase 2)* |
| `Cluster.EnableMigration` | `0` | allow player hand-off *(Phase 4)* |
| `Cluster.MigrationCombatDelay` | `3` | seconds to retry migration while in combat *(Phase 4)* |

---

## 7. Phase plan

Each phase: config-gated, compiles, boot-tests, and is independently reviewable.

### Phase 0 — Node identity + registry **[DONE, boot-tested]**
- `ClusterMgr` registers this node in `cluster_nodes`, heartbeats every
  `HeartbeatSeconds`, flips stale peers `offline`, flags itself `offline` on shutdown.
- `cluster_nodes` table + `characters.cluster_node` column.
- **Acceptance:** with `Enable=1`, the node row appears `online`, `last_heartbeat`
  advances each tick, and a clean shutdown sets `offline`. With `Enable=0`, no DB paths
  run. ✅ Verified.

### Phase 1 — Player-registry hooks **[DONE]**
- `RegisterPlayer`/`UnregisterPlayer`/`GetNodeForPlayer`/`GetOptimalNode` + gated hooks
  at `Map::Add(Player*)`, `WorldSession::LogoutPlayer`, and a no-op `RelayMovement` on
  the movement broadcast.
- **Acceptance:** compiles; zero behavior change when disabled. ✅

### Phase 2 — Inter-node transport **[IN PROGRESS]**
- `ClusterSocket`/network layer + `ClusterMessage` framing; peer connections discovered
  from `cluster_nodes`; `HEARTBEAT` exchange; `RelayMovement` fan-out implemented.
- **Open decision (panel in flight):** ACE-reactor-integrated socket vs a dedicated
  cluster-network thread. Thread-safety is the crux — movement runs on map threads, so
  sends must be queued, never inline socket writes; received game-state changes must be
  marshalled to the world thread.
- **Acceptance:** two nodes connect and exchange heartbeats; no game-behavior change
  (no cross-node players yet).

### Phase 3 — Player-state serialization **[IN PROGRESS]**
- `Player::SerializeForMigration(ByteBuffer&)` / `DeserializeFromMigration(...)`
  covering the full live state (identity/position, inventory + item instance data,
  spells, durable auras, quests, skills, reputation, action bars, homebind, group/guild
  keys), with a version header and trailing **SHA1** for integrity.
- `.cluster selftest` GM command: serialize → deserialize → field-compare → PASS/FAIL.
- **Open decision (panel in flight):** DB-parity format (mirror `SaveToDB`) vs an
  explicit versioned/section format (forward-compatible).
- **Acceptance:** round-trip selftest passes for a fully-geared character offline.

### Phase 4 — GM-triggered migration **[HIGH RISK]**
- `Player::MigrateToNode(nodeId)`: `CanMigrate()` gate (not in combat/falling/teleporting)
  → freeze (`SetClientControl(false)`) → `SaveToDB` → serialize → `PLAYER_TRANSFER` to
  target → mark `characters.cluster_node` → disconnect client.
- Target node validates SHA1, loads state; client reconnects and `realmd` routes it to
  the target node (reading `cluster_node`).
- **Migration model decision (the fundamental fork):** vanilla clients cannot
  transparently reconnect. Two options:
  1. **Disconnect-reconnect (recommended first):** brief, visible reconnect; simplest;
     no new infrastructure.
  2. **TCP relay/proxy:** a front proxy keeps the client socket alive while the game
     session moves; transparent but requires a relay process and session hand-off.
- `.cluster migrate <player> <node>`; gated `Cluster.EnableMigration=0`.
- **Acceptance:** a GM moves a player between two nodes; character arrives intact
  (selftest-equivalent), no item/gold loss.

### Phase 5 — Automatic migration at zone boundaries **[HIGH RISK]**
- Zone→node assignment (static table or load-based) checked on `TeleportTo`/`Map::Add`;
  triggers `MigrateToNode`. Freeze + post-migration immunity buff as UX polish.
- **Acceptance:** crossing a configured boundary auto-migrates with no manual step.

### Phase 6 — Cross-node social (whisper/guild/who/friends) **[VERY HIGH RISK]**
- `SocialMgr::SendFriendStatus` becomes cluster-aware (route `SOCIAL_STATUS` to the
  friend's node). `FindPlayerByName` falls back to `PLAYER_FIND_BY_NAME` broadcast +
  `PLAYER_FOUND_ACK`. Guild/Channel chat dispatched via `RELAY_CHAT`.
- **Acceptance:** whisper/guild-chat/who work across nodes within a latency budget.

### Phase 7 — Cross-node Group & BattleGround **[VERY HIGH RISK]**
- Group state DB-backed and read on demand; loot/roll coordinated cross-node. BG queue
  run by a coordinator node dispatching invites cluster-wide.
- **Acceptance:** a cross-node party can form, share loot, and queue/enter a BG.

### Phase summary
| Phase | Deliverable | Risk | State |
|---|---|---|---|
| 0 | Node identity + heartbeat | Low | ✅ done |
| 1 | Player-registry hooks | Low | ✅ done |
| 2 | Inter-node transport | Medium | 🔄 in progress |
| 3 | State serialization + SHA1 | Medium | 🔄 in progress |
| 4 | GM migration | High | planned |
| 5 | Auto-migration | High | planned |
| 6 | Cross-node social | Very High | planned |
| 7 | Cross-node Group/BG | Very High | planned |

---

## 8. Key design decisions & rationale

1. **Equal nodes, realmd-only coordination.** No master node = no single point of
   failure for the world; routing complexity stays at the (already-central) login layer.
2. **Shared DB as source of truth.** Avoids a distributed-consensus layer; node liveness
   and load live in `cluster_nodes`. Trade-off: the DB is a shared dependency (already
   true today).
3. **Atomic freeze-then-transfer migration.** Freeze the player, serialize a consistent
   snapshot with a SHA1, hand off, then unfreeze — avoids mid-flight state corruption.
4. **Observe/queue, never block on map threads.** All inter-node sends from gameplay
   code are enqueued; the network layer does I/O. Inbound messages that touch game state
   are marshalled onto the world thread. This mirrors how the anti-cheat desync detector
   already defers cross-thread work.
5. **Disconnect-reconnect migration first.** Honest about the vanilla client constraint;
   ship the simple model, evaluate a relay later if the reconnect blip is unacceptable.
6. **Everything gated, OFF by default.** Same discipline as the anti-cheat framework:
   `Cluster.Enable=0` ⇒ inert; no risk to single-node operators.

---

## 9. Risks & open questions for the team

- **Migration model:** disconnect-reconnect vs TCP relay — needs a team decision before
  Phase 4 (affects infra and UX).
- **Transport threading:** reactor-integrated vs dedicated thread (panel deciding;
  thread-safety is the real constraint).
- **Serialization format:** DB-parity vs versioned-sections (forward compatibility vs
  simplicity).
- **Cross-node consistency** for Group/Guild/BG (Phase 6–7) is the hardest part; may
  warrant a designated coordinator node per subsystem rather than full peer-to-peer.
- **Realmd routing** must learn to emit per-node `host:port` from `cluster_nodes`
  instead of a static realm address — small but required for Phase 4.
- **DB load** from heartbeats/registry at scale — currently negligible (one UPDATE per
  node per interval), revisit if node count grows large.

---

## 10. Engineering notes (for contributors)

- **Shutdown-time DB writes must be synchronous** (`DirectPExecute`); async `PExecute`
  is dropped when the DB delay threads halt during shutdown.
- **The real clean-shutdown path is in `src/mangosd/WorldThread.cpp`** (after the final
  `UpdateSessions`, DB still alive). `World::CleanupsBeforeStop()` is dead code — do not
  hook it.
- **Build:** new files in a new dir need a `cmake -S . -B build` reconfigure (GLOB).
  Use `--parallel 4`, not bare `--parallel` (16-way OOMs the compiler on playerbot TUs).
- **Hooks are additive and gated**, exactly like the anti-cheat integration: they sit
  beside existing logic and do nothing when `Cluster.Enable=0`.
```
