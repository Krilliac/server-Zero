# More movement / physics anti-cheat vectors (Slice 19) — design

## Goal
Broaden movement/physics detection coverage in `MovementAnticheat::HandlePositionUpdate`
(observe + score, latency-tolerant), reusing existing violation types so no
enum/DB changes are needed; the `detail` string disambiguates each.

## New detectors
All gated by `AntiCheat.Enable` + exemptions (the caller already skips exempt
players), per-packet in the movement path.

### Flag-spoofing (client asserts a capability flag with no aura/state) — FLAG_CONTRADICT
- **Water-walk** — `MOVEFLAG_WATERWALKING` && !`HasAuraType(SPELL_AURA_WATER_WALK)` (25).
- **Hover** — `MOVEFLAG_HOVER` && !`HasAuraType(SPELL_AURA_HOVER)` (25).
- **Slow-fall** — `MOVEFLAG_SAFE_FALL` && !`HasAuraType(SPELL_AURA_FEATHER_FALL)` &&
  class != Rogue (Rogue Safe Fall is an aura-less passive) (15).
- All restricted to the living player (`IsAlive`) to avoid ghost-state edge cases.

### Structural spoofs
- **Transport-flag spoof** — `AC_MOVE_TRANSPORT` state but `!GetTransport()` →
  FLAG_CONTRADICT (20). Closes the bypass where the speed/teleport detectors skip
  transport state.
- **Swim-flag spoof** — `AC_MOVE_SWIM` state but `!IsInWater()` → FLAG_CONTRADICT (20).
  Swimming "in air" abuses swim speed / avoids fall.

### Movement while rooted (root-break) — PHYSICS
- Grounded, non-flagged packet with `horiz > 3.0` yd while `hasUnitState(UNIT_STAT_ROOT)`
  → PHYSICS (20). Restricted to grounded + `!cheapTrip` to exclude knockback/transport
  and the >3yd floor tolerates positional jitter.

## False-positive guards
- Aura-gated flag checks (legit buffs set both the flag and the aura).
- Rogue class exclusion for slow-fall.
- Living-only for flag spoofs; grounded + threshold for root-break.
- Moderate weights so transient transitions (aura just expired, boarding a
  transport) decay rather than escalate; scoring/decay handles the noise.

No config additions. Default behaviour unchanged when AntiCheat.Enable = 0.
