# Cluster Gateway — Phase 4 Implementation Plan (config-driven seamless migration)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`).

**Goal:** Make the migration resume mode operator-selectable — `loadingscreen` (Phase 3, proven, Blizzard-faithful default) vs `seamless` (no loading screen). The seamless path is best-effort and flagged for real-client validation.

**Architecture:** A node config selects the resume mode. On a migration arrival (`IsGatewayMigrationArriving()`), node B branches: loading-screen = the Phase-3 `SMSG_LOGIN_VERIFY_WORLD` world-enter; seamless = add the player to the map and send the in-place object/world resume WITHOUT a world-change packet, so the client keeps the map loaded and play continues.

**Tech Stack:** Same as Phases 1–3. Builds on `27718dfc`.

## Global Constraints

- Node config `Gateway.SeamlessMigration` (bool, default **false** → loading-screen, the proven default). All deployments share config, so a node-level config is sufficient (no need to carry it per-migration).
- Only the migration-arrival path (`IsGatewayMigrationArriving()`) is affected; first-login and non-fronted sessions are untouched.
- Seamless is BEST-EFFORT and explicitly documented as needing real-client validation (re-creating the player + visible set mid-stream without a loading screen is beyond what a synthetic client can prove renders cleanly).
- Build: mangosd (node-side resume) + a mechanical test.

---

### Task 1: Config-driven mode + the resume branch

**Files:**
- Modify: `src/game/WorldHandlers/World.h`/`World.cpp` (config `Gateway.SeamlessMigration`), `src/game/WorldHandlers/CharacterHandler.cpp` (the `IsGatewayMigrationArriving()` branch), `src/mangosd/mangosd.conf.dist.in`.

**Interfaces:**
- Produces: `sWorld.getConfig(CONFIG_BOOL_GATEWAY_SEAMLESS)` read at the resume point; the migration-arrival path calls `ResumeSeamless()` vs the existing loading-screen flow.

- [ ] **Step 1:** Add `CONFIG_BOOL_GATEWAY_SEAMLESS` (World.h enum) + `setConfig(..., "Gateway.SeamlessMigration", false)` (World.cpp), documented in the conf dist.
- [ ] **Step 2:** In `HandlePlayerLogin`, where Phase 3 handles `migrationArriving` (Route A login flow), wrap it: `if (migrationArriving && sWorld.getConfig(CONFIG_BOOL_GATEWAY_SEAMLESS)) { seamless path (Task 2) } else { existing loading-screen path }`.
- [ ] **Step 3:** Build mangosd, gated-off boot unchanged. Commit: `git commit -am "mangosd: Gateway.SeamlessMigration config + resume-mode branch"`

---

### Task 2: Seamless resume path on node B

**Files:**
- Modify: `src/game/WorldHandlers/CharacterHandler.cpp` (the seamless branch).

**Interfaces:**
- Produces: a migration-arrival load that adds the player to the world and sends an in-place resume (player self-update + nearby objects + movement) WITHOUT `SMSG_LOGIN_VERIFY_WORLD`/`SMSG_NEW_WORLD`.

- [ ] **Step 1:** Investigate `SendInitialPacketsBeforeAddToMap` / `SendInitialPacketsAfterAddToMap` and what `SMSG_LOGIN_VERIFY_WORLD` triggers client-side. Identify the minimal set that (a) puts the player object in the client's world and (b) does NOT trigger a loading screen.
- [ ] **Step 2:** Implement the seamless branch: load the player + `GetMap()->Add(player)` (same as login) but SKIP `SMSG_LOGIN_VERIFY_WORLD`; send the player's own `SMSG_UPDATE_OBJECT` (create-self), the initial world states, and let the normal grid-visibility push nearby objects; resume the update/movement stream. The client should continue from its existing loaded map with no loading screen.
- [ ] **Step 3:** Build mangosd. Commit: `git commit -am "mangosd: seamless (no-loading-screen) migration resume on node B"`

---

### Task 3: Mechanical test + comparison

**Files:** test scaffolding (`gwtestclient.py` already a continuous reader from Phase 3).

- [ ] **Step 1:** Rig: the Phase 3 two-node rig + gateway. Set `Gateway.SeamlessMigration = 1` on both nodes.
- [ ] **Step 2:** Login char 898 on node A → `.gateway migrate Dev 2`. The continuous-reader client logs opcodes.
- [ ] **Step 3:** VERIFY (mechanical): in seamless mode node B's resume does NOT include `SMSG_LOGIN_VERIFY_WORLD` (0x236) but DOES include `SMSG_UPDATE_OBJECT` (0xA9) for the player + nearby — and the client connection stays alive (no reconnect). Compare against `Gateway.SeamlessMigration=0` (loading-screen): there, a 2nd `0x236` appears. The opcode-shape difference is the mechanical proof the modes diverge as designed.
- [ ] **Step 4:** Document: on-screen seamlessness (no visible loading screen, continuous motion) requires a real 1.12.1 client to confirm — the synthetic client proves the packet-level behavior only.
- [ ] **Step 5:** Commit: `git commit -am "gateway: Phase 4 seamless-mode mechanical test + comparison"`

---

## Self-Review

**Spec coverage:** §4-step1 "both modes config-driven" → Task 1 config + Tasks 2-3. The seamless resume (spec §5 step 7 "seamless: B re-sends P's own object + nearby and continues, no world-change packet") → Task 2. The loading-screen mode is Phase 3 (proven); Phase 4 adds the seamless alternative under config.

**Placeholder scan:** the seamless minimal-packet-set is a concrete investigation (Task 2 Step 1) not a hand-wave; the real-client dependency is explicitly documented, not a gap.

**Type consistency:** `CONFIG_BOOL_GATEWAY_SEAMLESS` used in Task 1 (config) + Task 2 (branch). Reuses the Phase 3 `IsGatewayMigrationArriving()` flag and the Phase 3 continuous-reader test client.

**Honest scope note:** seamless cross-process handoff without a loading screen is beyond what Blizzard does in the open world and beyond synthetic-client validation. This phase delivers the config-selectable path + best-effort implementation + mechanical packet-shape proof; the on-screen UX is a real-client follow-up.
