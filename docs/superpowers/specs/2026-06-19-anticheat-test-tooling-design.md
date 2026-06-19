# Anti-cheat test tooling: detectors, .anticheat test, .spoof + grant tracking

Covers Slices 21-22 (GM/dev tooling to exercise and live-test the AC).

## New detectors (Slice 21)
- **Acceleration / velocity-delta gate** — `AntiCheat.AccelCheck` (OFF by default,
  FP-prone): sudden speed increase to a real speed within one packet (instant
  0->fast stutter / oscillating speedhacks the steady-state check misses).
- **Opcode legality by state** — an active locomotion-START opcode while
  rooted/stunned (a legit client never sends one in that state).

## .anticheat test (Slice 21) — response-pipeline exerciser
`AntiCheatMgr` gains `TestInject` (gate-bypassing) + `SetScore` + `BuildDiag`.
- `.anticheat test list|config|<type> [weight]|all [weight]` — inject typed
  violations to drive scoring/decay/persist/marker/warn/rubberband/kick/autoban.
- `.anticheat rubberband|resync|timeskip <ms>|score [value]` — per-vector GM tools.

## .spoof (Slice 22) — live detector test (vs .modify legit side)
`MovementAnticheat::SimulateCheat(kind, mag)` crafts the packet signature of a
cheat and runs it through the REAL detectors (snapshot/restore the validator so
live tracking is untouched). `AntiCheatMgr::SetTestBypass` makes detectors run +
apply even on an exempt GM / with AC off.
- `.spoof <kind> [mag]` kinds: speed, teleport, fly, waterwalk, hover, slowfall,
  swim, transport, vertical, jump, desync, noclip.
- Difference from `.modify`: `.modify`/`.fly`/`.waterwalk` apply the **legitimate**
  effect (the AC must NOT flag it); `.spoof` sends the same signature WITHOUT the
  grant (the AC MUST flag it). They test the false-positive and true-positive sides.

## Grant tracking (Slice 22) — single source of truth
The flag-spoof detectors are FP-safe: a flag is legit if a backing **aura OR a
server grant** is present. The grant is recorded in `Player::SetWaterWalk/
SetFeatherFall/SetHover/SetCanFly` (`MovementAnticheat::SetGrantedFlag`) — the one
place all callers (GM `.fly`/`.waterwalk`, spell aura handlers, scripts) go
through. No standalone `.legit` command (absorbed into the setters). `.spoof`
clears grants during its simulation so spoofs always fire.

## Config
`AntiCheat.AccelCheck` (0), `AntiCheat.AccelMaxMult` (6), `AntiCheat.CastBurstPerSec` (8).
All under the `AntiCheat.Enable` gate (except `.spoof`/`.anticheat test`, which
bypass it deliberately for testing).
