#!/usr/bin/env python3
"""
GdbServer endpoint verification harness (MaNGOS Zero).

Drives a LIVE mangosd through the built-in GDB debug endpoint (see
doc/GdbServer.md) to check that game-level breakpoints behave correctly across
thread boundaries. Requires the endpoint enabled in mangosd.conf
(GdbServer.Enable = 1) and should be run against a TEST server: it arms
breakpoints and opens throwaway TCP connections to the world port.

Default mode runs two checks:

  TEST 1  Off-thread breakpoint (`netaccept`, which fires on an ACE network
          thread) must NOT pause the world. With correct thread confinement
          the world tick keeps advancing and the hit is still counted. A
          regression here means a network/DB-thread breakpoint drives the
          cooperative stop from the wrong thread (racing the RSP state and
          failing to actually pause the world).

  TEST 2  On-thread breakpoint (`worldtick`, which fires on the world thread)
          MUST enter the cooperative stop: the world tick freezes, an RSP stop
          reply is sent, and the target resumes on `$c` + detach.

--crash-repro runs a regression probe for the monitor-bridge socket lifetime.
  It parks the world thread on a `worldtick` stop, submits monitor lines from
  throwaway connections that close immediately (so a queued request outlives
  its socket), then resumes. A correct server survives; a server that keeps a
  raw, unreferenced socket pointer for the queued request crashes with a
  use-after-free when the world thread drains it. PASS = survived with no new
  crash dump.

Attach model: opening a TCP socket to the RSP port alone marks a debugger as
attached (breakpoints only fire while attached). The monitor bridge is a
plain-text line protocol ("mangos <verb>").
"""
import argparse
import glob
import os
import socket
import sys
import time

# Filled in from argv in main(); defaults match doc/GdbServer.md.
HOST = "127.0.0.1"
RSP = 2345
MON = 2346
WORLD = 8085
CRASH_DIR = "Crashes"


def crash_dumps():
    return sorted(glob.glob(os.path.join(CRASH_DIR, "*.txt")))


def server_alive():
    """The world port accepting connections == mangosd process alive."""
    try:
        c = socket.create_connection((HOST, WORLD), timeout=2)
        c.close()
        return True
    except OSError:
        return False


# ------------------------------------------------------------------ monitor ---
class Mon:
    """Persistent monitor-bridge connection (plain-text lines)."""

    def __init__(self, timeout=2.0):
        self.s = socket.create_connection((HOST, MON), timeout=timeout)
        self.s.settimeout(timeout)
        self.read(0.8)                          # greeting banner

    def send(self, cmd):
        self.s.sendall((cmd + "\n").encode())

    def read(self, timeout=2.0, quiet=0.3):
        """Accumulate reply bytes until `quiet`s of silence or `timeout`s."""
        out, deadline = b"", time.time() + timeout
        while time.time() < deadline:
            self.s.settimeout(min(quiet, max(0.05, deadline - time.time())))
            try:
                d = self.s.recv(4096)
                if not d:
                    break
                out += d
            except socket.timeout:
                if out:
                    break
        return out.decode(errors="replace")

    def cmd(self, c, timeout=2.0):
        self.send(c)
        return self.read(timeout)

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def parse_tick(text):
    for tok in text.split():
        if tok.startswith("world-tick="):
            try:
                return int(tok.split("=", 1)[1])
            except ValueError:
                return None
    return None


def parse_hits(text):
    for line in text.splitlines():
        if "hits=" in line:
            try:
                return int(line.split("hits=", 1)[1].strip())
            except ValueError:
                return 0
    return 0


# ---------------------------------------------------------------------- rsp ---
def rsp_pkt(p):
    return f"${p}#{sum(p.encode()) & 0xFF:02x}".encode()


def rsp_attach():
    s = socket.create_connection((HOST, RSP), timeout=5)
    s.settimeout(2.0)
    time.sleep(0.6)                             # let the world tick consume the reset
    return s


def rsp_drain(s, timeout=2.0):
    s.settimeout(timeout)
    out = b""
    try:
        while True:
            d = s.recv(4096)
            if not d:
                break
            out += d
    except socket.timeout:
        pass
    return out


def poke_world(n):
    ok = 0
    for _ in range(n):
        try:
            c = socket.create_connection((HOST, WORLD), timeout=2)
            time.sleep(0.05)
            c.close()
            ok += 1
        except OSError as e:
            print(f"   (poke failed: {e})")
        time.sleep(0.05)
    return ok


def banner(t):
    print("\n" + "=" * 68 + f"\n{t}\n" + "=" * 68)


# ==================================================================== tests ===
def run_tests():
    m = Mon()
    banner("BASELINE")
    t0 = parse_tick(m.cmd("mangos tick"))
    print(f"world-tick={t0}   breakpoints:\n{m.cmd('mangos break list').rstrip()}")

    banner("ATTACH RSP DEBUGGER (socket alone => a debugger is attached)")
    rsp = rsp_attach()
    print("attached.")

    # ------------------------------------------------------------- TEST 1
    banner("TEST 1  OFF-THREAD netaccept (ACE net thread) must NOT stall world")
    print("arm:", m.cmd("mangos break netaccept").strip())
    t_a = parse_tick(m.cmd("mangos tick"))
    h_a = parse_hits(m.cmd("mangos break list"))
    print(f"before: world-tick={t_a}  hits={h_a}")
    print("poking the world port to fire netaccept on ACE threads...")
    n = poke_world(12)
    time.sleep(1.0)
    t_b = parse_tick(m.cmd("mangos tick"))
    h_b = parse_hits(m.cmd("mangos break list"))
    print(f"after ({n} connects): world-tick={t_b}  hits={h_b}")
    alive = t_a is not None and t_b is not None and t_b > t_a
    seen = h_b > h_a
    t1 = alive and seen
    print(f"  world kept ticking during off-thread bp : {alive}  ({t_a} -> {t_b})")
    print(f"  hit counted (bp fired, no stall)        : {seen}   ({h_a} -> {h_b})")
    print(f"  >>> TEST 1 {'PASS' if t1 else 'FAIL'}")
    m.cmd("mangos break clear")

    # ------------------------------------------------------------- TEST 2
    banner("TEST 2  ON-THREAD worldtick (world thread) must stop and resume")
    t_c = parse_tick(m.cmd("mangos tick"))
    print(f"world-tick before arm: {t_c}")
    print("arm:", m.cmd("mangos break worldtick").strip())
    time.sleep(1.0)                             # next tick fires the bp -> stop

    # (a) RSP stop reply proves the cooperative stop engaged.
    stop_reply = rsp_drain(rsp, timeout=2.0)
    print(f"  RSP stop-reply bytes while stopped : {len(stop_reply)}  {stop_reply[:24]!r}")

    # (b) Monitor probe on the persistent connection: no reply while stopped
    #     (the request is queued and only drained on the parked world thread).
    #     The socket stays OPEN so its queued request is not torn down early.
    m.send("mangos tick")
    stalled_reply = m.read(timeout=1.5)
    unresponsive = parse_tick(stalled_reply) is None
    print(f"  monitor unresponsive during stop   : {unresponsive}")
    stopped = len(stop_reply) > 0 and unresponsive

    # (c) Resume with $c, then detach (close RSP) so the re-firing worldtick
    #     bp is rejected because no debugger is attached.
    rsp.sendall(rsp_pkt("c"))
    time.sleep(0.15)
    rsp.close()
    print("  sent $c and detached (closed RSP socket)")

    # (d) The queued probe drains on the first post-resume tick; its reply
    #     arriving now is positive proof of resume.
    deferred = m.read(timeout=3.0)
    t_d = parse_tick(deferred)
    print(f"  deferred monitor reply post-resume : world-tick={t_d}")

    time.sleep(1.0)
    t_e = parse_tick(m.cmd("mangos tick"))
    print(f"  world-tick 1s later                : {t_e}")
    resumed = (t_d is not None and t_e is not None and t_e > t_d
               and t_d >= (t_c or 0))
    print(f"  world resumed and advancing        : {resumed}  ({t_c} -> stop -> {t_d} -> {t_e})")

    print("clear:", m.cmd("mangos break clear").strip())
    alive_end = server_alive()
    print(f"  server alive at end of TEST 2      : {alive_end}")
    t2 = stopped and resumed and alive_end
    print(f"  >>> TEST 2 {'PASS' if t2 else 'FAIL'}")
    m.close()

    banner("RESULT")
    print(f"TEST 1 (off-thread breakpoint, no world stall): {'PASS' if t1 else 'FAIL'}")
    print(f"TEST 2 (on-thread breakpoint, stop + resume)  : {'PASS' if t2 else 'FAIL'}")
    ok = t1 and t2
    print("\nOVERALL:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


# ============================================================== crash repro ===
def stop_via_worldtick(m):
    """Arm worldtick on persistent mon `m` with an RSP socket attached; return
    the RSP socket once the stop reply proves the world thread is parked."""
    rsp = rsp_attach()
    m.cmd("mangos break worldtick")
    time.sleep(1.0)
    reply = rsp_drain(rsp, timeout=2.0)
    print(f"  stop engaged (RSP stop-reply {len(reply)} bytes)")
    return rsp


def resume_and_detach(rsp):
    try:
        rsp.sendall(rsp_pkt("c"))
        time.sleep(0.15)
    except OSError:
        pass
    rsp.close()


def run_crash_repro():
    dumps_before = crash_dumps()

    banner("CRASH-REPRO PHASE A (control): stop + resume, no monitor traffic "
           "during the stop")
    m = Mon()
    rsp = stop_via_worldtick(m)
    resume_and_detach(rsp)
    time.sleep(2.5)
    a_alive = server_alive()
    print(f"  server alive after control resume : {a_alive}")
    if a_alive:
        m.cmd("mangos break clear")
    m.close()
    if not a_alive:
        print("  !! control run killed the server: the resume/detach path "
              "itself is at fault - investigate before trusting Phase B")
        return 1

    banner("CRASH-REPRO PHASE B (trigger): monitor connect + send + CLOSE "
           "while stopped, then resume")
    m = Mon()
    rsp = stop_via_worldtick(m)
    print("  submitting monitor lines from throwaway connections, closing them "
          "immediately (a queued request now outlives its socket)...")
    for _ in range(2):
        try:
            s = socket.create_connection((HOST, MON), timeout=2)
            s.sendall(b"mangos tick\n")
            time.sleep(0.1)                     # let handle_input queue the line
            s.close()
        except OSError as e:
            print(f"   (throwaway probe failed: {e})")
        time.sleep(0.2)
    time.sleep(0.5)                             # reactor processes the closes
    resume_and_detach(rsp)
    time.sleep(3.0)
    b_alive = server_alive()
    new_dumps = [d for d in crash_dumps() if d not in dumps_before]
    print(f"  server alive after trigger resume : {b_alive}")
    print(f"  new crash dumps                    : {new_dumps or 'none'}")
    if b_alive:
        m.cmd("mangos break clear")
    m.close()

    banner("CRASH-REPRO VERDICT")
    survived = b_alive and not new_dumps
    if survived:
        print("PASS: the server survived a monitor client disconnecting while a "
              "request was\n      queued behind a stopped world thread - the "
              "monitor-bridge socket\n      lifetime is safe.")
        return 0
    print("FAIL: the dangling-monitor-socket scenario killed the server - the "
          "monitor\n      bridge dereferences a queued request whose socket was "
          "already freed\n      (use-after-free).")
    return 1


# ==============================================================================
def main():
    global HOST, RSP, MON, WORLD, CRASH_DIR
    ap = argparse.ArgumentParser(description="GdbServer endpoint verification harness")
    ap.add_argument("--host", default=HOST)
    ap.add_argument("--rsp-port", type=int, default=RSP)
    ap.add_argument("--mon-port", type=int, default=MON)
    ap.add_argument("--world-port", type=int, default=WORLD)
    ap.add_argument("--crash-dir", default=CRASH_DIR,
                    help="directory mangosd writes crash dumps to (for "
                         "--crash-repro); default './Crashes' relative to the "
                         "server's working directory")
    ap.add_argument("--crash-repro", action="store_true",
                    help="run the monitor-bridge use-after-free regression probe")
    args = ap.parse_args()

    HOST = args.host
    RSP = args.rsp_port
    MON = args.mon_port
    WORLD = args.world_port
    CRASH_DIR = args.crash_dir

    if not server_alive():
        print(f"error: no world server listening on {HOST}:{WORLD} - start "
              f"mangosd with GdbServer.Enable = 1 first.", file=sys.stderr)
        return 2

    return run_crash_repro() if args.crash_repro else run_tests()


if __name__ == "__main__":
    sys.exit(main())
