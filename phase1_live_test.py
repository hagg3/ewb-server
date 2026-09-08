#!/usr/bin/env python3
"""Live socket test for ROADMAP-SERVER stages 1.3-1.7 against ./edenserver.

    ./build_server.sh && python3 phase1_live_test.py [path/to/edenserver]

Rung 1.5 of the stage 1.8 verification ladder: real sockets, real server process,
no game client and no VuencLink. It starts ./edenserver twice on port 27099 in a
temp world dir (once plain, once with --legacy-snapshot) and asserts on the bytes.
Deliberately NOT wired into build_server.sh — it binds a port and spawns processes.

Covers the wire-order properties a unit test cannot see (the pure halves live in
protocol_test.cpp and region_test.cpp):
  1.3  join sequence: welcome -> CAPS:region -> SPAWN, no unsolicited ACTION dump
  1.4  PING -> PONG
  1.5  SIGNQ -> SIGNP burst (gated on JOIN, paced, no terminator)
  1.6  chat "quit" does not kick; charType 17 is echoed as 17
  1.7  name validation, duplicate names, ACTION extra validation + rate limit
"""
import os, shutil, socket, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "edenserver")
PORT = 27099

fails = []
def check(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        fails.append(msg)


class Client:
    def __init__(self, timeout=3.0):
        self.s = socket.create_connection(("127.0.0.1", PORT), timeout=timeout)
        self.s.settimeout(timeout)
        self.buf = b""

    def send(self, line):
        self.s.sendall((line + "\n").encode())

    def lines(self, wait=0.6, want=None):
        """Collect complete lines for `wait` seconds (or until `want` of them)."""
        out, deadline = [], time.time() + wait
        while time.time() < deadline:
            if want is not None and len(out) >= want:
                break
            self.s.settimeout(max(0.05, deadline - time.time()))
            try:
                b = self.s.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not b:
                self.closed = True
                break
            self.buf += b
            while b"\n" in self.buf:
                ln, self.buf = self.buf.split(b"\n", 1)
                out.append(ln.decode("utf-8", "replace"))
        return out

    def closed_by_peer(self, wait=1.0):
        deadline = time.time() + wait
        while time.time() < deadline:
            self.s.settimeout(max(0.05, deadline - time.time()))
            try:
                if self.s.recv(65536) == b"":
                    return True
            except socket.timeout:
                return False
            except OSError:
                return True
        return False

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def start(world_dir, *extra):
    p = subprocess.Popen([SERVER, "--port", str(PORT), "--connect-limit", "0", *extra],
                         cwd=world_dir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
            return p
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not come up")


def stop(p):
    p.terminate()
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill()


SIGNS = """\
# an operator-authored sidecar
65467:34:65115:0:27:0:step 3: burn the door

65500:40:65200:1:2:3:q
bogus line that is not a sign
65501:40:65201:0:0:0:Southwood (tentative name)
"""

def main():
    d = tempfile.mkdtemp(prefix="edenlive_")
    with open(os.path.join(d, "eden_signs.txt"), "w") as f:
        f.write(SIGNS)
    # Tight ACTION budget so the flood sub-test still exercises the token bucket —
    # the shipping default (512/s, 1024 burst) is sized for VuencLink's bulk-edit
    # drain rate and would pass a 400-line flood straight through (1.8 rung 2/3).
    p = start(d, "--verbose", "--action-rate", "8", "--action-burst", "64")
    try:
        run(d)
    finally:
        stop(p)

    # Second run over the same world dir: persistence, the opt-in legacy dump and
    # the per-IP connect limiter (which the first run disables so it can churn
    # connections on loopback).
    p = start(d, "--legacy-snapshot", "--connect-limit", "3")
    try:
        run_legacy(d)
    finally:
        stop(p)
        shutil.rmtree(d, ignore_errors=True)

    print()
    if fails:
        print(f"{len(fails)} check(s) FAILED")
        for f_ in fails:
            print("  - " + f_)
        return 1
    print("phase1_live_test: all checks passed")
    return 0


def run(d):
    # --- 1.3 join sequence ---------------------------------------------------
    print("[1.3] join sequence")
    c = Client()
    c.send("JOIN:Newcomer:17:")
    c.send("SIGNQ")
    ls = c.lines(1.0)
    check(ls and ls[0] == "[Server] Welcome, Newcomer! (Character Type: 17)",
          "welcome is the first line, charType echoed verbatim (1.6)")
    check(len(ls) > 1 and ls[1] == "CAPS:region", "CAPS:region is the second line")
    check(not any(x.startswith("SPAWN:") for x in ls), "no SPAWN for a name with no saved position")
    check(not any(x.startswith("ACTION:") for x in ls), "no unsolicited ACTION world dump")

    # --- 1.5 SIGNQ -> SIGNP --------------------------------------------------
    print("[1.5] SIGNQ -> SIGNP")
    signp = [x for x in ls if x.startswith("SIGNP:")]
    check(len(signp) == 3, f"the 3 well-formed sidecar lines became 3 SIGNP lines (got {len(signp)})")
    check(signp and signp[0] == "SIGNP:server:65467:34:65115:0:27:0:step 3: burn the door",
          "text-last: a ':' inside sign text survives")
    check(any(x.endswith(":Southwood (tentative name)") for x in signp), "long text intact")
    check(not any(x.strip() == "END" for x in ls), "the burst has no terminator")

    time.sleep(1.1)          # clear the pacing gap from the join-time SIGNQ
    c.send("SIGNQ")
    c.send("SIGNQ")          # ...and immediately spam a second, inside the gap
    n = len([x for x in c.lines(1.0) if x.startswith("SIGNP:")])
    check(n == 3, f"two back-to-back SIGNQ produce exactly one burst (got {n} lines)")

    # --- 1.4 PING -> PONG ----------------------------------------------------
    print("[1.4] PING -> PONG")
    c.send("PING")
    check(c.lines(1.0, want=1) == ["PONG"], "PING answers with a bare PONG")

    # --- 1.6 chat kill-switch is gone ---------------------------------------
    print("[1.6] chat")
    peer = Client()
    peer.send("JOIN:Peer:0:")
    peer.lines(0.6)
    c.lines(0.3)             # drain the "Peer has joined" broadcast
    c.send("MSG:quit")
    c.send("MSG:exit")
    c.send("PING")
    check("PONG" in c.lines(1.0), "typing quit/exit in chat does not kick")
    relay = peer.lines(0.5)
    check("[Newcomer (T17)] quit" in relay, "chat relays to a peer with the (T<n>) shape")
    c.send("MSG:hi\x01\x7fthere")
    check("[Newcomer (T17)] hithere" in peer.lines(0.5), "control bytes are stripped from chat")

    # --- 1.7 ACTION validation + rate limit ---------------------------------
    print("[1.7] ACTION")
    peer.lines(0.2)
    c.send("ACTION:65500:30:65500:3:255")     # paint 255 = the painted-base sentinel
    c.send("ACTION:65500:30:65501:3:99")      # past the palette
    c.send("ACTION:65500:30:65502:0:200")     # past the block table
    c.send("ACTION:65500:30:65503:0:13")      # legal
    got = [x for x in peer.lines(0.8) if x.startswith("ACTION:")]
    check(got == ["ACTION:Newcomer:17:65500:30:65503:0:13"],
          f"only the legal ACTION is applied and relayed (got {got})")

    flood = Client()
    flood.send("JOIN:Flooder:0:")
    flood.lines(0.5)
    peer.lines(0.2)
    for i in range(400):
        flood.send(f"ACTION:65600:30:{65600 + i}:0:13")
    n = len([x for x in peer.lines(1.5) if x.startswith("ACTION:")])
    check(0 < n <= 96, f"an ACTION flood is bounded by the token bucket (relayed {n} of 400)")
    flood.close()

    # --- 1.7 names -----------------------------------------------------------
    print("[1.7] names")
    for name, why in [("server", "reserved name"), ("bob] hi [server (T0)", "']' forging chat"),
                      ("", "empty name"), ("x" * 30, "over-long name")]:
        n1 = Client()
        n1.send(f"JOIN:{name}:0:")
        got = n1.lines(0.8)
        check(any(x.startswith("[Server] Invalid name") for x in got) and n1.closed_by_peer(),
              f"{why!r} is refused and disconnected")
        n1.close()

    dup = Client()
    dup.send("JOIN:Newcomer:0:")
    got = dup.lines(0.8)
    check(got == ["[Server] Name already in use."] and dup.closed_by_peer(),
          f"a duplicate connected username is refused (got {got})")
    dup.close()

    # --- 1.5 / 1.1 gating on JOIN -------------------------------------------
    print("[gating]")
    anon = Client()
    anon.send("SIGNQ")
    anon.send("REGION:65500:65500")
    check(not anon.lines(0.8), "SIGNQ and REGION before JOIN are silent")
    anon.send("PING")
    check(anon.lines(1.0, want=1) == ["PONG"], "...but PING works unauthenticated (no amplification)")
    anon.close()

    # --- 1.8 empty region answers with an explicit SNAPZ:0 frame -----------
    print("[1.8] empty region")
    c.send("REGION:70000:70000")   # nothing built out here
    sn = [x for x in c.lines(1.5) if x.startswith("SNAPZ:")]
    check(sn == ["SNAPZ:0:AwA"] or (sn and sn[0].startswith("SNAPZ:0:")),
          f"an unbuilt region is answered with a well-formed SNAPZ:0 frame, not silence (got {sn})")

    # --- 1.3 SPAWN on rejoin -------------------------------------------------
    print("[1.3] SPAWN")
    c.send("POSVEL:65530.5:33.0:65540.5:0:0:0")
    time.sleep(0.3)
    c.close()
    peer.close()
    time.sleep(0.4)
    again = Client()
    again.send("JOIN:Newcomer:0:")
    ls = again.lines(1.0)
    check(len(ls) >= 3 and ls[0].startswith("[Server] Welcome") and ls[1] == "CAPS:region"
          and ls[2].startswith("SPAWN:"), f"welcome -> CAPS -> SPAWN, in that order (got {ls[:3]})")
    check(ls[2] == "SPAWN:65530.50:33.00:65540.50", f"SPAWN carries the saved position ({ls[2]})")
    again.close()

    time.sleep(0.3)
    return


def run_legacy(d):
    # --- persistence + the opt-in legacy dump --------------------------------
    print("[1.3] --legacy-snapshot / persistence")
    c = Client()
    c.send("JOIN:Newcomer:0:")
    ls = c.lines(1.5)
    acts = [x for x in ls if x.startswith("ACTION:")]
    check(ls[:2] == ["[Server] Welcome, Newcomer! (Character Type: 0)", "CAPS:region"],
          "welcome and CAPS still lead, dump or no dump")
    check("ACTION:server:0:65500:30:65503:0:13" in acts,
          f"the edit from the previous run survived a restart ({len(acts)} dump lines)")
    check("ACTION:server:0:65500:30:65503:1" in acts,
          "a server-pushed solid cell is mined before it is built (plan §0.5.3)")
    check(acts.index("ACTION:server:0:65500:30:65503:1") <
          acts.index("ACTION:server:0:65500:30:65503:0:13"), "...in that order")

    # The same cells over REGION, which is the path the real client actually uses.
    # Every persisted cell is a solid type-13 with no paint, so the dump is two
    # lines per cell (mine+build) and the SNAPZ burst is one record per cell.
    cells = len(acts) // 2
    c.send("REGION:65500:65500")
    snapz = [x for x in c.lines(1.5) if x.startswith("SNAPZ:")]
    check(len(snapz) == 1 and snapz[0].startswith(f"SNAPZ:{cells}:"),
          f"REGION answers with all {cells} persisted cells in one frame (got {snapz[0][:16] if snapz else None})")
    c.close()

    # --- 1.7 per-IP connect limit -------------------------------------------
    print("[1.7] connect limit")
    held = []
    refused = 0
    for _ in range(6):
        try:
            k = Client(timeout=1.0)
            if k.closed_by_peer(0.4):
                refused += 1
            held.append(k)
        except OSError:
            refused += 1
    check(refused >= 2, f"a connect flood past --connect-limit 3 is closed immediately ({refused}/6)")
    for k in held:
        k.close()


if __name__ == "__main__":
    sys.exit(main())
