# Anti-Cheat Framework — Slice 2 Design (Debug Visualizer + GM commands)

Date: 2026-06-18
Branch: `feature/anticheat-detection-framework`
Builds on Slice 1 (core detection pipeline). Verify bar: compile (Release) + boot-test.

## Goal (per user)

Reinstate the lost fork's **debug visualizer**: when enabled, the server spawns
**temporary gameobjects trailing the player**, **color/model-coded by event type**
(normal movement, time-sync, and each cheat/violation kind). It is a diagnostic
aid only — no gameplay effect — and is **config-gated, OFF by default**. It is
especially useful with **playerbots un-exempted** (config) to test detectors live.

## Design

### Mechanism
Use the existing `WorldObject::SummonGameObject(entry, x, y, z, angle, despawnMs)`
(Object.cpp:2617). It spawns a temporary, non-interactive gameobject visible to
nearby players that auto-despawns. The marker entry is a real
`gameobject_template.entry`; defaults use existing, guaranteed-visible **colored
Banner** GOs (entries 180773–180778, displayIds 6545–6550) plus a circle, so
**no DB changes are required**. Every entry is config-overridable, so the user can
swap in any model/color they prefer.

### Marker categories → default GO entry (all config-overridable)
| Category | Trigger | Default entry | Colour (banner) |
|---|---|---|---|
| Movement (trace) | each accepted move packet (opt-in) | 180773 | blue |
| TimeSync | latency desync event | 180774 | green |
| Speed | speed violation | 180775 | pink |
| Teleport | teleport/blink violation | 180776 | purple |
| Vertical | vertical-climb violation | 180777 | red |
| Flag | fly-flag contradiction | 180778 | yellow |
| Physics | physics-impossible/suspect | 181227 (Circle) | — |

### Integration points (minimal, reuse Slice 1)
- `DebugVisualizer::Mark(player, violationType, x, y, z)` — called from
  **`AntiCheatMgr::RecordViolation`** (all violations funnel there → one call site),
  gated by `DebugVisualizer.Enable`. Spawns the category marker at the event pos.
- `DebugVisualizer::Trace(player, moveState, x, y, z)` — called from
  **`MovementAnticheat`** on each accepted packet when `DebugVisualizer.TraceMovement`
  is on, rate-limited by `TraceMinDistance` (yards) so it doesn't flood.
- Module: `src/game/AntiCheat/DebugVisualizer.h/.cpp` (in the existing AntiCheat
  CMake group — no build wiring beyond the glob).

### Config (World.h enums + World.cpp load + conf templates)
| Key | Type | Default | Meaning |
|---|---|---|---|
| `DebugVisualizer.Enable` | bool | 0 | master switch for the visualizer |
| `DebugVisualizer.TraceMovement` | bool | 0 | drop a marker on every accepted move packet |
| `DebugVisualizer.DespawnSeconds` | uint32 | 30 | marker lifetime |
| `DebugVisualizer.TraceMinDistance` | uint32 | 3 | min yards between trace markers |
| `DebugVisualizer.GO.Movement` | uint32 | 180773 | trace marker entry |
| `DebugVisualizer.GO.TimeSync` | uint32 | 180774 | |
| `DebugVisualizer.GO.Speed` | uint32 | 180775 | |
| `DebugVisualizer.GO.Teleport` | uint32 | 180776 | |
| `DebugVisualizer.GO.Vertical` | uint32 | 180777 | |
| `DebugVisualizer.GO.Flag` | uint32 | 180778 | |
| `DebugVisualizer.GO.Physics` | uint32 | 181227 | |

Visualizer requires the master `AntiCheat.Enable` to be on (it visualizes the
detection pipeline). It is independent of `AntiCheat.Action` (never punishes).

## GM commands (Slice 2b) — `.anticheat`
- `.anticheat status [name]` — live decayed score + lifetime violations (BuildStatus).
- `.anticheat report [name]` — recent rows from `character_anticheat_violation`.
- `.anticheat reload` — re-read config snapshot (`AntiCheatMgr::LoadConfig`).
- `.anticheat visual on|off` — toggle the visualizer at runtime (sets the cached flag).
Registered via the ChatHandler command table (AHBot command group is the template);
default security GM level 2. Touches `Chat.h` + a command-table file + a new
`AntiCheatCommands.cpp`.

## Risks
- Marker spam on a populated realm → mitigated: OFF by default, trace opt-in,
  distance-rate-limited, short despawn. Intended for test realms.
- Wrong/invisible GO entry if user misconfigures → SummonGameObject returns NULL
  safely (logged once at debug); no crash.
- No gameplay effect: markers are generic GOs with no interaction.
```
