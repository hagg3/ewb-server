#!/usr/bin/env python3
"""Live socket test for ROADMAP-SERVER stage 10.2 (fireworks, the golden cube,
expansion blocks) and 10.4 (the --tnt / --fire switches) against ./edenserver.

    ./build_server.sh && python3 phase10_live_test.py [path/to/edenserver]

burn_test proves the rules offline: the expansion fill, the dry-run preview, the
verdict, the timing. This one proves the server applies them on the ACTION path,
over real sockets, with a second client watching what gets relayed. Deliberately
NOT wired into build_server.sh — it binds a port and spawns processes. What a real
retail client does with the lines is a separate question (stage 10.0); this checks
what the server sends and keeps.

Covers:
  1  Switches on: a burnt firework takes only its own cell; a golden cube survives a
     blast; a burnt expansion block fills its box and ends as its material, and the
     MINE its client follows the burn with is not applied or relayed.
  2  --tnt off: a TNT build (9, and the TNT expansion block 87) is refused — not
     relayed, not stored, taken back out on the sender's screen at once, with a
     private notice. Other builds relay. A burn on existing TNT, or on wood touching
     TNT, is refused: a mine on the lit cell at once, the blast footprint restored
     ~0.25 s later and again once the fuse would be long over; peers see nothing and
     the TNT is still in the model. A burn on wood away from TNT is allowed.
     The `tnt` control verb shows and flips the switch at runtime.
  3  --fire off: every burn is refused; a flammable block is mined at once and put
     back; a non-flammable one only gets the notice. `fire:on` lets burns through.
  4  A bad --tnt / --fire value stops the server from starting.

The whole pass is ~30 s.
"""
import os, shutil, socket, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "edenserver")
PORT = 27097

fails = []
def check(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        fails.append(msg)


# --- transports (the phase8_live_test.py shapes) --------------------------------

class LineSock:
    def __init__(self, sock, timeout):
        self.s = sock
        self.s.settimeout(timeout)
        self.buf = b""

    def send(self, line):
        self.s.sendall((line + "\n").encode())

    def lines(self, wait=0.5):
        """Collect complete lines for `wait` seconds."""
        out, deadline = [], time.time() + wait
        while time.time() < deadline:
            self.s.settimeout(max(0.02, deadline - time.time()))
            try:
                b = self.s.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not b:
                break
            self.buf += b
            while b"\n" in self.buf:
                ln, self.buf = self.buf.split(b"\n", 1)
                out.append(ln.decode("utf-8", "replace"))
        return out

    def timed(self, wait):
        """Like lines(), but each line with the seconds since the call."""
        out, t0 = [], time.time()
        deadline = t0 + wait
        while time.time() < deadline:
            self.s.settimeout(max(0.02, deadline - time.time()))
            try:
                b = self.s.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not b:
                break
            self.buf += b
            while b"\n" in self.buf:
                ln, self.buf = self.buf.split(b"\n", 1)
                out.append((time.time() - t0, ln.decode("utf-8", "replace")))
        return out

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


class Client(LineSock):
    def __init__(self, timeout=4.0):
        super().__init__(socket.create_connection(("127.0.0.1", PORT), timeout=timeout), timeout)

    def join(self, name):
        self.send(f"JOIN:{name}:17:")
        return self.lines(0.5)


def ctl_cmd(path, line, wait=0.4):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(path)
    c = LineSock(s, 4.0)
    try:
        c.send(line)
        return c.lines(wait)
    finally:
        c.close()


class Server:
    def __init__(self, world_dir, *extra):
        self.dir = world_dir
        self.sock = os.path.join(world_dir, "edenserver.sock")
        self.out = []
        self._lock = threading.Lock()
        self.p = subprocess.Popen(
            [SERVER, "--port", str(PORT), "--connect-limit", "0", "--world-format", "text",
             "--action-rate", "0", *extra],
            cwd=world_dir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        threading.Thread(target=self._drain, daemon=True).start()
        for _ in range(200):
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.05)
        else:
            raise RuntimeError("server did not come up:\n" + "\n".join(self.out))
        for _ in range(100):
            if os.path.exists(self.sock):
                break
            time.sleep(0.05)

    def _drain(self):
        for line in self.p.stdout:
            with self._lock:
                self.out.append(line.rstrip("\n"))

    def ctl(self, line):
        return ctl_cmd(self.sock, line)

    def model(self):
        """The saved world as {(x, y, z): (type, color)} — a control `save` first."""
        self.ctl("save")
        cells = {}
        path = os.path.join(self.dir, "eden_world.model")
        if not os.path.exists(path):   # nothing stored yet: nothing saved
            return cells
        with open(path) as f:
            for ln in f:
                p = ln.strip().split(":")
                if len(p) == 5:
                    x, y, z, t, c = map(int, p)
                    cells[(x, y, z)] = (t, c)
        return cells

    def stop(self):
        self.p.terminate()
        try:
            self.p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.p.kill()


def said(lines, needle):
    return any(needle in ln for ln in lines)

def server_actions(lines):
    return [ln for ln in lines if ln.startswith("ACTION:server:0:")]

def relayed(lines):
    return [ln for ln in lines if ln.startswith("ACTION:") and not ln.startswith("ACTION:server:")]

def scratch():
    return tempfile.mkdtemp(prefix="ewb10_")


# --- 1. switches on: fireworks, golden cube, expansion blocks ---------------------

def test_rules():
    print("1  switches on: firework / golden cube / expansion")
    d = scratch()
    srv = Server(d)
    try:
        a, b = Client(), Client()
        a.join("alice"); b.join("bob")
        X, Y, Z = 70000, 100, 70000

        srv.ctl(f"setblock:{X}:{Y}:{Z}:65")
        srv.ctl(f"setblock:{X+1}:{Y}:{Z}:2")
        b.lines(0.3)
        a.send(f"ACTION:{X}:{Y}:{Z}:2")
        check(len(relayed(b.lines(0.5))) == 1, "the firework burn is relayed")
        m = srv.model()
        check(m.get((X, Y, Z), (0, 0))[0] == 0 and m.get((X+1, Y, Z)) == (2, 0),
              "a burnt firework takes only its own cell")
        check(sum(1 for (x, y, z) in m if abs(x - X) <= 6 and abs(z - Z) <= 6) == 2,
              "and nothing else around it is stored")

        G = X + 100
        srv.ctl(f"setblock:{G}:{Y}:{Z}:9")
        srv.ctl(f"setblock:{G+2}:{Y}:{Z}:71")
        srv.ctl(f"setblock:{G-2}:{Y}:{Z}:7")
        a.send(f"ACTION:{G}:{Y}:{Z}:2")
        time.sleep(0.3)
        m = srv.model()
        check(m.get((G+2, Y, Z)) == (71, 0), "a golden cube survives a blast")
        check(m.get((G-2, Y, Z), (0, 0))[0] == 0, "wood beside it does not")

        E = X + 200
        srv.ctl(f"setblock:{E}:{Y}:{Z}:84:5")
        b.lines(0.3)
        a.send(f"ACTION:{E}:{Y}:{Z}:2")
        check(len(relayed(b.lines(0.5))) == 1, "the expansion burn is relayed")
        m = srv.model()
        box = [(x, y, z) for x in range(E-2, E+3) for y in range(Y-2, Y+3) for z in range(Z-2, Z+3)]
        check(all(m.get(c, (0, 0))[0] == 2 for c in box), "an expansion fills its 5x5x5 box with stone")
        check(m.get((E+1, Y, Z)) == (2, 5), "the fill takes the block's colour")
        check(m.get((E, Y, Z)) == (2, 0), "the centre ends as stone, unpainted")
        a.send(f"ACTION:{E}:{Y}:{Z}:1")
        check(not relayed(b.lines(0.5)), "the MINE the client follows it with is not relayed")
        check(srv.model().get((E, Y, Z)) == (2, 0), "...or applied")
        a.send(f"ACTION:{E}:{Y}:{Z}:1")
        check(len(relayed(b.lines(0.5))) == 1, "a second MINE there is a real edit")
        a.close(); b.close()
    finally:
        srv.stop()
        shutil.rmtree(d, ignore_errors=True)


# --- 2. --tnt off ------------------------------------------------------------------

def test_tnt_off():
    print("2  --tnt off")
    d = scratch()
    srv = Server(d, "--tnt", "off")
    try:
        check(any("--tnt off" in ln for ln in srv.out), "the startup log says so")
        check(srv.ctl("tnt") == ["tnt: off"], "`tnt` shows it")
        a, b = Client(), Client()
        a.join("alice"); b.join("bob")
        X, Y, Z = 70000, 40, 70000

        for t in (9, 87):
            a.send(f"ACTION:{X}:{Y}:{Z}:0:{t}")
            la = a.lines(0.4)
            check(server_actions(la) == [f"ACTION:server:0:{X}:{Y}:{Z}:1"],
                  f"BUILD type {t}: taken back out on the sender's screen at once")
            check(said(la, "TNT is disabled on this server.") == (t == 9),
                  f"BUILD type {t}: the notice (once per 10 s)")
            check(not relayed(b.lines(0.3)), f"BUILD type {t}: not relayed")
            check((X, Y, Z) not in srv.model(), f"BUILD type {t}: not stored")
        a.send(f"ACTION:{X}:{Y}:{Z}:0:2")
        check(len(relayed(b.lines(0.4))) == 1, "a stone build relays as usual")

        T = X + 50
        srv.ctl(f"setblock:{T}:{Y}:{Z}:9")
        a.lines(0.3); b.lines(0.3)
        a.send(f"ACTION:{T}:{Y}:{Z}:2")
        tl = a.timed(9.5)
        acts = [(t, ln) for t, ln in tl if ln.startswith("ACTION:server:0:")]
        first = acts[0] if acts else (99, "")
        check(first[1] == f"ACTION:server:0:{T}:{Y}:{Z}:1" and first[0] < 0.15,
              "a burn on TNT: the lit cell is mined at once")
        early = [ln for t, ln in acts if 0.15 <= t < 2.0]
        late = [ln for t, ln in acts if t >= 2.0]
        check(len(early) > 100 and f"ACTION:server:0:{T}:{Y}:{Z}:0:9" in early,
              "the blast footprint is restored ~0.25 s later, TNT included")
        check(len(late) > 100 and min(t for t, ln in acts if t >= 2.0) > 6.0,
              "and again once the fuse would be long over")
        check(not relayed(b.lines(0.2)) and not server_actions(b.lines(0.1)), "the peer sees nothing")
        check(srv.model().get((T, Y, Z)) == (9, 0), "the TNT is still in the model")

        W = X + 100
        srv.ctl(f"setblock:{W}:{Y}:{Z}:7")
        srv.ctl(f"setblock:{W+1}:{Y}:{Z}:7")
        srv.ctl(f"setblock:{W+2}:{Y}:{Z}:9")
        b.lines(0.3)
        a.send(f"ACTION:{W}:{Y}:{Z}:2")
        check(not relayed(b.lines(0.5)), "wood whose fire would reach TNT is refused too")
        check(srv.model().get((W, Y, Z)) == (7, 0), "and stays")

        P = X + 150
        srv.ctl(f"setblock:{P}:{Y}:{Z}:7")
        b.lines(0.3)
        a.send(f"ACTION:{P}:{Y}:{Z}:2")
        check(len(relayed(b.lines(0.5))) == 1, "wood away from TNT burns as usual")

        check(srv.ctl("tnt:on") == ["ok: tnt on"], "`tnt:on`")
        a.send(f"ACTION:{X}:{Y}:{Z+5}:0:9")
        check(len(relayed(b.lines(0.4))) == 1, "TNT builds relay again")
        check(srv.ctl("tnt:maybe")[0].startswith("usage:"), "a bad value is refused")
        a.close(); b.close()
    finally:
        srv.stop()
        shutil.rmtree(d, ignore_errors=True)


# --- 3. --fire off -------------------------------------------------------------------

def test_fire_off():
    print("3  --fire off")
    d = scratch()
    srv = Server(d, "--fire", "off")
    try:
        a, b = Client(), Client()
        a.join("alice"); b.join("bob")
        X, Y, Z = 70000, 40, 70000
        srv.ctl(f"setblock:{X}:{Y}:{Z}:7")
        srv.ctl(f"setblock:{X+5}:{Y}:{Z}:2")
        a.lines(0.3); b.lines(0.3)

        a.send(f"ACTION:{X}:{Y}:{Z}:2")
        tl = a.timed(1.0)
        acts = [(t, ln) for t, ln in tl if ln.startswith("ACTION:server:0:")]
        check(acts and acts[0][1] == f"ACTION:server:0:{X}:{Y}:{Z}:1" and acts[0][0] < 0.15,
              "a burn on wood: mined at once")
        check([ln for t, ln in acts[1:]] == [f"ACTION:server:0:{X}:{Y}:{Z}:1", f"ACTION:server:0:{X}:{Y}:{Z}:0:7"]
              and acts[1][0] >= 0.15, "and rebuilt ~0.25 s later")
        check(any("Burning is disabled on this server." in ln for t, ln in tl), "the notice")
        check(not relayed(b.lines(0.3)), "not relayed")
        check(srv.model().get((X, Y, Z)) == (7, 0), "not applied")

        a.send(f"ACTION:{X+5}:{Y}:{Z}:2")
        la = a.lines(0.6)
        check(not server_actions(la), "a burn on stone: nothing to put back")

        check(srv.ctl("fire:on") == ["ok: fire on"], "`fire:on`")
        a.send(f"ACTION:{X}:{Y}:{Z}:2")
        check(len(relayed(b.lines(0.5))) == 1, "burns relay again")
        a.close(); b.close()
    finally:
        srv.stop()
        shutil.rmtree(d, ignore_errors=True)


# --- 4. a bad switch value is fatal ---------------------------------------------------

def test_bad_flag():
    print("4  a bad --tnt / --fire value")
    d = scratch()
    try:
        for flag in ("--tnt", "--fire"):
            p = subprocess.run([SERVER, "--port", str(PORT), flag, "of"], cwd=d,
                               capture_output=True, text=True, timeout=10)
            check(p.returncode != 0 and "must be 'on' or 'off'" in p.stderr + p.stdout,
                  f"{flag} of: the server refuses to start")
    finally:
        shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    if not os.path.exists(SERVER):
        sys.exit(f"no server binary at {SERVER}; run ./build_server.sh first")
    test_rules()
    test_tnt_off()
    test_fire_off()
    test_bad_flag()
    print()
    if fails:
        print(f"{len(fails)} FAILED:")
        for f in fails:
            print("  - " + f)
        sys.exit(1)
    print("all phase 10 live checks passed")
