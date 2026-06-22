# Cluster Gateway — Phase 3 Implementation Plan (loading-screen migration)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Transparently migrate an in-world, gateway-fronted player from node A to node B with NO relogin/char-select — the §5 handshake, loading-screen resume. Validated mechanically via a force-migrate path (a real client is only needed to *see* the loading screen).

**Architecture:** Node A detects the need to migrate (cross-node zone, or a force-migrate command) and — for a gateway-fronted session — emits `GW_MIGRATE_REQUEST` to the gateway instead of the Phase-4 kick, after quiescing + `SaveToDB`. The gateway buffers client packets, `GW_SESSION_PREPARE`s node B, awaits `GW_SESSION_READY`, atomically switches the client's forward-target, releases node A, and replays the buffer. Node B loads the player from the shared DB and sends the loading-screen resume (`SMSG_TRANSFER_PENDING` + `SMSG_NEW_WORLD`). See the spec §5 for the full sequence and guarantees.

**Tech Stack:** Same as Phases 1–2. Builds on `63987488`.

## Global Constraints

- Gateway game-independent (shared ByteBuffer, local opcode constants).
- Wire types already reserved in `GatewayProtocol.h`: `GW_SESSION_PREPARE=4`, `GW_SESSION_READY=5`, `GW_MIGRATE_REQUEST=6`, `GW_MIGRATE_ABORT=7`. Use them.
- Only gateway-fronted sessions use this path (`WorldSession::IsGatewayFronted()`); a normal client/session is untouched. All node changes gated by that flag.
- Authoritative state moves via shared DB (`Player::SaveToDB` on A, `LoadFromDB` on B) — reuse Phase 4 `SerializeForMigration`/`ValidateMigrationBlob` for the integrity check, but the DB is the source of truth.
- Exactly-one-authority ordering: A saves before B loads; A releases only after the gateway commits the switch (spec §5).
- Build: gateway + mangosd as touched.

---

### Task 1: Node-side migration trigger + force-migrate command

**Files:**
- Modify: `src/game/Server/GatewayIntake.{h,cpp}` (send `GW_MIGRATE_REQUEST`, handle `GW_SESSION_PREPARE`/`GW_MIGRATE_ABORT`), `src/game/Object/Player.cpp` (gateway-fronted branch in the migration trigger), `src/game/ChatCommands/ClusterCommands.cpp` + `Chat.{h,cpp}` (a `.gateway migrate <player> <node>` test command).

**Interfaces:**
- Produces: `GatewayIntake::RequestMigrate(uint32 clientId, uint32 destNode, uint32 charGuid)` → frames `GW_MIGRATE_REQUEST` to the gateway. Node-B side: on `GW_SESSION_PREPARE{clientId, accountId, charGuid, destNode}` it opens a fronted session, loads the player, and replies `GW_SESSION_READY{clientId, ok}`.

- [ ] **Step 1:** In `Player`'s migration trigger (where Phase 4/5 calls `MigrateToNode`), add: if the owning `WorldSession->IsGatewayFronted()`, instead call `sGatewayIntake.RequestMigrate(session->GetGatewayClientId(), destNode, GetGUIDLow())`, then quiesce (stop sending updates) + `SaveToDB()`. Do NOT kick. Keep the non-fronted path (MigrateToNode kick) unchanged.
- [ ] **Step 2:** `GatewayIntake::RequestMigrate` frames `GW_MIGRATE_REQUEST{clientId, destNode, charGuid}` over the active gateway link.
- [ ] **Step 3:** Node-B intake: handle `GW_SESSION_PREPARE{clientId, accountId, charGuid, locale, security, destNode}` — create a gateway-fronted `WorldSession` (as in `GW_SESSION_OPEN`), then drive a character login for `charGuid` from the shared DB (reuse the normal player-login path: queue a synthetic `CMSG_PLAYER_LOGIN(charGuid)` into the session, OR call the load directly). Reply `GW_SESSION_READY{clientId, ok=loaded}`.
- [ ] **Step 4:** Handle `GW_MIGRATE_ABORT{clientId}` on node A: re-activate the quiesced session (un-quiesce; it was saved but not destroyed).
- [ ] **Step 5:** Add `.gateway migrate <playerName> <nodeId>` (SEC_ADMINISTRATOR, console-ok): looks up the online player; if gateway-fronted, calls the same `RequestMigrate` path. This is the mechanical test trigger.
- [ ] **Step 6:** Build mangosd. Gated-off boot unchanged. Commit: `git commit -am "mangosd: gateway migration trigger + session-prepare + force-migrate command"`

---

### Task 2: Gateway migration orchestration (buffer → prepare → switch → release → replay)

**Files:**
- Modify: `src/gateway/ClientSocket.{h,cpp}` (migration state machine + packet buffer), `src/gateway/NodeRegistry.{h,cpp}` (route control frames), `src/gateway/NodeLink.cpp` (surface `GW_MIGRATE_REQUEST`/`GW_SESSION_READY` to the owning ClientSocket).

**Interfaces:**
- Consumes: `GW_MIGRATE_REQUEST` (from node A), `GW_SESSION_READY` (from node B).
- Produces: a per-client migration state (`MIGRATING_PREPARING`), a FIFO buffer of client→server packets held during the window, and the atomic switch of `m_CurrentNodeId`.

- [ ] **Step 1:** On `GW_MIGRATE_REQUEST{clientId, destNode}` for a ClientSocket: set state=MIGRATING, start buffering all incoming client packets (don't forward), and send `GW_SESSION_PREPARE{clientId, accountId, charGuid, locale, security, destNode}` to `Get(destNode)`.
- [ ] **Step 2:** On `GW_SESSION_READY{clientId, ok}`: if ok → atomic (per-client lock) `oldNode=m_CurrentNodeId; m_CurrentNodeId=destNode;` send `GW_SESSION_RELEASE{clientId}` to `Get(oldNode)`; replay the buffered packets to `Get(destNode)`; clear state. If !ok → send `GW_MIGRATE_ABORT{clientId}` to the old node, flush the buffer to the old node, clear state (stay put).
- [ ] **Step 3:** While MIGRATING, server→client packets from EITHER node for this client still encrypt+send normally (the client stream is continuous) — node B's resume packets (TRANSFER_PENDING/NEW_WORLD) flow straight through.
- [ ] **Step 4:** Build gateway. Commit: `git commit -am "gateway: migration orchestration (buffer/prepare/switch/release/replay)"`

---

### Task 3: Node-B loading-screen resume

**Files:**
- Modify: `src/game/Server/GatewayIntake.cpp` or the prepare path: after the player is loaded on B, send the loading-screen resume.

**Interfaces:**
- Produces: on a migrated load (vs a fresh login), node B sends `SMSG_TRANSFER_PENDING(map)` + `SMSG_NEW_WORLD(map, x,y,z,o)` so the client shows a loading screen and re-requests world state; on `MSG_MOVE_WORLDPORT_ACK` the player is added to the map.

- [ ] **Step 1:** When node B loads a player via `GW_SESSION_PREPARE` (migration, not a first login), route it through the same world-port/teleport path the client expects: set the player's transport to the current map+position and emit `SMSG_TRANSFER_PENDING` + `SMSG_NEW_WORLD`. The normal `MSG_MOVE_WORLDPORT_ACK` handler then adds the player to the map. (Investigate the existing teleport path — `Player::TeleportTo` to the same map produces exactly this sequence; reuse it.)
- [ ] **Step 2:** Build mangosd. Commit: `git commit -am "mangosd: node-B loading-screen resume on gateway migration"`

---

### Task 4: End-to-end migration proof (mechanical)

**Files:** test scaffolding (extend `gwtestclient.py` to stay connected + log all received opcodes after login).

- [ ] **Step 1:** Rig: 2 nodes (A intake 9100/world 8090, B intake 9101/world 8092, shared secret) + gateway (Node.1→9100, Node.2→9101). Seed char 898 affinity to node A (so login lands on A).
- [ ] **Step 2:** Client: auth → char-enum → `CMSG_PLAYER_LOGIN(898)` → then **stay connected** and log every received opcode (decrypting headers).
- [ ] **Step 3:** Confirm node A loaded the player. Then run on node A's console (via SOAP or stdin): `.gateway migrate Dev 2`.
- [ ] **Step 4:** VERIFY (the decisive proof): gateway log shows `GW_MIGRATE_REQUEST` → `GW_SESSION_PREPARE` to node B → `GW_SESSION_READY` → switch → `GW_SESSION_RELEASE` to node A. **Node B** loads char 898 (`Player ... added to map`). The **same** synthetic client connection (never reconnected) receives `SMSG_TRANSFER_PENDING` + `SMSG_NEW_WORLD` (the loading-screen resume) — proving the transparent handoff. Node A released the session.
- [ ] **Step 5:** Commit: `git commit -am "gateway: Phase 3 end-to-end transparent migration (mechanical proof)"`

---

## Self-Review

**Spec coverage:** §5 sequence steps 1-7 → Task 1 (trigger+prepare), Task 2 (buffer/switch/release/replay), Task 3 (resume). §5 abort path → Task 1 Step 4 + Task 2 Step 2. The "exactly one authority" ordering (A saves before B loads, releases after switch) → Task 1 Step 1 + Task 2 Step 2.

**Placeholder scan:** the node-B load mechanism (synthetic CMSG_PLAYER_LOGIN vs direct load) is a concrete impl choice flagged for the implementer, not a gap. Opcodes (GW_* reserved 4-7; SMSG_TRANSFER_PENDING/NEW_WORLD) are concrete.

**Type consistency:** `RequestMigrate(clientId, destNode, charGuid)` (Task 1) ↔ `GW_MIGRATE_REQUEST` parsed by the gateway (Task 2). `GW_SESSION_PREPARE`/`READY` payloads consistent between Task 1 (node) and Task 2 (gateway). `m_CurrentNodeId` switch reuses the Phase 2 member.
