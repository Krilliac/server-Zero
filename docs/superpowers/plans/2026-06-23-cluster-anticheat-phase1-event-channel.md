# Cluster Anti-Cheat Phase 1 — AC Event Channel Implementation Plan

> **For agentic workers:** implement task-by-task; each task ends at an independently testable deliverable. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Give the game-independent gateway a one-way channel to report detected anti-cheat violations to a player's owning node, where the existing `AntiCheatMgr` records, scores, and escalates them — the foundation every later phase reports through.

**Architecture:** New `GW_AC_EVENT` frame on the shared gateway protocol. The gateway builds the frame and sends it on the player's node link. The node-side `GatewayIntake` parses it on the reactor thread and **marshals it to the world thread** via a per-`WorldSession` AC-event queue (mirroring how `QueuePacket` defers game packets), because `AntiCheatMgr::RecordViolation` touches the `Player`, the DB, and can kick — all world-thread work. A new `AntiCheatMgr::RecordGatewayViolation(Player*, type, weight, detail)` wraps the existing pipeline.

**Tech Stack:** C++03-ish MaNGOS, ACE reactor, shared `ByteBuffer`, MariaDB. Gateway binary links `shared` only (NOT the game lib).

## Global Constraints

- Gateway stays **game-independent**: only `shared` (ByteBuffer + `GatewayProtocol.h`); no `Opcodes.h`/`Player.h`/`AntiCheatMgr.h`.
- New protocol frame is **append-only** at the end of the `GatewayMsg` enum; do not renumber existing values (`GW_HELLO=8`).
- AC-event handling is **config-gated and GM-exempt** via the existing `AntiCheatMgr` gate — gateway events run through the same `RecordViolation` path, so the existing enabled/exempt/tolerance logic applies unchanged.
- All cross-thread delivery uses the existing locked-queue pattern; **never** call `AntiCheatMgr`/`Player` from the intake reactor thread.
- New violation enum values are **appended before `AC_VIOLATION_MAX`**; existing values keep their numbers (they are persisted as the `type` column).
- Copyright header style + brace/indent style match the file being edited.

---

## File Structure

- `src/shared/Cluster/GatewayProtocol.h` — add `GW_AC_EVENT = 9` + payload doc. (both binaries)
- `src/game/AntiCheat/AntiCheatDefines.h` — add gateway/edge violation types before `AC_VIOLATION_MAX`.
- `src/game/AntiCheat/AntiCheatMgr.{h,cpp}` — `RecordGatewayViolation(Player*, type, weight, detail)`.
- `src/game/Server/WorldSession.{h,cpp}` — per-session AC-event queue + drain in `Update()`.
- `src/game/Server/GatewayIntake.cpp` — `case GW_AC_EVENT` + `handleAcEvent()`.
- `src/gateway/ClientSocket.{h,cpp}` — `ReportAcViolation(uint8 type, uint8 severity, const char* detail)` send helper + a config-gated one-shot self-test to prove the wire end-to-end.
- `src/gateway/gateway.conf.dist.in` + `run/gateway.conf` — `Gateway.SelfTestAcEvent` (default 0).
- `src/game/Commands/Level3.cpp` (or wherever `.cheat`/`.anticheat` live) — a GM command to inject a gateway-AC-event into the node pipeline for testing the node half without the wire.
- Tests: `src/gateway/test/test_gateway_acevent.cpp` (payload encode/round-trip).

---

### Task 1: `GW_AC_EVENT` protocol frame

**Files:**
- Modify: `src/shared/Cluster/GatewayProtocol.h`

**Interfaces:**
- Produces: `GW_AC_EVENT = 9` in `enum GatewayMsg`. Payload (little-endian via ByteBuffer): `uint32 clientId, uint8 acType, uint8 severity, string detail`. `severity` is a 0..255 hint the node maps to a score weight; `detail` is a short static description (never client input).

- [ ] **Step 1:** Add `GW_AC_EVENT = 9, // gateway -> node: a detected anti-cheat violation for a client` to the enum, after `GW_HELLO = 8`.
- [ ] **Step 2:** Add the payload line to the header doc-comment block (mirror the existing per-message doc style).
- [ ] **Step 3:** Bump `GW_PROTOCOL_VERSION` to `2` (wire vocabulary changed; gateway + node must match).
- [ ] **Step 4:** Build both targets (`--target gateway`, `--target mangosd`) to confirm the shared header still compiles for both. Commit: `cluster-ac: add GW_AC_EVENT gateway->node frame (protocol v2)`.

---

### Task 2: gateway/edge violation types

**Files:**
- Modify: `src/game/AntiCheat/AntiCheatDefines.h`

**Interfaces:**
- Produces: appended `AntiCheatViolationType` values (keep existing numbers):
  - `AC_VIOLATION_GW_SPEED = 15,`  // gateway independent-clock speed/time manipulation
  - `AC_VIOLATION_RATE = 16,`      // packet flood / rate-limit breach (gateway)
  - `AC_VIOLATION_PROTOCOL = 17,`  // malformed / oversized / illegal-state packet (gateway)
  - `AC_VIOLATION_SESSION = 18,`   // multi-session / accounts-per-IP / mid-session IP change (gateway)
  - then `AC_VIOLATION_MAX`.

- [ ] **Step 1:** Insert the four values immediately before `AC_VIOLATION_MAX`, each with the trailing comment above.
- [ ] **Step 2:** Build `mangosd`. Confirm no `switch` on `AntiCheatViolationType` warns about unhandled new cases (if any exhaustive switch exists, add the new cases). Commit: `cluster-ac: add gateway-detected violation types`.

---

### Task 3: `AntiCheatMgr::RecordGatewayViolation`

**Files:**
- Modify: `src/game/AntiCheat/AntiCheatMgr.h`, `src/game/AntiCheat/AntiCheatMgr.cpp`

**Interfaces:**
- Consumes: existing `RecordViolation(Player*, AntiCheatViolationType, float, AntiCheatContext const&)`.
- Produces: `void RecordGatewayViolation(Player* player, AntiCheatViolationType type, float weight, const char* detail);` — public. Builds an `AntiCheatContext` from the player's current map/position (latency from the session EWMA if cheaply available, else 0), sets `ctx.detail = detail`, and calls `RecordViolation`. If `player` is null it logs at error level and returns (pre-in-world events are not scored in Phase 1).

- [ ] **Step 1: Write the failing test** — add a node-side unit/integration assertion is impractical here (needs a live Player); instead the test is the GM command in Task 6 + the e2e in Task 7. Document that in a comment on the method. (Skip a standalone unit test for this task; it is exercised by Task 6/7.)
- [ ] **Step 2:** Declare `RecordGatewayViolation` in `AntiCheatMgr.h` (public, next to `RecordViolation`).
- [ ] **Step 3:** Implement in `AntiCheatMgr.cpp`: null-check player (log + return), build `AntiCheatContext` (mapId/x/y/z from `player->GetMapId()`/`GetPositionX/Y/Z`, `speed=0`, `latency=0`, `detail=detail`), clamp `weight` to the same sane range `RecordViolation` uses, call `RecordViolation(player, type, weight, ctx)`. It deliberately reuses the full gate (enabled/exempt/score/escalate/autoban).
- [ ] **Step 4:** Build `mangosd`. Commit: `cluster-ac: AntiCheatMgr::RecordGatewayViolation wraps the scoring pipeline`.

---

### Task 4: per-`WorldSession` AC-event queue (thread marshal)

**Files:**
- Modify: `src/game/Server/WorldSession.h`, `src/game/Server/WorldSession.cpp`

**Interfaces:**
- Produces:
  - `void QueueGatewayAcEvent(uint8 acType, uint8 severity, std::string const& detail);` — thread-safe; callable from the intake reactor thread.
  - drained in `WorldSession::Update(...)` on the world thread: for each queued event, map `acType`→`AntiCheatViolationType` (validate range; ignore out-of-range), map `severity`→weight (`weight = severity` clamped, default a sane value if 0), and call `sAntiCheatMgr->RecordGatewayViolation(GetPlayer(), type, weight, detail.c_str())`.

- [ ] **Step 1:** Add a private member: a small struct `{ uint8 type; uint8 severity; std::string detail; }` in a `std::mutex`-guarded `std::vector` (or reuse the project's `LockedQueue` if that is the established pattern — check `QueuePacket`'s queue type and match it).
- [ ] **Step 2:** Implement `QueueGatewayAcEvent` (lock, push). Keep the queue bounded (drop + DEBUG_LOG beyond e.g. 64 pending, so a flood can't grow it unboundedly).
- [ ] **Step 3:** In `WorldSession::Update`, after the existing packet drain, swap the queue under lock and process each event on the world thread (the map `acType`→enum + `severity`→weight + `RecordGatewayViolation` call). Guard the whole drain on `sAntiCheatMgr` being enabled is unnecessary (RecordGatewayViolation/RecordViolation already gate), but skip cheaply if the queue is empty.
- [ ] **Step 4:** Build `mangosd`. Commit: `cluster-ac: WorldSession AC-event queue marshals gateway events to the world thread`.

---

### Task 5: node intake `handleAcEvent`

**Files:**
- Modify: `src/game/Server/GatewayIntake.cpp`

**Interfaces:**
- Consumes: `WorldSession::QueueGatewayAcEvent`, `GW_AC_EVENT`.

- [ ] **Step 1:** Add `case GW_AC_EVENT: handleAcEvent(in); break;` to the authenticated `dispatch` switch (NOT reachable pre-HELLO — it is inside the `m_authenticated` switch already).
- [ ] **Step 2:** Implement `handleAcEvent(ByteBuffer& in)`: read `uint32 clientId, uint8 acType, uint8 severity, std::string detail`; `WorldSession* s = sGatewayIntake.FindSession(clientId)`; if null, `DEBUG_LOG` and return; else `s->QueueGatewayAcEvent(acType, severity, detail)`. Wrap in the existing `try/catch(ByteBufferException&)` already around `dispatch`.
- [ ] **Step 3:** Build `mangosd`. Commit: `cluster-ac: node intake routes GW_AC_EVENT to the session AC-event queue`.

---

### Task 6: GM test-injection command (node half)

**Files:**
- Modify: the command table + handler where `.anticheat test` / `.cheat` live (search `AC_VIOLATION` in `src/game/Commands` / wherever `TestInject` is wired).

**Interfaces:**
- Produces: a GM command, e.g. `.anticheat gwevent <type> <severity>`, that calls `sAntiCheatMgr->RecordGatewayViolation(handler's selected/own Player, (AntiCheatViolationType)type, severity, "gm-test")`. This exercises Tasks 2–4 without the wire.

- [ ] **Step 1:** Add the command following the existing `.anticheat` subcommand pattern (and add its row to the World-DB command-table PR convention used earlier — note it in the commit body; do not block on DB).
- [ ] **Step 2:** Build + deploy `mangosd`; on a live node `.anticheat gwevent 16 30` then `.anticheat status` (or check `world-server.log` / `character_anticheat_violation`) shows the recorded gateway violation. Commit: `cluster-ac: .anticheat gwevent GM injection for the gateway AC channel`.

---

### Task 7: gateway send helper + self-test (wire half) + unit test

**Files:**
- Modify: `src/gateway/ClientSocket.h`, `src/gateway/ClientSocket.cpp`, `src/gateway/gateway.conf.dist.in`, `run/gateway.conf`
- Create: `src/gateway/test/test_gateway_acevent.cpp` (+ wire into the gateway test CMake target like `test_gateway_auth.cpp`)

**Interfaces:**
- Produces: `void ClientSocket::ReportAcViolation(uint8 type, uint8 severity, const char* detail);` — builds the `GW_AC_EVENT` payload (`clientId,type,severity,detail`) and sends it on this client's node link via the existing `link->SendFrame(GW_AC_EVENT, payload)` path (same as `BuildSessionOpen`/`GW_SESSION_OPEN`). No-op + DEBUG_LOG if the link is down.

- [ ] **Step 1: Write the failing test** in `test_gateway_acevent.cpp`: build a `GW_AC_EVENT` payload with known `clientId/type/severity/detail`, parse it back with a ByteBuffer, assert all four fields round-trip. Run, expect FAIL (helper not present).
- [ ] **Step 2:** Implement `ReportAcViolation` (build payload, find node link, `SendFrame`). Re-run the test → PASS.
- [ ] **Step 3:** Add `Gateway.SelfTestAcEvent` (default 0) to `gateway.conf.dist.in`. When set, the gateway sends **one** benign `GW_AC_EVENT(type=AC_VIOLATION_RATE=16, severity=1, "selftest")` per client right after session open, so a full gateway→node→AntiCheatMgr round-trip can be observed in the node log. (A pure diagnostic; off by default.)
- [ ] **Step 4:** Build both targets; deploy; with `Gateway.SelfTestAcEvent=1` + one synthetic client login, confirm the node logs a recorded `AC_VIOLATION_RATE` from the gateway. Set it back to 0. Commit: `cluster-ac: gateway ReportAcViolation + self-test prove the AC channel end-to-end`.

---

## Self-Review notes

- **Spec coverage:** This plan implements spec §2 (the `GW_AC_EVENT` channel + node policy intake) and the foundation for §3 checks. Detectors (spec §3 items 1–10) are Phases 2–5; cluster escalation (§3 item 11) is Phase 6.
- **Threading:** the one real hazard — intake reactor vs world thread — is handled by Task 4's queue; Tasks 3/6/7 never touch `Player` off-thread.
- **Type consistency:** enum names (`AC_VIOLATION_GW_SPEED/RATE/PROTOCOL/SESSION`), method names (`RecordGatewayViolation`, `QueueGatewayAcEvent`, `ReportAcViolation`), and the frame name (`GW_AC_EVENT`) are used identically across tasks.
- **No client changes; all server-side.**
