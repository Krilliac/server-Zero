# Cluster Gateway — Phase 2 Implementation Plan (affinity routing)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** The gateway connects to MULTIPLE nodes and routes each client's character to the node that owns it (affinity), instead of one fixed node. No migration yet (that's Phase 3) — the routing decision is made once, at `CMSG_PLAYER_LOGIN`, before the character loads.

**Architecture:** Replace the single `NodeLink` with a registry of links (one per configured node). Each client tracks its current node. During the pre-world phase (auth + char-enum) the client is attached to a default "pre-world" node; at `CMSG_PLAYER_LOGIN` the gateway looks up the character's owning node in the shared DB and, if different, re-homes the (player-less) session to that node before forwarding the login.

**Tech Stack:** Same as Phase 1 (C++11, ACE, `shared`, MariaDB). Builds on commits through `62ba5c4c`.

## Global Constraints

- Gateway stays game-independent (no Opcodes.h/WorldPacket.h/WorldSession.h; shared `ByteBuffer` + local opcode constants).
- Every node link is authenticated with `Gateway.Secret` (`GW_HELLO`) exactly as Phase 1 — the registry must HELLO each link.
- The affinity source of truth is `cluster_character_node` (character DB: `guid` → `node_id`); fallback `characters.zone` → `cluster_zone_assignment` (login DB); final fallback = the pre-world/default node.
- Re-homing a pre-world session is cheap (no player loaded): `GW_SESSION_RELEASE` to the old node, `GW_SESSION_OPEN` to the new one. This is NOT migration (no state transfer).
- Build: `cmake --build build --config Release --target gateway --parallel 2` (mangosd unchanged this phase).

---

### Task 1: Multi-node registry + link pool

**Files:**
- Create: `src/gateway/NodeRegistry.h`, `src/gateway/NodeRegistry.cpp`
- Modify: `src/gateway/NodeLink.{h,cpp}` (carry a `nodeId`; allow N instances), `src/gateway/Main.cpp` (build the registry from config), `src/gateway/gateway.conf.dist.in`

**Interfaces:**
- Produces: `NodeRegistry` with `void LoadFromConfig()`, `NodeLink* Get(uint32 nodeId)`, `NodeLink* PreWorldNode()` (lowest connected node id, or configured `Node.Default`), `void StartAll()/StopAll()`. Each `NodeLink` gains `uint32 m_NodeId` and sends `GW_HELLO` on connect.

- [ ] **Step 1:** Config: support `Node.Count` (uint32) + per-node `Node.<i>.Host` / `Node.<i>.IntakePort` (and the shared `Gateway.Secret`). Document in `gateway.conf.dist.in` with a 2-node example (Node.1 → 9100, Node.2 → 9101). Keep `Node.1.*` back-compat.
- [ ] **Step 2:** Refactor `NodeLink` so multiple instances coexist (it currently may assume a singleton `sNodeLink`): give it `m_NodeId`, move the singleton accessor into `NodeRegistry`. The inbound `clientId → ClientSocket*` map must be shared/global (a client's inbound packets can come from whichever node currently fronts it) — keep ONE registry-level map, not per-link.
- [ ] **Step 3:** `NodeRegistry::LoadFromConfig` constructs a `NodeLink` per configured node, `StartAll()` starts them (each connects + HELLOs). `Main.cpp` builds the registry and starts it; logs each node's connect state.
- [ ] **Step 4:** Build (`--target gateway --parallel 2`).
- [ ] **Step 5:** Test: boot two `mangosd` nodes with `Gateway.IntakePort=9100` and `=9101` (matching secret), boot the gateway with both configured; confirm the gateway log shows BOTH links authenticated/connected. Kill all.
- [ ] **Step 6:** Commit: `git commit -am "gateway: multi-node registry + link pool (per-node HELLO)"`

---

### Task 2: Per-client current-node + pre-world attach

**Files:**
- Modify: `src/gateway/ClientSocket.{h,cpp}`

**Interfaces:**
- Consumes: `NodeRegistry`.
- Produces: `ClientSocket::m_CurrentNodeId`; on auth the client attaches to `registry->PreWorldNode()` and `GW_SESSION_OPEN`s there; `GW_CLIENT_PACKET` and `GW_SESSION_RELEASE` go to `Get(m_CurrentNodeId)`.

- [ ] **Step 1:** Add `uint32 m_CurrentNodeId` to `ClientSocket`. On successful auth (where Phase 1 sent `GW_SESSION_OPEN` to the single link), instead pick `m_CurrentNodeId = registry->PreWorldNode()->NodeId()` and send `GW_SESSION_OPEN` to that node's link. Register in the registry's `clientId → ClientSocket*` map.
- [ ] **Step 2:** Route all post-auth client packets via `registry->Get(m_CurrentNodeId)->SendFrame(GW_CLIENT_PACKET, ...)`. On close, `GW_SESSION_RELEASE` to the current node and unregister.
- [ ] **Step 3:** Build.
- [ ] **Step 4:** Test: with the 2-node rig, run `gwtestclient.py` (auth + char-enum round-trip). Confirm AUTH PASS + ROUNDTRIP PASS, and the pre-world node (lowest id) shows the fronted session. Kill all.
- [ ] **Step 5:** Commit: `git commit -am "gateway: per-client current-node + pre-world attach"`

---

### Task 3: Player-login affinity routing + re-home

**Files:**
- Modify: `src/gateway/ClientSocket.{h,cpp}` (intercept `CMSG_PLAYER_LOGIN`), `src/gateway/NodeRegistry.{h,cpp}` (the affinity lookup helper)

**Interfaces:**
- Consumes: `LoginDatabase` / `CharacterDatabase` (opened in Main).
- Produces: `uint32 NodeRegistry::NodeForCharacter(uint64 guid)` → owning node id (0 → unknown). `ClientSocket` intercepts `CMSG_PLAYER_LOGIN` (opcode 0x3D), reads the guid, resolves the node, re-homes if needed, then forwards the login.

- [ ] **Step 1:** `NodeRegistry::NodeForCharacter(uint64 guid)`: `CharacterDatabase.PQuery("SELECT node_id FROM cluster_character_node WHERE guid=%u", low(guid))`; if none, `LoginDatabase` join `cluster_zone_assignment` via the char's zone (`CharacterDatabase`: `SELECT zone FROM characters WHERE guid=%u`, then `LoginDatabase`: `SELECT node_id FROM cluster_zone_assignment WHERE zone_id=%u`); return 0 if still unknown.
- [ ] **Step 2:** In `ClientSocket`, when a post-auth decrypted packet has opcode `CMSG_PLAYER_LOGIN` (0x3D), parse the 8-byte player guid from the payload. `uint32 target = registry->NodeForCharacter(guid)`; if `target==0 || !registry->Get(target)` → keep `m_CurrentNodeId` (fallback). If `target != m_CurrentNodeId`: send `GW_SESSION_RELEASE` to the old node, `GW_SESSION_OPEN` to the target node (same accountId/security/locale), set `m_CurrentNodeId=target`. Then forward the `CMSG_PLAYER_LOGIN` packet to `m_CurrentNodeId` as usual.
- [ ] **Step 3:** Build.
- [ ] **Step 4:** Unit-ish check: with no DB rows, `NodeForCharacter` returns 0 (fallback path holds). Verified in the Task 4 integration test.
- [ ] **Step 5:** Commit: `git commit -am "gateway: CMSG_PLAYER_LOGIN affinity routing + pre-world re-home"`

---

### Task 4: Multi-node end-to-end routing test

**Files:** test scaffolding only (extend `run/tools/gwtestclient.py` to send `CMSG_PLAYER_LOGIN` for a given guid and observe).

- [ ] **Step 1:** Rig: node A `gw-node.conf` (WorldServerPort 8090, IntakePort 9100, secret), node B `gw-node2.conf` (WorldServerPort 8092, IntakePort 9101, distinct pid/log, secret). Gateway conf: Node.1→9100, Node.2→9101, default pre-world = Node.1. Pick a real character guid on the test account; seed `cluster_character_node (guid, node_id=2)`.
- [ ] **Step 2:** Boot both nodes + gateway. Extend the client to, after auth+enum, send `CMSG_PLAYER_LOGIN(guid)`.
- [ ] **Step 3:** Run. Expected: gateway log "routing char <guid> to node 2 (re-home from 1)"; **node B** (9101) logs the player login / world-add for that guid; **node A** does NOT. Then re-seed affinity to node 1 and confirm it lands on node A instead.
- [ ] **Step 4:** Commit: `git commit -am "gateway: Phase 2 end-to-end affinity routing (char lands on its owning node)"`

---

## Self-Review

**Spec coverage:** §4 step 3 (player-login routed to owning node) → Tasks 3,4. §3 topology multi-node → Task 1. Pre-world node + char-enum via a node → Task 2. Affinity source (`cluster_character_node` + zone fallback) → Task 3 Step 1. No migration (§5) — correctly deferred to Phase 3.

**Placeholder scan:** none — DB queries, opcode (0x3D), and config keys are concrete. The "default pre-world node" is defined (lowest connected, or `Node.Default`).

**Type consistency:** `NodeRegistry::Get/PreWorldNode/NodeForCharacter` and `NodeLink::m_NodeId` used consistently across Tasks 1–3. The `clientId → ClientSocket*` map moves to ONE registry-level map (Task 1 Step 2) and is used by inbound routing from any link.
