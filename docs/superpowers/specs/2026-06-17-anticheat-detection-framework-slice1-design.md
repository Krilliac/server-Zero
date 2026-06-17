# Anti-Cheat / Movement-Validation Framework — Slice 1 Design (Core Detection Pipeline)

Date: 2026-06-17
Target: MaNGOS Zero (Vanilla WoW 1.12.1), repo `D:\server-Zero`, branch `feature/anticheat-detection-framework`.
Status: **Draft — autonomous build proceeding with conservative defaults; open questions deferred to user (see §12).**

## 0. Context & guardrails

The user's fork (which previously contained an integrated anti-cheat / physics / movement / cluster feature set) was lost. We are rebuilding it from scratch on upstream MaNGOS Zero, as **layered slices**. This document specs **Slice 1: the core detection pipeline** — the interlocking foundation everything else layers on. Cluster is explicitly **last** (its hooks are stubbed as safe no-ops here so movement/manager interfaces don't need retrofitting later).

Hard guardrails for every slice:
- **Config-gated, default OFF.** With `AntiCheat.Enable = 0` (default), there is *zero* behavior change and effectively zero overhead (one bool check at each hook). The server runs exactly as upstream.
- **Server-authoritative, low false-positive.** Staged scoring + decay + hysteresis, not instant hard punishment.
- **Centralized response.** All countermeasures go through one manager entrypoint; callers never teleport/kick directly.
- **1.12.1-compatible only.** Time sync uses `CMSG_PING`/`SMSG_PONG` exclusively. No modern opcodes/features.
- **Must compile (Release) + boot-test** before the slice is considered done.

## 1. Subsystems in Slice 1

1. **AntiCheatMgr** — singleton manager: scoring model, violation recording, central punishment, persistence, GM review hooks.
2. **Movement validation** — hook in the movement opcode path; normalizes move state, runs detectors, feeds the manager.
3. **Physics validation** — server-side position plausibility (terrain height, liquid, collision) as a first-class validation stage feeding detector confidence.
4. **Ping/pong time sync** — per-session rolling latency window + EWMA; latency-aware movement tolerances.
5. **Shared config** — all toggles/thresholds, exact names/types/defaults/load points.
6. **Database** — `character_anticheat_violation` table (+ optional session log), async writes.
7. **System hooks** — world-tick maintenance (decay/flush), per-session ping update, movement-packet hook.

Deferred to later slices: full network/packet-burst hardening, security/session-state machine, data-cache layer, debug visualizer beyond log/GM-chat, **cluster** (last).

## 2. New module layout

```
src/game/AntiCheat/
  AntiCheatMgr.h / .cpp        # singleton, scoring, punishment, persistence, GM hooks
  AntiCheatDefines.h           # enums: violation types, actions, detector ids; constants
  MovementAnticheat.h / .cpp   # per-player movement validator (state tracking + detectors)
  PhysicsValidator.h / .cpp    # position plausibility stage (terrain/liquid/collision)
```
Rationale: isolated module, mirrors `src/game/Warden/` pattern (its own folder, own `*Mgr`). Wired into the existing `game` static lib via `src/game/CMakeLists.txt` (glob-based — confirm during build).

Per-player validator state lives on `Player` via a single owned pointer (`Player::m_movementAnticheat`) to keep `Player` clean and allow null-when-disabled. Per-session latency state lives on `WorldSession` (small POD, always present — it's cheap and ping handling is session-level).

## 3. Config (src/shared/Config + World.h/.cpp + mangosd.conf.dist.in)

Pattern (confirmed): add enum value in `World.h` (`eConfigUInt32Values` / `eConfigBoolValues` / `eConfigInt32Values` before the `*_VALUE_COUNT` sentinel), load in `World::LoadConfigSettings` via `setConfig` / `setConfigMinMax`, document + default in `src/mangosd/mangosd.conf.dist.in`, read via `sWorld.getConfig(CONFIG_...)`.

Keys (all default OFF/conservative):

| Conf key | Enum | Type | Default | Meaning |
|---|---|---|---|---|
| `AntiCheat.Enable` | CONFIG_BOOL_ANTICHEAT_ENABLE | bool | 0 | Master switch. Off = no hooks run. |
| `AntiCheat.Movement.Enable` | CONFIG_BOOL_ANTICHEAT_MOVEMENT | bool | 1 | Movement detectors (gated by master). |
| `AntiCheat.Physics.Enable` | CONFIG_BOOL_ANTICHEAT_PHYSICS | bool | 1 | Physics validation stage. |
| `AntiCheat.ExemptBots` | CONFIG_BOOL_ANTICHEAT_EXEMPT_BOTS | bool | 1 | Skip server-controlled playerbots. |
| `AntiCheat.ExemptGMLevel` | CONFIG_UINT32_ANTICHEAT_EXEMPT_GM | uint32 | 1 | Min GM security level exempted. |
| `AntiCheat.Action` | CONFIG_UINT32_ANTICHEAT_ACTION | uint32 | 1 | Max action: 0=off,1=log,2=GM-alert,3=rubberband,4=kick. |
| `AntiCheat.Speed.Tolerance` | CONFIG_UINT32_ANTICHEAT_SPEED_TOL | uint32 | 110 | Allowed % of server speed before flag (110 = +10%). |
| `AntiCheat.Teleport.Distance` | CONFIG_UINT32_ANTICHEAT_TELE_DIST | uint32 | 50 | Yards of single-packet jump (latency-adjusted) → flag. |
| `AntiCheat.Score.Warn` | CONFIG_UINT32_ANTICHEAT_SCORE_WARN | uint32 | 30 | Score → log/GM alert. |
| `AntiCheat.Score.Rubberband` | CONFIG_UINT32_ANTICHEAT_SCORE_RUBBER | uint32 | 60 | Score → teleport-back to last valid pos. |
| `AntiCheat.Score.Kick` | CONFIG_UINT32_ANTICHEAT_SCORE_KICK | uint32 | 120 | Score → kick. |
| `AntiCheat.Score.DecayPerSec` | CONFIG_UINT32_ANTICHEAT_DECAY | uint32 | 2 | Points decayed per second (hysteresis). |
| `AntiCheat.Persist` | CONFIG_BOOL_ANTICHEAT_PERSIST | bool | 1 | Write violations to DB. |
| `TimeSync.Enable` | CONFIG_BOOL_TIMESYNC_ENABLE | bool | 1 | Maintain per-session latency EWMA. |
| `TimeSync.EWMA.Alpha` | CONFIG_UINT32_TIMESYNC_ALPHA | uint32 | 20 | EWMA alpha %/100 (20 = 0.20). |
| `TimeSync.Desync.Threshold` | CONFIG_UINT32_TIMESYNC_DESYNC | uint32 | 1000 | ms deviation from baseline → desync flag. |

`AntiCheat.Action` is a **ceiling**: the manager never escalates past it. With the master switch off, none of this runs.

## 4. AntiCheatMgr (the core)

Singleton (`#define sAntiCheatMgr AntiCheatMgr::instance()`, WardenCheckMgr style). Responsibilities:

- **Init/Load** (called from `World::SetInitialWorldSettings`, after Warden load): read config snapshot, prepare log channel; no heavy load.
- **`RecordViolation(Player*, ViolationType, float weight, const ViolationContext&)`** — the single ingress for all detectors. Adds weighted points to the player's score, persists (async, if enabled), and calls `Evaluate()`.
- **`Evaluate(Player*)`** — compares decayed score against thresholds and invokes the **single** countermeasure path `Apply(Player*, Action)` (log / GM-alert / rubberband / kick), capped by `AntiCheat.Action`.
- **`Apply(...)`** — the ONLY place that teleports/kicks. Rubberband = teleport to player's last server-validated position (`Player::m_movementAnticheat->lastValidPos`). Kick = `WorldSession::KickPlayer`.
- **Decay** — per-player score decays linearly via `DecayPerSec`, applied lazily on next access using `getMSTime()` delta (no per-tick sweep needed for correctness; a world-tick maintenance pass flushes idle entries).
- **GM review hooks** — `.anticheat status <player>` / `.anticheat report` chat commands (added to ChatCommands) read live scores + recent DB rows. (Slice 1: status + report; full GM UI later.)

Per-player score state is held in the per-player validator object (cache-friendly, auto-freed on logout), not a global map, to avoid lock contention on map threads. The manager holds only global config + a mutexed queue for cross-thread GM/report reads.

**Scoring model (initial, tunable via config):**

| Violation | Weight | Notes |
|---|---|---|
| Speed exceed | (excess% ) clamp 5–25 | scales with how far over tolerance |
| Teleport/blink | 25 | single-packet jump beyond latency-adjusted max |
| Vertical anomaly (Z-gain, no jump/levitate/fly) | 20 | climbing without cause |
| Flag contradiction (e.g. FLYING w/o CAN_FLY) | 40 | near-certain tamper |
| Underwater-walk / fly-hack surface mismatch | 30 | physics stage |
| Desync (latency) | 5 | low weight; informs confidence, rarely punishes |

Decay + thresholds give staged escalation: brief blips log and fade; sustained cheating climbs to rubberband then kick.

## 5. Movement validation (MovementAnticheat + hook)

**Hook point (confirmed):** `WorldSession::HandleMovementOpcodes` in `MovementHandler.cpp`, immediately after `movementInfo.Read(recv_data)` and before `VerifyMovementInfo` (~line 340). Single call:

```
if (sAntiCheatMgr enabled && plMover && !exempt(plMover))
    plMover->GetMovementAnticheat()->HandlePositionUpdate(opcode, movementInfo);
```

`HandlePositionUpdate` (per-player, runs on map/world thread — safe, single-threaded per map):
1. **Normalize move state** from `movementInfo` flags (ground / swim / fall / fly / transport / mount / root) — one source of truth used by all detectors.
2. Compute deltas vs last accepted packet: `dt` (clamped, using packet time + server `getMSTime`), horizontal distance, vertical delta.
3. Run cheap detectors in order; each calls `sAntiCheatMgr.RecordViolation(...)` on trip:
   - **Speed**: `dist / dt` vs `Player::GetSpeed(activeMoveType)` × `Speed.Tolerance%`, plus latency slack (EWMA).
   - **Teleport/blink**: single-packet `dist` > `Teleport.Distance` + latency·speed slack, while not on transport / not taxi.
   - **Vertical**: positive Z delta beyond jump arc while no FALLING/JUMPING/LEVITATING/FLYING/SWIMMING and not on stairs/slope (physics stage confirms).
   - **Flag contradiction**: `FLYING`/`CAN_FLY` set without server grant; `SWIMMING` while terrain dry; `ROOT` while moving.
4. On suspicion (not every packet), invoke **PhysicsValidator** (moderate cost) to confirm/deny before heavier weight.
5. Update `lastValidPos` / last packet snapshot only if accepted (so rubberband target is clean).

Periodic vs event-driven: detectors are **event-driven** (per move packet). A light **per-session** timer (in `WorldSession::Update`) handles latency desync checks + idle decay; a **world-tick** maintenance pass (new `WUPDATE_ANTICHEAT`) flushes the persistence queue and prunes idle state.

## 6. Physics validation (PhysicsValidator)

A pure, side-effect-free stage: `Result Validate(Player*, MoveState, MovementInfo const&)`. Uses confirmed terrain APIs (cheap→expensive, escalated only on suspicion):
- `TerrainInfo::GetHeightStatic(x,y,z)` — ground/VMAP height; impossible-floating / underground checks.
- `TerrainInfo::getLiquidStatus / IsInWater / IsSwimmable` — validate SWIMMING vs actual liquid; detect water-walk/fly over land.
- `Map::IsInLineOfSight` / `GetHitPosition` — **only on suspicion** — wall/no-clip bypass (did the straight-line move pass through solid geometry?).
- Movement-flag-aware tolerances (slopes, stairs, transports excluded). Returns `OK / SUSPECT / IMPOSSIBLE` + reason; the validator converts that into violation weight. Never mutates state.

## 7. Time sync (ping/pong EWMA)

Confirmed: `WorldSocket::HandlePing` reads `ping` seq + client `latency`, calls `m_Session->SetLatency(latency)`, echoes `SMSG_PONG`. Additive changes:
- `WorldSession` gains a small latency window: `uint32 m_latSamples[8]`, index, `m_latEWMA`, `m_latMin/Max`, `m_lastPingMS`.
- New `WorldSession::UpdateLatencyStats(uint32 sampleMS)` called from `HandlePing` after `SetLatency`. Computes EWMA (`alpha` from config) + min/max + jitter.
- `GetLatencyEWMA()` / jitter consumed by movement detectors for tolerance, and by a **desync** detector (deviation > `Desync.Threshold`, scaled by jitter → low-weight violation, mostly informational). Server clock = `getMSTime()` (monotonic). Client clock never trusted as authoritative.

## 8. Database (character DB)

New table (character DB, per-character; chosen over realmd so wipes/transfers carry naturally — see §12 Q4):

```sql
CREATE TABLE `character_anticheat_violation` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `guid` INT UNSIGNED NOT NULL,           -- character low-guid
  `account` INT UNSIGNED NOT NULL DEFAULT 0,
  `time` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `type` TINYINT UNSIGNED NOT NULL,       -- ViolationType
  `score` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `map` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `x` FLOAT NOT NULL DEFAULT 0, `y` FLOAT NOT NULL DEFAULT 0, `z` FLOAT NOT NULL DEFAULT 0,
  `speed` FLOAT NOT NULL DEFAULT 0,
  `latency` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `detail` VARCHAR(128) NOT NULL DEFAULT '',
  PRIMARY KEY (`id`), KEY `idx_guid_time` (`guid`,`time`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
```

Writes are async via `CharacterDatabase.PExecute(...)`. Schema ships as a migration `.sql` under `_db/Character/Updates/` and is also folded into `characterLoadDB.sql` for fresh installs. Consumed by: `AntiCheatMgr::RecordViolation` (insert) and `.anticheat report` (select).

## 9. System hooks summary

| Hook | File / point | Purpose |
|---|---|---|
| Manager init | `World::SetInitialWorldSettings` (after Warden) | construct/load AntiCheatMgr |
| Config load | `World::LoadConfigSettings` | all keys §3 |
| Move packet | `MovementHandler.cpp` ~L340 | per-packet validation |
| Ping update | `WorldSocket::HandlePing` | latency EWMA |
| Per-session tick | `WorldSession::Update` (Warden block) | desync check, decay |
| World tick | `World::Update` new `WUPDATE_ANTICHEAT` (~30s) | flush queue, prune |
| Player attach/detach | `Player` ctor/dtor or login/logout | own/free validator |
| GM cmds | `src/game/ChatCommands/` | `.anticheat status/report` |

## 10. Build / linkage

`src/game/AntiCheat/*.cpp` compiled into the `game` lib (confirm `src/game/CMakeLists.txt` globs `*.cpp` recursively; if explicit lists, add files). No new external deps. Reuses existing `shared` (Config, Database, Log, Timer) and `game` (Map/TerrainInfo/Unit/Player) symbols.

## 11. Risks & false positives

- **Latency spikes / teleports (taxi, summon, GM tele, BG, instance-port):** all server-initiated relocations must mark the player "trusted" for the next packet (set `lastValidPos` and a skip flag in `HandleMoverRelocation` / teleport ack) so legitimate ports don't score. Slice 1 wires the ack paths.
- **Transports/elevators:** excluded via `MOVEFLAG_ONTRANSPORT` + transport-offset handling already in `VerifyMovementInfo`.
- **Slopes/stairs/water edges:** physics stage uses tolerances + nearest-surface height (the existing #359 fix) to avoid Z false-positives.
- **Playerbots:** exempt by default (server-controlled movement).
- **Conservative defaults + log-only ceiling** mean Slice 1 ships in observe mode; thresholds tuned from real logs before enabling enforcement.

## 12. DEFERRED — genuine design questions for the user (phone-friendly, see chat)

1. Enforcement ceiling when enabled: **log-only first** (recommended) vs allow rubberband/kick immediately.
2. Threshold tuning: accept conservative defaults in §3 (recommended) vs you'll specify.
3. Debug visualizer: no safe built-in 1.12.1 client visual exists that won't touch gameplay → default to **GM-chat + log markers** (recommended). OK?
4. Violation persistence DB: **character DB per-character** (recommended) vs realmd per-account.
5. Bots: **exempt playerbots** (recommended) vs validate them too.
6. Module path/name `src/game/AntiCheat/` and command prefix `.anticheat` — OK?
```
