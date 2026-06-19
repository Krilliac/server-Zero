# Bot-movement detection (Slice 28) — design

## Goal
Detect classic third-party movement bots from server-side movement packets, per
the game-bot-detection literature (Platzer ICICS'11 sequence/trajectory analysis;
the Lee survey; steering-behavior work). The user's framing: the **sharpness of
movement snapping/turning** — a bot snaps to a bearing then runs dead-straight to
a waypoint — while NOT flagging a human's legitimate sharp turn (the both-mouse-
button look-behind flip that reverses the run direction).

## Signatures used (all server-observable from MovementInfo)
1. **Snap-to-waypoint**: a sharp heading change (>~80deg between consecutive moving
   packets) that *ends a sustained straight run* (>=15 yd held within ~20deg). One
   such snap+run = a "clean cycle."
2. **Path straightness**: implicit in the straight-run requirement.
3. **Metronomic timing**: low coefficient of variation (CV) of inter-packet
   intervals (Welford running variance). Bots tick on a fixed clock; humans jitter.

## Decision (windowed, not per-packet)
Accumulate over a **30s window**; at window end flag `AC_VIOLATION_BOT` (weight 12)
only if ALL hold:
- `>= 50` moving samples and `>= 30` intervals (enough data),
- `>= 4` clean snap->straight cycles,
- inter-packet timing `CV < 0.15` (very regular).
Then reset the window.

## False-positive guards (incl. the both-button flip)
- **Windowed + repeated + regular**: a one-off human sharp turn is a single event,
  far below the 4-cycle bar; a look-behind flip also doesn't create a *new
  sustained straight run to a waypoint*, and human timing CV is well above 0.15.
- Straight-run requires both a low pre-snap turn variance AND >=15 yd distance, so
  combat strafing / wandering doesn't accumulate cycles.
- `AntiCheat.BotDetect` is **OFF by default** (heuristic); low weight + decay so
  even an occasional trip won't escalate without a sustained pattern.
- Playerbots are exempt (the movement AC path is skipped for exempt units, and
  server-AI bots don't send client movement packets anyway).

## Touch points
- `AntiCheatDefines.h`: `AC_VIOLATION_BOT = 14`.
- `World.h/.cpp` + `mangosd.conf`: `AntiCheat.BotDetect` (bool, off).
- `MovementAnticheat`: window/heading/interval state + the detector block in
  `HandlePositionUpdate` (uses the already-computed dx/dy/horiz/dtMS).
- `.anticheat test bot` injects the violation (the windowed pattern can't be
  simulated in one `.spoof` packet).
