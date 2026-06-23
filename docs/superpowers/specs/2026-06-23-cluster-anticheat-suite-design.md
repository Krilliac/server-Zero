# Cluster / Gateway Anti-Cheat Suite — Design Spec

**Date:** 2026-06-23
**Status:** Approved direction ("do all"), pending phased implementation
**Builds on:** the AntiCheat framework (movement validation, timesync desync, `account_anticheat` autoban accumulator, `character_anticheat_violation` log, `.anticheat` commands), the cluster (multi-node + shared DB + inter-node bus), and the connection gateway (encryption-terminating edge proxy).

## 1. Purpose

Use the new architecture to make cheating materially harder, by exploiting two vantage points a single server lacks:
- **The gateway** is a single, neutral, *plaintext* chokepoint that sees every client's traffic and owns the connection (it did the auth + holds the cipher). Ideal for edge validation the client can't influence.
- **The cluster + shared DB** lets violations be correlated across nodes, sessions, characters, and accounts, and makes the autoban accumulator global.

## 2. Core architecture decision — where checks live + how they report

The gateway is **game-independent** (links `shared`, not the game lib), so it cannot call `AntiCheatMgr` or read game state. Therefore:

- **Gateway = edge DETECTION + connection-level enforcement.** It does lightweight, stateless-ish checks on the packet stream (rate, timing, protocol, session) and, on a violation, sends a new **`GW_AC_EVENT`** message to the player's owning node. For egregious connection abuse (flood) it may also disconnect directly.
- **Node `AntiCheatMgr` = POLICY brain.** A new `AntiCheatMgr::RecordGatewayViolation(accountId, guidLow, type, severity, detail)` feeds the existing `kick_score` accumulator + `character_anticheat_violation` log + escalation. Node-side game-state checks (movement physics, teleport, fly/noclip) also funnel here. One policy/escalation path, fed by both gateway and node.
- **Shared DB = global state.** `account_anticheat.kick_score` is cluster-wide, so offenses can't be spread across nodes to dodge thresholds.

`GW_AC_EVENT` is a new `GatewayProtocol.h` message (gateway → node): `{clientId, uint8 acType, uint8 severity, string detail}`. The node maps clientId → session → account/guid and calls `RecordGatewayViolation`.

All checks are **config-gated with tolerances** and **GM-exempt** (reuse `CONFIG_UINT32_ANTICHEAT_EXEMPT_GM`), to avoid false positives.

## 3. The checks (the full suite)

### Gateway edge checks (the client can't influence these)
1. **Independent-clock speedhack.** The gateway timestamps each packet on arrival. Compare the client's *self-reported* movement-time deltas against the gateway's wall-clock receive cadence over a sliding window. A sustained discrepancy beyond tolerance ⇒ time/speed manipulation. Stronger than a node check (the gateway clock is neutral and unaffected by game-thread load) and complements the existing timesync desync detector.
2. **Rate limiting / flood detection.** Per-connection, per-opcode token-bucket caps. Movement spam, action spam, packet floods, Warden-bypass injectors exceed caps ⇒ throttle + `GW_AC_EVENT`; egregious ⇒ disconnect.
3. **Protocol / opcode validation.** Reject malformed packets, oversized payloads, and illegal opcode order/state (movement before login, cast before in-world). Crafted-packet exploits die at the edge, uniformly for all nodes.
4. **Session integrity.** Enforce one live connection per account and an accounts-per-IP cap (multi-box limit) cluster-wide (the gateway sees every connection). Flag mid-session source-IP changes (hijacking).

### Migration / zone checkpoints
5. **Migration state-consistency validation.** On the hand-off, node B validates the arriving (already SHA1-checked) blob is *internally* consistent: position vs. last-known + elapsed time, stats/inventory within bounds. Tamper on node A must still produce a coherent blob; inconsistencies flag at the seam.
6. **Teleport detection (position re-anchor).** Migration + zone entry reassert the authoritative position. An impossible boundary crossing or a position that "jumped" beyond max travel for the elapsed time ⇒ teleport hack. Node-side per-tick position-delta validation backs this up in open world.

### Movement-hack suite (node-side game-state, gateway pre-validates flag legality)
7. **Fly / levitate hack** — movement flags claim flying/levitating without the granting aura/spell.
8. **No-clip / wall-walk** — movement through collision, validated against the LoS/collision (vmap) data the cluster already uses.
9. **Water-walk / no-fall-damage / anti-knockback** — movement flags (waterwalk, feather-fall, root-immunity) asserted without the corresponding effect; ignoring server knockback.
10. **Movement physics** — instant direction reversal, multi-jump, impossible acceleration (extends the existing movement-vector validation).

### Cluster-wide policy
11. **Global autoban escalation.** `account_anticheat.kick_score` accumulates from all sources cluster-wide; escalation warn → kick → temp-ban → perm-ban, with decay. Ban-evasion alts/IPs correlated.

## 4. Phasing (each independently valuable + testable)

1. **AC event channel + node intake.** `GW_AC_EVENT` + `AntiCheatMgr::RecordGatewayViolation`. Foundation; nothing else can report without it. *Test:* gateway sends a synthetic event → node records it (kick_score + violation log).
2. **Gateway edge checks** — rate-limit, protocol/opcode validation, session integrity. *Test:* synthetic client floods / sends malformed / opens 2 connections per account → gateway flags + reports + (egregious) disconnects.
3. **Gateway independent-clock speedhack.** *Test:* synthetic client sends accelerated movement timestamps → speedhack event raised.
4. **Migration/zone state validation + teleport re-anchor.** *Test:* force an impossible position/blob → flagged at hand-off / zone entry.
5. **Movement-hack suite** (fly / noclip / waterwalk / anti-knockback / physics). *Test:* spoofed movement flags / through-collision → flagged.
6. **Cluster-wide autoban escalation + tuning** (decay, exemptions, thresholds). *Test:* accumulate cluster-wide violations → escalation fires; GM exempt.

## 5. Constraints / non-goals

- Gateway stays game-independent (shared `ByteBuffer` + local opcode constants; detection logic only, policy lives node-side).
- Everything config-gated with tolerances + GM exemption; default to *log/flag*, escalate to kick/ban only above thresholds, to avoid false positives on lag/legit play.
- Reuse the existing `AntiCheatMgr`, `account_anticheat`, `character_anticheat_violation`, and `.anticheat` commands — extend, don't duplicate.
- No client modifications; all detection is server-side on the existing 1.12.1 protocol.
- New SQL (if any) follows the non-cluster DB-repo PR convention; AC tables already exist (account_anticheat realm, character_anticheat_violation character).
