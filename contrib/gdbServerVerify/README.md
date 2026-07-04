# gdbServerVerify

Runtime verification harness for the GdbServer debug endpoint (see
[`doc/GdbServer.md`](../../doc/GdbServer.md)). It drives a **live** `mangosd`
over the endpoint's RSP and monitor ports to confirm that game-level
breakpoints behave correctly across thread boundaries.

## Requirements

- Python 3 (standard library only).
- A running `mangosd` with the endpoint enabled in `mangosd.conf`:
  ```
  GdbServer.Enable        = 1
  GdbServer.IP            = 127.0.0.1
  GdbServer.Port          = 2345
  GdbServer.MonitorPort   = 2346
  ```
- Run it against a **test** server. The harness arms breakpoints and opens
  throwaway TCP connections to the world port; it does not need a game client.

## Usage

```sh
# Default: TEST 1 (off-thread breakpoint must not stall the world) +
#          TEST 2 (on-thread breakpoint must stop and resume)
python gdb_verify.py

# Regression probe for the monitor-bridge socket lifetime (use-after-free)
python gdb_verify.py --crash-repro --crash-dir /path/to/mangosd/Crashes

# Non-default host/ports
python gdb_verify.py --host 127.0.0.1 --rsp-port 2345 --mon-port 2346 --world-port 8085
```

Exit code `0` means all selected checks passed.

## What it checks

- **TEST 1** — arms `netaccept` (fires on an ACE network thread) with a
  debugger attached, then pokes the world port. The world tick must keep
  advancing (an off-thread breakpoint must not drive the cooperative stop) and
  the hit counter must climb.
- **TEST 2** — arms `worldtick` (fires on the world thread). The world must
  enter the cooperative stop (tick freezes, RSP stop reply sent) and resume
  cleanly on `$c` + detach.
- **`--crash-repro`** — parks the world thread on a `worldtick` stop, then has
  monitor clients submit a line and disconnect immediately, so a queued request
  outlives its socket. A correct server survives; a use-after-free crashes it.
  PASS = survived with no new crash dump.
