# Movement time-sync / desync resync (Slice 17) — design

## Goal
Turn `CMSG_MOVE_TIME_SKIPPED` from a logging stub into a real movement-sync +
anti-cheat signal, and add an optional desync auto-resync. Vanilla 1.12.1 has no
`CMSG_TIME_SYNC_RESP` opcode, so this is the vanilla-accurate equivalent built on
the move-time-skip packet + the existing per-player clock-offset service.

## Background
- Client sends `CMSG_MOVE_TIME_SKIPPED {guid, time_skipped}` when its own movement
  clock jumps (a lag-freeze). The stock handler only logged it.
- The anti-cheat already has a per-packet desync detector (client vs server
  elapsed-time divergence) and a smoothed clock-offset service.

## Design
### 1. Handle CMSG_MOVE_TIME_SKIPPED (`MovementHandler.cpp`)
- Anti-spoof: reported guid must equal the session's active mover.
- **Relay** `MSG_MOVE_TIME_SKIPPED {packGUID, time}` to nearby players (always on)
  so observers' interpolation of the mover stays aligned after its clock skip —
  the real observer-side desync fix.
- Route the skip to the mover's `MovementAnticheat::NotifyClientTimeSkip()` when
  AC movement is enabled.

### 2. NotifyClientTimeSkip (`MovementAnticheat`)
Two jobs:
- **Legit handling:** re-baseline the clock-offset + client-timestamp service and
  arm a grace window (`time + min(skip+1000, 5000)` ms) so the per-packet desync
  detector doesn't double-count the same event. Trust the next packet.
- **Abuse scoring (time-based cheats):** within a 10s window, score
  - oversized skip (`> TimeSync.MaxSkipMs`) → DESYNC, weight scaled by ratio (≤30)
  - skip spam (`> 10 / window`) → PACKETTIMING (12)
  - excessive accumulation (`> 3×MaxSkipMs`) → DESYNC (10)

### 3. Desync auto-resync (`MovementAnticheat::PeriodicCheck`, gated OFF)
- The per-packet desync detector increments a streak (decays on clean packets).
- When `TimeSync.AutoResync` is on and the streak ≥ `TimeSync.ResyncDesyncTrips`,
  rubberband the client to its current authoritative position
  (`NearTeleportTo`) + `NotifyServerRelocation`, cooldown `ResyncCooldownMs`.
  This is the only reliable vanilla lever to realign a drifted client.

## Config (all in World.h/.cpp + run/mangosd.conf)
- `TimeSync.MaxSkipMs` = 2000
- `TimeSync.AutoResync` = 0 (off)
- `TimeSync.ResyncDesyncTrips` = 5
- `TimeSync.ResyncCooldownMs` = 10000

## Safety / gating
- Relay is always-on standard movement forwarding (the non-AC movement benefit).
- Abuse scoring runs only under the AC gate (`AntiCheat.Enable`) and respects exemptions.
- Auto-resync is OFF by default and cooldown-limited.
