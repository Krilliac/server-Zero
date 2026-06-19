# Spell-cast + move-ack timing anti-cheat (Slice 18) — design

## Goal
Extend the time-based anti-cheat (from Slice 17's move-time-skip work) to two more
client packet vectors the user flagged: spell-cast timing and movement-ACK
timestamps. Same per-player validator (`MovementAnticheat`) holds the state.

## 1. Spell-cast timing (`CMSG_CAST_SPELL`)
Hooked in `HandleCastSpellOpcode` *after* the existing known/non-passive validation
(so only legit-eligible casts are timed). Passes the spell's cast time and GCD
(`GetSpellCastTime`, `spellInfo->StartRecoveryTime`) to
`MovementAnticheat::NotifySpellCast`.

Detectors:
- **Cast spam** — more than `AntiCheat.CastBurstPerSec` (default 8) cast requests in
  a 1s window → `AC_VIOLATION_SPELL` (12).
- **GCD bypass** — two GCD-triggering casts closer together than
  `gcd - latency - 150ms` slack → `AC_VIOLATION_SPELL`, weight 8..25 scaled by how
  far under the floor (a cast-speed / no-GCD hack). Only when both casts have a
  non-zero GCD, so off-GCD abilities don't false-positive.

## 2. Move-ACK timestamps (`CMSG_FORCE_*_SPEED_CHANGE_ACK`, `CMSG_MOVE_WATER_WALK_ACK`)
Both acks carry a `MovementInfo` with a client timestamp. Hooked to call
`MovementAnticheat::NotifyMoveAckTime(mi.GetTime())`.

Detector:
- **Ack timestamp regression** — the ack's client time going backwards beyond
  `CLIENT_TIME_BACK_MS` (500) → `AC_VIOLATION_PACKETTIMING` (10), suppressed during
  the post-time-skip grace window.
- The sample is folded into the clock-offset service. Crucially it uses a
  **separate** `m_lastAckTime` (not the movement path's `m_lastClientTime`) so it
  can't skew the per-packet desync delta (which compares movement client-dt vs
  server-dt).

## Config
- `AntiCheat.CastBurstPerSec` = 8 (World.h/.cpp + mangosd.conf)

## Gating
All under the existing `AntiCheat.Enable` master gate and exemption checks;
observe + score only (no rejection), consistent with the rest of the framework.
