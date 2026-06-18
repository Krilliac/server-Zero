# Master Reconciliation Plan — Original Patch ↔ Current Rebuild

Date: 2026-06-18
Branch: `feature/anticheat-detection-framework`
Inputs: recovered original patch (`docs/original-recovered-patch.txt`, 2140-line diff)
analysed in full; current rebuild = Slices 1–3 on this branch.

## Headline finding

The recovered patch is a **skeleton**: hook points + enums + config + SQL + class
signatures, but nearly every validator/manager body is a **stub returning `true`**
(PhysicsValidator, PositionVerifier, MovementValidator, all Network/Security/
Cluster/Cache/Debug modules, even `AnticheatMgr::ValidateMovement/ValidatePosition`).
It also contains an infinite-recursion bug and unprepared string-concat SQL. The
**only real algorithms** are the time-desync / latency-manipulation checks in the
extended ping/pong path.

Our Slices 1–3 already implement, for real, what the original only stubbed:
central scoring + decay + single-entrypoint escalation, real terrain physics
checks, real per-packet movement detectors, and a real gameobject debug-draw
toolkit with GM commands. **We keep our base and cherry-pick the original's good
ideas — we do not re-apply the patch.**

## What to ADOPT from the original (real value we lack)

1. **Latency-manipulation + desync detection** (the original's one real algorithm),
   adapted to be **stock-1.12.1-client compatible**:
   - The original's extended 3-field `CMSG_PING` carrying `clientTime` needs a
     *modified client* → NOT viable on a stock client. **Drop the clientTime
     channel.**
   - Keep the viable part: compare each client-reported latency sample against our
     EWMA baseline; a large deviation (`TimeSync.Desync.Threshold`, scaled by
     jitter) ⇒ low-weight `DESYNC` violation. This is also the **movement** signal
     (latency feeds movement tolerance — already wired in Slice 1C).
   - Add `time_sync_logs` table for desync forensics.
2. **Explicit fall hook**: `Player::HandleFall` → a `PhysicsValidator::ValidateFall`
   path (cleaner than inferring falls from packet flags only).
3. **Latency-parameterised position check in `Map::Update`** (per-tick map-loop
   validation, distinct from per-packet) — a second, periodic validation cadence.
4. **Fuller countermeasure ladder**: extend our action enum with SLOW_FALL / JAIL /
   TEMP_BAN, and detection categories MOVEMENT_BURST / PACKET_TIMING.
5. **PerformanceMonitor::TrackUpdate(diff)** — cheap world-tick timing metric.
6. **Config vocabulary**: reuse the original's `Movement.Tolerance/ErrorMargin/
   TeleportThreshold` naming when we expose those thresholds.

## What to SKIP (stub-only in original — intent, no code)

- Network PacketCompressor / TrafficOptimizer (no real compression/rate-limit
  existed; the movement-compress hook was nonsensical).
- Security SessionSecurity / PlayerSecurity (empty; only the hook matrix is useful
  if/when we build real rate-limiting).
- Cluster ClusterManager / NodeCommunicator (pure placeholder; **last**, per user).
- DB caches WorldCache / QueryCache / PlayerDataCache (stubs; QueryCache keyed on
  raw SQL is naive and its INSERT pattern is injection-prone — do NOT copy).

These become **optional future epics**, designed fresh if we want them — not merges.

## What to KEEP as-is (ours is better)

Central AntiCheatMgr (scoring/decay/single punishment entrypoint), MovementAnticheat
detectors, PhysicsValidator (real terrain checks), DebugVis (real gameobject
markers + `.debug vis` commands). The original's binary cheat→action, with no
scoring/decay/escalation, is strictly weaker.

## User asks folded in (beyond the original — the original had none of these)

- **Anti-gaming account-level autoban**: the original's TEMP_BAN was an unwired
  stub. We build it properly: per-**account** kick/violation accumulation with
  **slow (days) decay**, ban when rolling-window or lifetime kicks exceed config,
  **escalating** duration (1d→7d→perm). Slow long window defeats "know-the-window"
  gaming. Persist account aggregate + use the realmd ban table.
- **Both-level persistence**: per-character violation log (have) + per-account
  aggregate/ban (new).
- **Latency as a general movement service** (not AC-only): EWMA already feeds
  movement tolerance; formalise and reuse for desync + movement smoothing.

## Slice roadmap (each: spec → compile → boot-test → commit)

- **Slice 4 — Enforcement + anti-gaming autoban**: wire rubberband (validator
  already tracks last-valid pos) + account-level kick/violation accumulator with
  slow decay + escalating autoban via realmd ban table; extend countermeasure
  ladder enum. (User's top explicit ask.)
- **Slice 5 — Latency/desync service**: stock-compatible latency-manipulation +
  desync detection on the EWMA baseline; `time_sync_logs`; formalise latency→
  movement tolerance. + PerformanceMonitor::TrackUpdate.
- **Slice 6 — Detection depth**: explicit fall hook + Map::Update periodic position
  check + MOVEMENT_BURST / PACKET_TIMING detectors.
- **Slice 7+ (optional epics, only if wanted)**: real network rate-limiting,
  session/packet security analyzers, DB query cache — designed fresh.
- **Last — Cluster** (per user).

## Hard rules carried from the original review
- Keep stock 1.12.1 client compatibility (no custom-client-only packet fields).
- All new behaviour config-gated, safe defaults, fail-open.
- Prepared/escaped SQL only — never the original's string-concat INSERTs.
- Respect threading: record violations on the map/world thread, not the network
  thread (defer from ping handler via a session flag).
