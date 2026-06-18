# DebugVis labeled markers — design (Slice 13)

## Goal
Two enhancements to the `.debug vis` toolkit, from user testing:
1. **Better-suited marker objects** — replace the colored banners with clearer
   "engine debug-draw" visuals. Default to color-coded vertical light columns;
   offer floating crystals as a config-selectable alternate style.
2. **Hover shows verbose/debug info** — mousing over a marker shows that
   marker's own captured data (hit coords, path point index/type, ground delta…).

## Client constraint (the design driver)
- The 1.12.1 client caches gameobject name/type **by entry id** (it sends
  `CMSG_GAMEOBJECT_QUERY` once per entry and caches the response). So per-instance
  tooltip text requires a *distinct entry per labeled marker*.
- The rendered **model/colour** comes from the per-object `GAMEOBJECT_DISPLAYID`
  field, which IS per-instance — so colour does NOT need distinct entries.

## Design
**Colour (per-instance, no caching issue):** spawn the marker, then
`SetDisplayId(colour)` before adding it to the map. Colour chosen per category
from a style preset (`DebugVis.Style`: 0=beams default, 1=crystals), each
category overridable via `DebugVis.Disp.<Cat>`.

Beams preset (verified real display ids in mangos0):
| category        | display | object                  |
|-----------------|---------|-------------------------|
| cell            | 263     | blue light column       |
| los-ok / path   | 3993    | green light column      |
| los-block / bad | 327     | red crystal             |
| collision       | 363     | purple light column     |
| height          | 266     | yellow tall column      |
| hit point       | 6430    | "Cannon Target" reticle |
| generic         | 6679    | glowing circle          |

**Tooltip (per-instance, needs distinct entries):** a reserved pool of GO
template entries **305000–305511** (just above the real max 300153, so
`sGOStorage` barely grows), all `type=5` generic, inserted via SQL migration.
A ring allocator hands out the next pool entry per *labeled* marker; an
in-process `entry -> label` map stores the verbose string.
`HandleGameObjectQueryOpcode` consults the map and substitutes the label as the
GO name. Markers without a label (e.g. LoS ray fill dots) reuse a single shared
pool entry — they still get the right colour, just a generic tooltip.

**Known limitation:** the client cache is per-session and per-entry. After a
session places more than ~512 labeled markers, reused ring slots show stale
tooltip text until relog. Acceptable for a GM diagnostic; documented in config.

## Touch points
- `src/game/DebugVis/sql/debugvis_marker_pool.sql` — pool insert (applied to mangos0).
- `DebugVis.h/.cpp` — `DV_HITPOINT` category; `Marker(...label="")`; ring
  allocator + label map; `ColorDisplayId()`; `GetEntryLabel()` / `IsDebugEntry()`.
- `Object.h/.cpp` — `SummonGameObject(... , uint32 displayId)` overload.
- `QueryHandler.cpp` — label override hook.
- `DebugVisCommands.cpp` — pass verbose labels at each call site.

Config-gated by the existing `AntiCheat.Enable`/DebugVis gating; markers remain
diagnostic-only and auto-despawn. Default behaviour unchanged when off.
