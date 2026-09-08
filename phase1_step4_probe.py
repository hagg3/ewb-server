#!/usr/bin/env python3
"""PHASE1-CLOSEOUT Step 4 — the socket-observable half of the 1.9 probes.

    ./build_server.sh && python3 phase1_step4_probe.py [path/to/edenserver]

Step 4 in PHASE1-CLOSEOUT.md is "Human: 1.9 deliberate probes". Three of its four
items have a half that needs *eyes on the iOS game client* (does it visibly
teleport / display a close string / render a bigger disc) and a half that is pure
wire behaviour. This script does the wire half of all four, spawning ./edenserver
in a temp world dir on port 27097 exactly like phase1_live_test.py, and writes a
timestamped report to logs/phase1-step4-<date>.log.

What it CANNOT do (stays in phase1_step4_manual.md, needs the game on screen):
  - whether the game client *visually* teleports on an operator SPAWN
    (there is no operator SPAWN path in the server yet — see probe A)
  - whether the game client *displays* "[Server] Invalid name (...)" /
    "Name already in use." or fails silently
  - whether the game client's *rendered* world radius tracks --region-radius

Probes:
  A  SPAWN            rejoin-spawn from a saved position (wire); connected-client
                      teleport has no server path -> documented as a gap
  B  chat flood       MSG: has no rate limiter -> N lines in, N lines relayed,
                      no "throttled" log (constrains plan risk #16 / SIGNP pacing)
  C  --region-radius  A/B/default: the REGION reply box span scales with the flag
  D  name rejection   reserved / malformed / duplicate -> exact server strings
"""
import datetime as _dt
import os, re, shutil, socket, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "edenserver")
PORT = 27097
LOGDIR = os.path.join(HERE, "logs")

_report = []
_fails = []


def say(line=""):
    print(line)
    _report.append(line)


def check(cond, msg):
    say(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        _fails.append(msg)


class Client:
    def __init__(self, timeout=3.0):
        self.s = socket.create_connection(("127.0.0.1", PORT), timeout=timeout)
        self.s.settimeout(timeout)
        self.buf = b""
        self.closed = False

    def send(self, line):
        self.s.sendall((line + "\n").encode())

    def lines(self, wait=0.6, want=None):
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


class Server:
    """Spawns ./edenserver, tees its stdout so probes can assert on log lines."""

    def __init__(self, world_dir, *extra):
        self.p = subprocess.Popen(
            [SERVER, "--port", str(PORT), "--connect-limit", "0", "--verbose", *extra],
            cwd=world_dir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.out = []
        import threading
        self._t = threading.Thread(target=self._pump, daemon=True)
        self._t.start()
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("server did not come up")

    def _pump(self):
        for ln in self.p.stdout:
            self.out.append(ln.rstrip("\n"))

    def log_matching(self, pat):
        rx = re.compile(pat)
        return [ln for ln in list(self.out) if rx.search(ln)]

    def stop(self):
        self.p.terminate()
        try:
            self.p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.p.kill()
        time.sleep(0.2)


def _world():
    d = tempfile.mkdtemp(prefix="edenstep4_")
    with open(os.path.join(d, "eden_signs.txt"), "w") as f:
        f.write("65536:34:65536:0:0:0:step4 fixture\n")
    return d


# --- A: SPAWN ---------------------------------------------------------------
def probe_spawn(d):
    say("\n[A] SPAWN — rejoin position + connected-client teleport gap")
    srv = Server(d)
    try:
        c = Client()
        c.send("JOIN:Wanderer:17:")
        c.lines(0.8)
        c.send("POSVEL:65512.5:40.0:65590.5:0:0:0")
        time.sleep(0.3)
        c.close()
        time.sleep(0.4)
        again = Client()
        again.send("JOIN:Wanderer:0:")
        ls = again.lines(1.0)
        sp = [x for x in ls if x.startswith("SPAWN:")]
        check(sp == ["SPAWN:65512.50:40.00:65590.50"],
              f"rejoin replays the saved position as SPAWN (got {sp})")
        # A connected client should never get an unsolicited SPAWN today: there is
        # no operator path that unicasts one. Confirm the silence so the manual
        # step knows it is testing a not-yet-built feature.
        idle = [x for x in again.lines(1.0) if x.startswith("SPAWN:")]
        check(idle == [],
              "a connected client receives no further SPAWN (no operator teleport path exists)")
        again.close()
    finally:
        srv.stop()
    say("  NOTE: connected-client SPAWN teleport is unimplemented server-side. To make")
    say("        Step 4 item 1 testable end-to-end, add a one-shot --debug-spawn x:y:z")
    say("        (or an admin socket) that calls the existing SPAWN unicast. Not done here")
    say("        (agent kept out of server logic this pass).")


# --- B: chat flood ---------------------------------------------------------
def probe_chat_flood(d):
    say("\n[B] chat flood — is MSG rate-limited?")
    srv = Server(d)
    try:
        a = Client(); a.send("JOIN:Talker:17:"); a.lines(0.6)
        b = Client(); b.send("JOIN:Listener:0:"); b.lines(0.6)
        a.lines(0.3)
        N = 20
        t0 = time.time()
        for i in range(N):
            a.send(f"MSG:flood line {i:02d}")
        relayed = [x for x in b.lines(1.5) if x.startswith("[Talker (T17)]")]
        dt = time.time() - t0
        thr = srv.log_matching(r"throttled .*from Talker")
        check(len(relayed) == N,
              f"all {N} flood lines relayed to the observer in {dt:.2f}s (got {len(relayed)})")
        check(not thr, f"no chat throttle log fired (got {thr})")
        say("  -> MSG has no BurstLimiter/token bucket: chat is relayed unbounded.")
        say("     Plan risk #16: any SIGNP/announcement pacing must be server-imposed;")
        say("     the client will not self-limit and the server does not limit MSG today.")
        a.close(); b.close()
    finally:
        srv.stop()


# --- C: --region-radius A/B ----------------------------------------------
def _region_box_span(srv, radius, world_dir):
    """Join, REGION at a fixed point, return (x1-x0) from the server's REGION log."""
    c = Client()
    c.send("JOIN:Surveyor:0:")
    c.lines(0.8)
    c.send("REGION:65536:65536")
    c.lines(1.2)
    time.sleep(0.2)
    c.close()
    logs = srv.log_matching(r"REGION #\d+ Surveyor .*box x\[(\d+)\.\.(\d+)\]")
    if not logs:
        return None, None
    m = re.search(r"box x\[(\d+)\.\.(\d+)\] z\[(\d+)\.\.(\d+)\]", logs[-1])
    x0, x1 = int(m.group(1)), int(m.group(2))
    return x1 - x0 + 1, logs[-1]


def probe_region_radius(d):
    say("\n[C] --region-radius A/B — does the REGION box span track the flag?")
    spans = {}
    for r in (112, 224, 448):
        wd = _world()
        extra = [] if r == 224 else ["--region-radius", str(r)]
        srv = Server(wd, *extra)
        try:
            start_line = srv.log_matching(r"REGION radius \d+")
            span, logline = _region_box_span(srv, r, wd)
            spans[r] = span
            say(f"  R={r:<4} startup:{start_line if start_line else '(default, not logged)'}")
            say(f"           reply : {logline}")
            say(f"           x-span: {span} blocks")
        finally:
            srv.stop()
        shutil.rmtree(wd, ignore_errors=True)
    if all(spans.values()):
        check(spans[112] < spans[224] < spans[448],
              f"box span is monotonic in --region-radius ({spans[112]} < {spans[224]} < {spans[448]})")
        # region_box snaps to 16 and adds REGION_CHUNK-1, so span ~= 2*(R rounded) + 16
        for r, s in spans.items():
            approx = 2 * r
            check(abs(s - approx) <= 32,
                  f"R={r}: x-span {s} ~= 2R ({approx}) within snap tolerance")
    say("  -> server half only. Whether the GAME CLIENT renders a wider disc at R=448")
    say("     vs R=112 is the manual half (phase1_step4_manual.md) — the only way to")
    say("     actually *measure* the client's R.")


# --- D: name rejection ---------------------------------------------------
def probe_name_rejection(d):
    say("\n[D] name rejection — exact server strings")
    srv = Server(d)
    try:
        for name, why, expect in [
            ("server", "reserved name", "[Server] Invalid name"),
            ("bob] hi [server (T0)", "']' forging chat structure", "[Server] Invalid name"),
            ("", "empty name", "[Server] Invalid name"),
            ("x" * 40, "over-long name", "[Server] Invalid name"),
        ]:
            n = Client()
            n.send(f"JOIN:{name}:0:")
            got = n.lines(0.8)
            hit = any(x.startswith(expect) for x in got)
            check(hit and n.closed_by_peer(),
                  f"{why!r}: refused with {expect!r} + disconnect (got {got})")
            n.close()
        # duplicate connected name
        live = Client(); live.send("JOIN:Dupe:0:"); live.lines(0.6)
        dup = Client(); dup.send("JOIN:Dupe:0:")
        got = dup.lines(0.8)
        check(got == ["[Server] Name already in use."] and dup.closed_by_peer(),
              f"duplicate live name: exact 'Name already in use.' + disconnect (got {got})")
        dup.close(); live.close()
    finally:
        srv.stop()
    say("  -> these are the literal strings the server sends. Whether the GAME CLIENT")
    say("     surfaces them or fails silently is the manual half.")


def main():
    os.makedirs(LOGDIR, exist_ok=True)
    stamp = _dt.date.today().isoformat()
    say(f"PHASE1 Step 4 socket probes — {_dt.datetime.now().isoformat(timespec='seconds')}")
    say(f"server: {SERVER}")
    d = _world()
    try:
        probe_spawn(d)
        probe_chat_flood(d)
        probe_region_radius(d)
        probe_name_rejection(d)
    finally:
        shutil.rmtree(d, ignore_errors=True)

    say()
    if _fails:
        say(f"{len(_fails)} check(s) FAILED:")
        for f in _fails:
            say("  - " + f)
    else:
        say("all socket checks passed")

    outpath = os.path.join(LOGDIR, f"phase1-step4-{stamp}.log")
    with open(outpath, "w") as fh:
        fh.write("\n".join(_report) + "\n")
    say(f"\nwrote {outpath}")
    return 1 if _fails else 0


if __name__ == "__main__":
    sys.exit(main())
