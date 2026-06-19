# No-clip detector + napalm-repo research (Slice 20) — design

## Research: Krilliac/smellslikenapalm (RS2V Vietnam server, Unreal FPS)
Mined the `Physics/` validators for portable detection ideas. The repo's movement
anti-cheat is simpler than ours; most of it (speed, teleport, time-sync, decay +
escalation) we already do better. EAC/Steam/aim/hardware-fingerprint code is not
applicable to a 1.12 client. Distilled outcome:

| Idea | Verdict |
|------|---------|
| **Swept-segment wall-clip / no-clip** (raycast last→new pos vs collision) | **Port — new, high value.** Implemented this slice via VMap LoS. |
| Control-loss revert (frozen/stunned ⇒ revert) | Partial: extended root-break to **stun** this slice (fear/confuse are server-driven). |
| NaN/Inf/world-bounds pre-filter | **Already covered** — core's `IsValidMapCoord` in `VerifyMovementInfo`. |
| Acceleration / velocity-delta gate | Deferred — FP-prone in WoW (knockback/charge/blink/mount). Possible later as a heavily-exempted scoring signal. |
| Turn-rate of facing | Skip — WoW allows instant mouse facing snaps. |
| Opcode-legality-by-state | Possible later — structural (session state machine + grace windows). |
| Large-gap ⇒ reset baseline | Already effectively handled (gap reset + trust-next on relocation). |
| Chat/name sanitization, EAC emulation, hardware fingerprint | Out of scope / not applicable. |

## Implemented this slice
### No-clip / wall-clip detector
`HandlePositionUpdate`: when the physics module is enabled and the player makes a
grounded step `> 4.0` yd that is not a teleport (cheapTrip) and not right after a
server relocation, cast `map->IsInLineOfSight(lastPos+1.5z, newPos+1.5z)`. No LoS
between two consecutive positions ⇒ the client moved through world geometry ⇒
`AC_VIOLATION_PHYSICS` (15). VMap-gated + step floor bounds cost and corner FPs;
scoring signal, not a hard reject.

### Root-break → also stun
The move-while-rooted detector now also fires on `UNIT_STAT_STUNNED`
(`UNIT_STAT_ROOT | UNIT_STAT_STUNNED`); fear/confuse remain excluded because they
are server-pathed movement.

## False-positive notes
- No-clip: corner-cutting on large steps can clip a LoS; mitigated by the 4yd
  floor, GROUND-only, post-teleport/relocation skip, low weight + decay.
- Stun: knockback during stun is airborne (not GROUND state) so excluded.

No config changes; reuses `AC_VIOLATION_PHYSICS`. Inert when AntiCheat.Enable = 0.
