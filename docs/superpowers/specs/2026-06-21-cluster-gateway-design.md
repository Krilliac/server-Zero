# Cluster Connection Gateway — Design Spec

**Date:** 2026-06-21
**Status:** Approved (brainstorming), pending implementation
**Depends on:** the existing cluster framework (Phases 0–8 + failover + Phase 7b), the
shared library (`src/shared`), and realmd as a binary template.

## 1. Purpose

Make cross-node player transfer **transparent** — no relogin, no return to character
select. Today the cluster uses a disconnect-reconnect model (visible reconnect blip on
migration). This adds a **connection gateway**: a front-end process that owns the client
connection and swaps the backend world node behind it, so a boundary crossing is at worst
a loading screen (Blizzard's own handoff model) and optionally fully seamless.

**Hard requirement:** smooth transfer. **Config-driven:** the operator selects
loading-screen vs truly-seamless resume per deployment.

## 2. Why a gateway (the load-bearing constraint)

The world packet header cipher (`src/shared/Auth/AuthCrypt`) is a **stateful ARC4 stream**:
`_send_i/_send_j` and `_recv_i/_recv_j` advance per byte, continuously across every packet
(4 crypted bytes server→client, 6 client→server). You therefore **cannot** hand a
mid-stream encrypted connection to a different backend — the keystream desyncs instantly.

So the gateway must **terminate encryption**: it owns the single `AuthCrypt` for each
client, decrypts client traffic to plaintext, forwards plaintext to the owning node, and
encrypts node replies back to the client. On migration the client-facing cipher stream is
never interrupted — only the plaintext forward-target changes. This is the only design that
supports a seamless swap, and it matches Blizzard's edge-proxy architecture.

(Rejected alternative: a "dumb" TCP proxy that transfers live cipher counters between nodes
on migration — extremely fragile, one byte of misalignment corrupts the stream, and the
proxy can't read opcodes to make routing decisions. The stateful cipher makes this a trap.)

## 3. Topology

Four components:

1. **realmd** (unchanged logic) — logon/SRP6. Its realmlist now advertises **one realm**
   (the gateway's address) instead of one-per-node. It still writes `account.sessionkey`.
2. **gateway** (NEW binary; links `shared`; modeled on realmd) — owns client TCP + the
   `AuthCrypt` stream; performs the world auth handshake; maps each client to a backend
   node; forwards plaintext both ways; orchestrates transfers.
3. **world nodes** (mangosd + a new "gateway session" intake) — accept pre-authed plaintext
   sessions from the gateway over an internal protocol; **no per-node client crypto** for
   gateway-fronted sessions; otherwise fully authoritative for game logic.
4. **shared DB + existing cluster bus** — zone/character affinity, the Phase 4 migration
   hand-off, and the inter-node TCP already built.

Clients only ever see the gateway: one realm, one persistent connection. **Single-node
deployments skip the gateway** and run mangosd directly, exactly as today (the node-side
intake is additive and gated).

## 4. Connection lifecycle (login → in-world)

1. **Connect + auth (at the gateway).** Client connects to the gateway world port. Gateway
   sends `SMSG_AUTH_CHALLENGE` (server seed). On `CMSG_AUTH_SESSION`, the gateway validates
   exactly as `WorldSocket::HandleAuthSession` does: read `account.sessionkey` (K) from the
   login DB, compute `SHA1(account + 0000 + clientSeed + serverSeed + K)`, compare digest.
   On success it sets up its `AuthCrypt` (`SetKey(K,40); Init()`). Auth-level checks (build,
   ban, IP-lock) run here. No node involved yet.
2. **Char-enum via a pre-world node.** No owning node exists pre-world. Gateway picks a
   pre-world node (least-loaded, or whichever owns most of the account's chars), opens a
   plain authed session there, and **forwards** `CMSG_CHAR_ENUM` / create / delete / rename
   to it. The node runs the real DB handlers and replies through the gateway. No char-logic
   duplication in the gateway.
3. **Player-login routed to the owning node.** On `CMSG_PLAYER_LOGIN(guid)`, the gateway
   does ONE routing lookup — `cluster_character_node` affinity (fallback
   `characters.zone` → `cluster_zone_assignment`) — and routes the login to that node,
   dropping the player-less pre-world session if the node differs. This routing-table lookup
   is the gateway's only "game knowledge."
4. **In-world.** The owning node loads the character and streams login packets out through
   the gateway; thereafter it is plaintext piping C↔node. All existing cluster behavior
   keeps working; boundary crossings trigger the §5 handshake.
5. **Logout / disconnect.** Clean logout → gateway sends `SESSION_RELEASE`, node saves
   normally. **Node crash** is the one inherently non-transparent case: gateway detects the
   dropped internal link and disconnects the client; reconnect lands on a survivor (failover
   already cleared the dead node's affinities). The gateway makes *planned* migrations
   transparent; a crash still costs unsaved progress, as today.

## 5. Migration handshake (the core)

Setup: player **P** in-world on **node A**; gateway **G** forwards client **C**'s plaintext
to A. P crosses into a zone/BG/instance owned by **node B**.

Two boundary kinds (differ only in resume):
- **Natural boundary** (continent boat/portal, dungeon/BG/instance entry): the client already
  triggers a teleport + loading screen; migration routes the post-teleport session to B.
- **Open-world zone crossing** (same map, different node): resume is **seamless** (B silently
  resumes updates) or **loading-screen** (B forces a same-map teleport for a clean reset) —
  the config switch.

Sequence (new internal gateway↔node control protocol, alongside the tunneled plaintext):

1. **Trigger (A).** A's existing logic fires (`Player::UpdateZone` → cross-node zone, or BG
   convergence). Gate on Phase 4 `CanMigrate` (not in combat / casting / on taxi; alive).
   A computes `destNode` from `cluster_zone_assignment`.
2. **A → G: `MIGRATE_REQUEST {clientId, destNode=B, charGuid, mode}`.** A quiesces P (stops
   world updates) and `SaveToDB` (+ Phase 4 SHA1 blob).
3. **G buffers** further client→server packets for C and marks C "migrating."
4. **G → B: `SESSION_PREPARE {clientId, accountId, charGuid, locale, mode}`.** B creates a
   pre-authed gateway session (no crypto), loads P from the shared DB, validates the blob.
5. **B → G: `SESSION_READY {ok}`.**
6. **G: atomic switch** (per-client lock) — forward-target C := B. **G → A:
   `SESSION_RELEASE`** (A drops the inert, already-saved session). **G replays** buffered
   packets to B, resumes live forwarding C↔B.
7. **Resume on B:**
   - *Seamless:* B re-sends P's own object + nearby objects and continues the
     movement/update stream at the current position — no world-change packet.
   - *Loading-screen:* B sends `SMSG_TRANSFER_PENDING` + `SMSG_NEW_WORLD(currentMap, pos)` →
     client loading screen → `MSG_MOVE_WORLDPORT_ACK` → B adds P to the map.
   - *Natural boundary:* B handles the WorldPort on the new map as a normal teleport.

**Guarantees:**
- **No relogin / no char-select** — C's socket + `AuthCrypt` are never touched; only the
  backend changes.
- **No lost packets** — gateway buffers the in-flight window and replays it (FIFO).
- **Exactly one authority** — A saves before B loads; A releases only after the switch
  commits. Briefly both hold a session, but A's is inert and the gateway's forward-target is
  the single source of truth.
- **Crash-safe** — B fails/times out → `MIGRATE_ABORT`, A re-activates the saved session,
  buffered packets replay to A, P stays put. A dies after release → P already committed on
  shared DB + live on B.

## 6. Internal gateway↔node protocol

A framed TCP protocol on a dedicated port (per node), distinct from the existing
inter-node cluster bus. Two channels multiplexed by message type:
- **Control:** `SESSION_OPEN` (pre-world authed session), `SESSION_PREPARE` (load a char),
  `SESSION_READY`, `SESSION_RELEASE`, `MIGRATE_REQUEST` (node→gateway), `MIGRATE_ABORT`,
  `CLIENT_DISCONNECT`, heartbeats.
- **Tunnel:** `CLIENT_PACKET {clientId, plaintext opcode+payload}` in both directions.

Framing reuses `ByteBuffer`/`WorldPacket` from `shared`. `clientId` is a gateway-assigned
per-connection id. The node maps `clientId` → its `WorldSession`; the gateway maps
`clientId` → (client socket, crypt, current node).

## 7. Node-side changes (mangosd)

- A `GatewaySocket`-style intake (new): accepts the internal protocol; for each
  `SESSION_OPEN`/`PREPARE`, creates a `WorldSession` whose "socket" is a **plaintext
  gateway pipe** (no `AuthCrypt`) — outgoing packets are framed as `CLIENT_PACKET` to the
  gateway instead of encrypted-to-client.
- Migration trigger change: where Phase 4/5 today calls `MigrateToNode` (kick), a
  gateway-fronted session instead emits `MIGRATE_REQUEST` to the gateway and quiesces.
- All gated: a node in single-node mode (no gateway configured) behaves exactly as today.

## 8. Phasing (each independently testable)

1. **Gateway → single fixed node.** Auth handshake + `AuthCrypt` stream + node-side
   plaintext-session intake. Proves the protocol plumbing.
2. **Affinity routing at login.** Two nodes; char affinity → correct node.
3. **Loading-screen migration.** Walk across a `cluster_zone_assignment` boundary; transparent
   loading-screen handoff.
4. **Seamless migration.** Same crossing; no loading screen.

## 9. Testing

Mirrors the existing approach (command/log-driven automation + a real client for visual
moments):
- **Synthetic test client** (new, automatable): speaks enough protocol to connect, complete
  the auth handshake (computes the digest with a known session key seeded into the login
  DB), and exchange tunneled packets — exercises the gateway end-to-end with NO WoW client.
- Phase 1: synthetic client authenticates through the gateway and reaches a node; assert
  node sees an authed session and the gateway pipes a round-trip packet.
- Phase 2: seed `cluster_character_node`; assert login lands on the right node.
- Phase 3/4: drive a bot/char across a boundary (DebugVis boundary visualizer shows the
  line); assert no relogin and the configured resume behavior. A real 1.12.1 client confirms
  the visual handoff.
- Failure injection: kill node B mid-handshake → assert `MIGRATE_ABORT` keeps P on A; kill
  the owning node → assert disconnect + reconnect to survivor.
- Unit: packet framing, auth-digest computation (reuse `shared` `Sha1`/`BigNumber`),
  migration state machine vs mock nodes.

## 10. Scope / non-goals (v1)

- **Single gateway** (front-door SPOF). Multi-gateway behind realmd/round-robin is additive
  (gateways are independent, coordinating only via shared DB + nodes) — a later enhancement.
- **Node crash is not transparent** (inherent — unsaved state is lost; reconnect to survivor).
- No client-side modifications (vanilla 1.12.1 protocol only).
- Reuse `shared` (AuthCrypt, WorldPacket/ByteBuffer, Database, Config, Log, Sha1, BigNumber)
  and the realmd binary template; do NOT link the game lib into the gateway.
