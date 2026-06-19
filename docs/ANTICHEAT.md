# Anti-Cheat, Time-Sync & Debug-Draw — system reference

A server-side, **observe-and-score** anti-cheat framework for MaNGOS Zero
(vanilla 1.12.1), plus a movement time-sync system and an engine-style debug-draw
toolkit. Everything is **config-gated and OFF by default** (`AntiCheat.Enable = 0`)
— the framework is fully inert until enabled. No client modification.

## Philosophy
- **Detectors never punish.** Each detector observes a client packet and, on a
  suspicious signature, calls `AntiCheatMgr::RecordViolation(player, type, weight,
  ctx)`. The central manager is the **single ingress** and the **only** place a
  countermeasure is applied.
- **Weighted score with decay.** Violations add weight to a per-player score that
  decays over time (`AntiCheat.Score.DecayPerSec`), so isolated blips fade while
  sustained cheating accumulates.
- **Escalation, capped by a ceiling.** As the score crosses thresholds the manager
  applies the highest warranted action up to `AntiCheat.Action`: log → GM alert →
  rubberband → kick. Repeated kicks feed an account-level **autoban** (escalating
  1d → 7d → permanent, hour-decayed so spacing offences out doesn't evade it).
- **Latency-tolerant.** Detectors fold in a smoothed per-session latency (EWMA) so
  laggy-but-legit players aren't false-flagged.

## Detectors
Movement (per packet, in `MovementAnticheat::HandlePositionUpdate`):
- **speed** — horizontal distance vs time beyond allowed speed + tolerance.
- **teleport** — single-packet displacement beyond a latency-adjusted max.
- **acceleration** *(opt-in `AntiCheat.AccelCheck`)* — implausible speed jump to a
  real speed within one packet (oscillating speedhacks the steady-state check misses).
- **vertical** — unexplained upward climb on the ground.
- **fly / waterwalk / hover / slowfall flag spoof** — a movement capability flag
  with no backing aura **or** server grant.
- **transport spoof** — `ONTRANSPORT` with no transport (closes the speed/teleport
  bypass that skips transport state).
- **swim spoof** — `SWIMMING` while not in liquid.
- **root-break** — horizontal translation while rooted/stunned.
- **opcode legality** — an active move-START opcode while rooted/stunned.
- **no-clip** — a ground step with no line-of-sight between the two positions
  (moved through world geometry, via VMap).
- **jump** — mid-air / infinite re-jump; **fall** — fall-damage suppression.
- **burst / packet-timing** — movement-packet flood; client-timestamp regression.

Time-based (see Time-Sync below): **desync**, move-time-skip abuse, ack-timestamp
regression, **spell-cast** GCD-bypass + cast-spam, **item** use-in-trade,
**interact** remote-interaction attempt. Plus a periodic idle terrain re-check.

## Time-Sync / desync system
Vanilla has **no `TIME_SYNC` opcode** (WotLK+). This reconstructs the equivalent on
movement timestamps + ping/pong:
- **Latency EWMA** — smoothed per-session latency (detector tolerance + queryable).
- **Clock-offset service** — smoothed (serverClock − clientTimestamp) per player;
  its **drift** is the desync signal (time-manipulation speedhacks).
- **Desync detector** — client-reported vs server-measured elapsed time per packet.
- **`CMSG_MOVE_TIME_SKIPPED`** (was a stub): relays the skip to nearby players
  (fixes observer warp/stutter after a lag-freeze), graces+re-baselines a legit
  skip, and scores oversized/spammed/accumulated skips as a time hack.
- **Movement-timestamp normalization** *(opt-in `TimeSync.MovementCorrection`)* —
  rewrites relayed movement timestamps to the server clock so all observers
  interpolate on one timebase (reduces other-player desync).
- **Auto-resync** *(opt-in `TimeSync.AutoResync`)* — sustained desync rubberbands
  the client to its authoritative position (the only resync lever vanilla has).

Advantages over baseline: detection of time-based speedhacks, fewer false
positives, smoother other-player movement, and full GM observability/tuning.
**Ceiling:** an approximation built on movement timestamps (no dedicated opcode);
normalization is opt-in because it is higher-risk netcode.

## Debug-draw toolkit (`DebugVis`)
Server-spawned, auto-despawning markers the client can see. Composite markers =
a colour-coded glowing beam (visual) + a clickable crystal (hover tooltip with
per-instance data). Used by `.debug vis …` and by the AC violation visualizer.
Key facts: hover requires a **solid** model (crystals, not particle effects);
the client caches GO names per entry, so per-instance tooltips use a reserved
GOOBER entry pool (305000-305511, `debugvis_marker_pool.sql`) + a query-handler
override.

## Command reference (GM)
- `.anticheat status|top [n]|report|reload|set <field> <val>` — inspect/triage/tune.
- `.anticheat test list|config|<type> [w]|all [w]` — inject violations (drive the
  response pipeline on an exempt GM via a test bypass).
- `.anticheat warn|jail|unjail|delete|rubberband|score [v]` — enforce / manage.
- `.spoof <kind>|all [mag]` — live-fire: run a real cheat signature through the
  detectors (snapshot/restore baseline). The **legitimate** counterpart is the
  normal `.fly` / `.waterwalk` / `.modify speed` / spell auras (which record a
  server grant so the AC treats them as legal — single source of truth).
- `.timesync status|config|set <field> <val>|resync|skip <ms>|desync [n]`.

## Config
All in `mangosd.conf`. Master: `AntiCheat.Enable`. Detectors/policy:
`AntiCheat.Movement/Physics`, `AntiCheat.Action` (1-4 ceiling),
`AntiCheat.Score.Warn/Rubberband/Kick/DecayPerSec`, `AntiCheat.Speed.Tolerance`,
`AntiCheat.Teleport.Distance`, `AntiCheat.AccelCheck/AccelMaxMult`,
`AntiCheat.CastBurstPerSec`, `AntiCheat.Exempt*`, `AntiCheat.AutoBan.*`,
`AntiCheat.Jail.*`. Time-sync: `TimeSync.Enable/EWMA.Alpha/Desync.Threshold/
MaxSkipMs/MovementCorrection/AutoResync/ResyncDesyncTrips/ResyncCooldownMs`.
Debug-draw: `DebugVis.Style/Disp.*/Glow/GlowDisp.*/DespawnSeconds`,
`DebugVisualizer.*`. Most are settable at runtime via `.anticheat set` /
`.timesync set` (revert on restart/reload-from-file).

## Honest ceiling
Server-authoritative detection raises the cost of off-the-shelf cheating and
catches the common movement/time/interaction vectors. It is not a guarantee
against a determined client reverser, and is meant to be paired with — not a
replacement for — the core's existing server-side validation. Roll new detectors
out log-first (`AntiCheat.Action = 1`) and observe false-positive rates before
promoting to rubberband/kick/ban.
