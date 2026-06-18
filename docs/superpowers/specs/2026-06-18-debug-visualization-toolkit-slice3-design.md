# Server-Side Debug Visualization Toolkit — Slice 3 Design

Date: 2026-06-18
Branch: `feature/anticheat-detection-framework`

## Reframe

Per the user, the visualizer is **not anti-cheat-exclusive**. It is a general
**server-side debug-draw toolkit** — engine-style debug visualization rendered by
spawning temporary gameobjects the client can see. The anti-cheat movement
visualizer (Slice 2) becomes one *consumer* of this toolkit; the toolkit itself
visualises any server-authoritative spatial data.

## Module

`src/game/DebugVis/DebugVis.{h,cpp}` — primitive layer:
- `Marker(viewer, category, x,y,z)` — one temp GO (category → colour/model).
- `Line(viewer, category, p1, p2, spacing)` — markers along a 3D segment (raw,
  not ground-snapped; for rays).
- `DespawnSeconds()` — from config `DebugVis.DespawnSeconds` (default 45).
Categories: CELL, LOS_OK, LOS_BLOCK, PATH, PATH_BAD, COLLISION, HEIGHT, GENERIC →
existing colored Banner GOs (no DB changes; overridable later).

## Tools (GM commands) — on-demand now; toggle mode is a follow-up

Nested under the existing `.debug` table as `.debug vis ...` (SEC_GAMEMASTER):
- `.debug vis cells [radius]` — markers on the cell lattice (`SIZE_OF_GRID_CELL`)
  around you; shows the Map→Grid→Cell spatial backbone.
- `.debug vis los` — to selected target: green line if `Map::IsInLineOfSight`,
  else red line to the `GetHitPosition` blocking point.
- `.debug vis path` — `PathFinder` navmesh path to selected target; markers per
  point, green if a real path, red if incomplete/none (by `PathType`).
- `.debug vis collision [dist]` — forward raycast (`GetHitPosition`); marks the
  world-geometry hit ahead of you.
- `.debug vis height` — marks terrain/VMAP ground height (`Map::GetHeight`) at
  your position and reports the delta to your Z.

Plus Anti-Cheat GM commands (`.anticheat status|report|reload`).

## Trigger model
User chose **Both**: on-demand commands (this slice) + toggle/continuous mode for
suitable tools (cells, collision) as a later follow-up. Markers auto-despawn.

## Safety
GM-only; markers are non-interactive generic GOs (no gameplay effect); ray/line
markers capped (≤200) and cell radius capped (≤5) to prevent flooding; short
despawn. Intended for test realms.

## Future
Consolidate the Slice-2 anti-cheat `DebugVisualizer` onto these primitives; add
toggle mode; optionally lift category→GO entries into config; add liquid/area
and grid-occupancy visualizers.
