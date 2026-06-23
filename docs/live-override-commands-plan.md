# Implementation Plan: Live Client-State Override Command Suite (server-Zero)

> **Handoff note:** This document is self-contained. It is written so a fresh Claude
> session (e.g. Claude on desktop) can open the server-Zero repo and begin work
> without the prior conversation. All file paths are relative to the repo root
> (`/home/user/server-Zero` in the session it was authored in). server-Zero is a
> **MaNGOS-Zero** emulator targeting the **WoW 1.12.x** client.

---

## Context — what we're building and why

We want a coherent suite of **GM/admin chat commands that push live, client-visible
state changes** — lighting/skybox, music, sound, weather, spell visuals, cinematics,
on-screen text, and world-state HUD counters — culminating in an `.event` meta-command
that fires several at once for coordinated server events.

**Why this approach (the key architectural fact):** WoW content lives in three tiers
with very different client relationships:

1. **DBC** (`dbc/*.dbc` → `DBCStorage<T>` in RAM): the **client has its own copy** in
   its MPQ, reads it once at load, and never accepts it from the server. Changing
   server-side DBC does **not** change the client UI. (Not what we're doing here.)
2. **World DB templates** (MySQL `creature_template`, `item_template`, …): client
   requests these at runtime via `SMSG_*_QUERY_RESPONSE`; already live-editable.
3. **Live-override `SMSG_*` packets**: the client already knows how to render the
   effect (light, weather, sound, visual); the server just sends an ID/command and the
   client reacts **instantly, with no client patch**.

This plan targets **tier 3** exclusively — the things that work live on the stock
1.12 client. The headline primitive (`SMSG_OVERRIDE_LIGHT`) is currently unimplemented
in the codebase, so building it unlocks the whole suite.

---

## Background: the pattern we're extending

Every command is the same shape: **command → send helper → `SMSG_*` → client reacts.**

Canonical existing template — the weather command:
```cpp
// src/game/ChatCommands/GMCommands.cpp:571  HandleChangeWeatherCommand
ExtractUInt32(&args, type);  ExtractFloat(&args, grade);
player->GetMap()->SetWeather(zoneId, (WeatherType)type, grade, false);  // → SMSG_WEATHER (zone-wide)
```

Simpler "send to one player" form:
```cpp
// src/game/ChatCommands/DebugCommands.cpp
m_session->GetPlayer()->SendCinematicStart(dwId);            // SMSG_TRIGGER_CINEMATIC
m_session->GetPlayer()->SendUpdateWorldState(world, state);  // SMSG_UPDATE_WORLD_STATE
```

### How commands are registered
Chat commands are static `ChatCommand` entries in `Chat.cpp::getCommandTable()`.
Each entry: `{ name, securityLevel, allowConsole, &ChatHandler::HandlerFn, "", subTable }`.
Handler methods are declared in `src/game/WorldHandlers/Chat.h` and implemented in
`src/game/ChatCommands/*.cpp`. There is a reserved placeholder file
`src/game/ChatCommands/ZZZ_CustomCommands.cpp` for custom commands.

### Existing infrastructure inventory (reuse, don't reinvent)
| Effect | Helper (location) | SMSG | Existing command |
|---|---|---|---|
| Weather | `Map::SetWeather` (Map.cpp:1960) | `SMSG_WEATHER` | `.wchange` (GMCommands.cpp:571) |
| Sound (to player/positional) | `Object::PlayDirectSound` (Object.h:905) | `SMSG_PLAY_SOUND` | `.debug playsound` (DebugCommands.cpp:502) |
| Sound from object | `Object::PlayDistanceSound` / object-sound (Object.cpp:2966) | `SMSG_PLAY_OBJECT_SOUND` | none |
| Music | `Object::PlayMusic` (Object.h:906, Object.cpp:3007) | `SMSG_PLAY_MUSIC` | none |
| Cinematic | `Player::SendCinematicStart` | `SMSG_TRIGGER_CINEMATIC` | `.debug play cinematic` (DebugCommands.cpp:446) |
| World-state HUD | `Player::SendUpdateWorldState` | `SMSG_UPDATE_WORLD_STATE` | `.debug updateworldstate` (DebugCommands.cpp:422) |
| **Light / skybox** | **none (to build)** | `SMSG_OVERRIDE_LIGHT` | **none** |

`SMSG_OVERRIDE_LIGHT` is registered but never sent — and flagged with a dev TODO:
```cpp
// src/game/Server/Opcodes.cpp:972
OPCODE(SMSG_OVERRIDE_LIGHT, STATUS_NEVER, PROCESS_INPLACE, &WorldSession::Handle_ServerSide); // 0x411: @TODO need to check usage in vanilla WoW
```

---

## Implementation

### Step 0 — Shared target-scope helper (do this first)
Every command needs a consistent way to choose **who** receives the effect. Build one
small helper and reuse it everywhere.

- Add a helper (e.g. in `Chat`/`ChatHandler`, or a free function in the new command
  file) that parses a trailing scope token and returns the recipient set:
  - `self` → `m_session->GetPlayer()`
  - `target` → `getSelectedUnit()` / selected player (pattern: DebugCommands.cpp:519)
  - `zone` → all players in `player->GetZoneId()` (pattern: `Map::SetWeather` iterates the zone)
  - `server` → all sessions (pattern: world broadcast helpers in `World`/`sWorld`)
- Default scope when omitted: **self** (safe), except weather/light which are naturally
  zone-scoped.
- A unicast helper `SendPacketToScope(WorldPacket&, scope)` keeps each command tiny.

### Step 1 — `SMSG_OVERRIDE_LIGHT` send helper (headline primitive)
- Add `void Player::SendOverrideLight(uint32 overrideLightId, uint32 fadeInMs)` in
  `src/game/Object/Player.{h,cpp}` (mirror the style of `SendCinematicStart`).
- Build the packet:
  ```cpp
  WorldPacket data(SMSG_OVERRIDE_LIGHT, 12);
  data << uint32(currentZoneLightId);   // VERIFY: see open question below
  data << uint32(overrideLightId);      // ID from client Light.dbc (client-side, no server load needed)
  data << uint32(fadeInMs);
  SendPacket(&data);
  ```
- **Note:** the server does NOT need `Light.dbc` loaded — it only transmits the uint32
  ID; the client resolves it against its own `Light.dbc`.
- **Open question to resolve during implementation:** the exact 1.12 field layout of
  `SMSG_OVERRIDE_LIGHT` (count/order of the light IDs and whether the "current light"
  field is required). Verify against a 1.12 client / packet logs or the vmangos source
  (vmangos implements override light for vanilla and is the best reference). This is
  what the in-code TODO at Opcodes.cpp:972 is flagging.

### Step 2 — The command file and registrations
- Create `src/game/ChatCommands/EventCommands.cpp` (or extend
  `ZZZ_CustomCommands.cpp`). Add it to the game CMake target if a new file
  (`src/game/CMakeLists.txt` / the ChatCommands glob).
- Declare each handler in `src/game/WorldHandlers/Chat.h`.
- Register under a parent command table in `Chat.cpp::getCommandTable()`, e.g. a
  top-level `event` command with subcommands, plus convenient top-level aliases.
  Suggested security: `SEC_GAMEMASTER` for cosmetic effects, `SEC_ADMINISTRATOR` for
  server-wide scope.

### Step 3 — Command specs (build in this order)

**Atmosphere**
- `.light <lightId> [fadeMs] [scope]` → `Player::SendOverrideLight` *(new, Step 1)*.
  Live day/night, blood-moon, eclipse, dungeon ambiance.
- `.music <soundId> [scope]` → `Object::PlayMusic` (helper exists; just expose + scope).
- `.zonesound <soundId>` / `.sound <soundId> [scope]` → `Object::PlayDirectSound`
  (broaden the existing debug command to zone/server scope).
- `.weather <type> <grade> [server]` → wrap existing `Map::SetWeather`; add a `server`
  scope that loops all zones with weather chances.

**Spectacle**
- `.spellvisual <visualId> [scope]` → build `SMSG_PLAY_SPELL_VISUAL` (target GUID +
  visual id). Currently unused as a command.
- `.cinematic <id>` / `.movie <id>` → promote from `.debug` (movie is post-vanilla;
  guard like the existing `#if defined(TBC)...` in DebugCommands.cpp:473).
- `.zoneattack <zoneId>` → build `SMSG_ZONE_UNDER_ATTACK` (red flash + map ping).

**HUD / world / text**
- `.worldstate <field> <value> [zone]` → `Player::SendUpdateWorldState`; optionally
  `SMSG_INIT_WORLD_STATES` to spawn a new on-screen counter.
- `.timespeed <speed>` / `.daynight <freeze|speed>` → `SMSG_LOGIN_SETTIMESPEED`
  (fast-forward/freeze day-night cycle).
- `.screenmsg <text> [scope]` → `SMSG_AREA_TRIGGER_MESSAGE` (big center text);
  optional variants for `SMSG_NOTIFICATION` (toast) and `SMSG_SERVER_MESSAGE`.

### Step 4 — `.event` capstone (meta-command)
- A command that composes the primitives into named presets, e.g.
  `.event bloodmoon [scope]` = override light (red) + weather (fog) + music +
  zone-wide screen message + a world-state countdown.
- Implement presets as a small in-code table (name → list of effect calls) so adding a
  new event is a few lines. Optionally make presets DB-driven later (a `game_event_fx`
  table) for no-recompile authoring — but in-code table is the v1.

---

## Files to create / modify (summary)
| Purpose | File |
|---|---|
| New: override-light send helper | `src/game/Object/Player.h` + `Player.cpp` |
| New: command implementations + scope helper | `src/game/ChatCommands/EventCommands.cpp` (new) |
| Modify: handler declarations | `src/game/WorldHandlers/Chat.h` |
| Modify: command table registration | `src/game/WorldHandlers/Chat.cpp` (`getCommandTable`) |
| Modify (if new file): build | `src/game/CMakeLists.txt` |
| Reference only (existing helpers to reuse) | `Object.h/.cpp` (Play*), `Map.cpp` (SetWeather), `DebugCommands.cpp` (patterns) |
| Reference only (opcode + TODO) | `src/game/Server/Opcodes.cpp:972`, `Opcodes.h` |

No DBC changes, no DB schema changes required for v1 (all effects are live SMSG).

---

## Verification
1. **Build:** CMake project; rebuild the `game`/`mangosd` targets. Confirm the new file
   compiles and links and the commands appear in the command table.
2. **Manual, in-game (best signal):** connect a 1.12 client as a GM account, then:
   - `.light <id>` — confirm skybox/lighting changes; test fade and reverting.
   - `.music <id>` / `.sound <id>` — confirm audio plays for the chosen scope.
   - `.weather`, `.spellvisual`, `.cinematic`, `.zoneattack`, `.screenmsg`,
     `.worldstate` — confirm each effect renders.
   - Test each **scope** (self / target / zone / server) and confirm only the intended
     recipients see it.
   - `.event bloodmoon` — confirm all sub-effects fire together.
3. **Packet correctness:** for `SMSG_OVERRIDE_LIGHT`, if the effect doesn't render,
   re-verify the field layout against vmangos / a packet capture (the open question in
   Step 1). Use the existing `.debug send opcode` tooling (DebugCommands.cpp:307) to
   experiment with raw payloads.

---

## Risks / things to confirm during implementation
- **`SMSG_OVERRIDE_LIGHT` layout** is the main unknown (see Step 1). Resolve before
  building dependent commands; everything else uses confirmed existing helpers.
- **Scope = server** should be `SEC_ADMINISTRATOR`-gated to avoid griefing.
- **Sound/music spam**: throttle or restrict server-scope audio commands.
- `.movie` is not a vanilla opcode — keep it guarded by client-version macros as the
  existing code does.

---

## Appendix — adjacent extension tracks (context from the planning conversation)
These were explored alongside this suite and are documented here so the desktop session
has the full map. They are **separate future work**, not part of v1.

- **DB-backed DBC overrides (server-side).** The engine already exposes a runtime
  mutation hook: `DBCStorage::SetEntry(id, T*)` at
  `src/shared/DataStores/DBCStore.h:129` (LookupEntry checks the override map first).
  A `LoadDBCOverridesFromDB()` pass after `LoadDBCStores()` (DBCStores.cpp:262) plus
  override tables (e.g. `spell_dbc`) and `.dbc set` / `.reload` commands (pattern:
  `ReloadCommands.cpp`) would allow live server-side DBC edits **without recompiling**.
  Caveat: changes server *behavior* only — the client still renders from its own DBC,
  so expect desync unless a client patch MPQ is also shipped. Rebuild derived caches
  (e.g. `SpellMgr`) after override.
- **Handler modifications.** ~311 live `HandleXxxOpcode` functions in
  `src/game/WorldHandlers/*Handler.cpp` accept in-place custom checks/content (inject a
  guard clause before the commit line). `HandleMessagechatOpcode` (ChatHandler.cpp:111)
  is the richest: intercept `LANG_ADDON` messages with a custom prefix to build a
  client↔server protocol that works on the **stock** client (paired with a Lua UI
  addon) — the legitimate substitute for inventing new CMSG opcodes.
- **Query-response content** (creature/item/GO/quest) is already DB-driven and
  live-pushable via `SMSG_*_QUERY_RESPONSE` (QueryHandler.cpp / ItemHandler.cpp:371) —
  add `.modify`-style commands and the client sees changes on re-query.
