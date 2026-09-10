#!/usr/bin/env python3
"""Adversarial live socket test for ROADMAP-SERVER stages 3.2-3.4 against ./edenserver.

    ./build_server.sh && python3 phase3_live_test.py [path/to/edenserver]

The offline suites (control_test, worldedit_test) prove the bounds are *correct*.
This one tries to *break* them from a socket, which is the honest exit criterion
for "each §0.5.7 defect has a test": it starts real servers in a temp world dir,
connects real TCP clients and a real unix control socket, and attacks them.

Deliberately NOT wired into build_server.sh — it binds a port and spawns
processes. Run it by hand before hosting anything publicly.

Covers, defect by defect:
  1  oversized selections refused at read time, on every command that reads one
  2  level-0 players refused every editing verb; no case/prefix trick gets past
  3  the edited-cell ceiling is a refusal path (unreachable in a fast test — see
     the note printed by group 12)
  5  undo bounded by bytes: a long session evicts oldest-first, it does not grow
  6  commands sent before JOIN are ignored in silence, not default-constructed
  7  the clipboard cannot outrun the cap, so //paste cannot either
  8  /tp <player> is level 2; /tp <x> <y> <z> is level 1
  9  duplicate usernames refused at JOIN

...and the four stage-3.4 items:
  A  control-socket flood guard: command pacing, strikes, concurrent-conn cap
  B  the Tier 1 fill cap derives from --we-max-cells (one number, not two)
  C  audit completeness: every mutation logged without --verbose, to --audit-file
  D  command flooding and malformed-argument fuzzing leave the server serving
"""
import os, random, shutil, socket, string, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "edenserver")
PORT = 27098

fails = []
def check(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        fails.append(msg)


# --- transports ---------------------------------------------------------------

class LineSock:
    """Shared line framing for both surfaces: the game wire and the control socket."""
    def __init__(self, sock, timeout):
        self.s = sock
        self.s.settimeout(timeout)
        self.buf = b""
        self.closed = False

    def send(self, line):
        self.s.sendall((line + "\n").encode())

    def send_raw(self, data):
        self.s.sendall(data)

    def lines(self, wait=0.5, want=None):
        """Collect complete lines for `wait` seconds (or until `want` of them)."""
        out, deadline = [], time.time() + wait
        while time.time() < deadline:
            if want is not None and len(out) >= want:
                break
            self.s.settimeout(max(0.02, deadline - time.time()))
            try:
                b = self.s.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                self.closed = True
                break
            if not b:
                self.closed = True
                break
            self.buf += b
            while b"\n" in self.buf:
                ln, self.buf = self.buf.split(b"\n", 1)
                out.append(ln.decode("utf-8", "replace"))
        return out

    def closed_by_peer(self, wait=1.5):
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


class Client(LineSock):
    """A game client. `join` runs the handshake and drains the join burst."""
    def __init__(self, timeout=4.0):
        super().__init__(socket.create_connection(("127.0.0.1", PORT), timeout=timeout), timeout)

    def join(self, name, char=17, password=""):
        self.send(f"JOIN:{name}:{char}:{password}")
        return self.lines(0.6)

    def at(self, x, y, z):
        """Put the player somewhere and let the server record it. //pos1 and the
        shape commands are all relative to this."""
        self.send(f"POS:{x}:{y}:{z}")
        time.sleep(0.08)

    def cmd(self, text, wait=0.5, want=None):
        """One Tier 2 command, as the client sends it: an ordinary MSG."""
        self.send("MSG:" + text)
        return self.lines(wait, want)


class Ctl(LineSock):
    def __init__(self, path, timeout=4.0):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect(path)
        super().__init__(s, timeout)


def ctl_cmd(path, line, wait=0.6):
    """One control command on its own connection, the way edenctl does it."""
    c = Ctl(path)
    try:
        c.send(line)
        return c.lines(wait)
    finally:
        c.close()


# --- server harness -----------------------------------------------------------

class Server:
    """A running ./edenserver with its stdout drained into a list.

    Draining matters here and not in phase1_live_test.py: this test asserts on
    the audit channel, and an undrained pipe would deadlock the server as soon
    as it filled."""
    def __init__(self, world_dir, *extra):
        self.dir = world_dir
        self.sock = os.path.join(world_dir, "edenserver.sock")
        self.out = []
        self._lock = threading.Lock()
        self.p = subprocess.Popen(
            [SERVER, "--port", str(PORT), "--connect-limit", "0", *extra],
            cwd=world_dir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self._t = threading.Thread(target=self._drain, daemon=True)
        self._t.start()
        for _ in range(200):
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.05)
        else:
            raise RuntimeError("server did not come up")
        for _ in range(100):          # the control socket is a separate thread
            if os.path.exists(self.sock):
                break
            time.sleep(0.05)

    def _drain(self):
        for line in self.p.stdout:
            with self._lock:
                self.out.append(line.rstrip("\n"))

    def mark(self):
        with self._lock:
            return len(self.out)

    def since(self, mark, settle=0.35):
        time.sleep(settle)            # give the server's line a chance to arrive
        with self._lock:
            return list(self.out[mark:])

    def alive(self):
        return self.p.poll() is None

    def stop(self):
        self.p.terminate()
        try:
            self.p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.p.kill()


def said(lines, needle):
    return any(needle in ln for ln in lines)


# --- group 1: defect 1 — oversized selections ---------------------------------

def group_selection_cap(srv):
    print("\n[1] defect 1 — an oversized selection is refused by every reader")
    c = Client(); c.join("Alice")
    ctl_cmd(srv.sock, "op:Alice:1")

    c.at(65536, 40, 65536)
    c.cmd("//pos1", 0.4)
    c.at(66600, 200, 66600)                      # 1065 x 161 x 1065 = 182M cells
    pos2 = c.cmd("//pos2", 0.4)
    check(said(pos2, "over the"), "//pos2 warns the selection is over the limit before you use it")

    # Every command that reads a selection must refuse it, not just //set: the
    # point of Selection::box being the only accessor is that a command cannot
    # opt out of the check.
    for verb in ["//set 2", "//walls 2", "//paint 1", "//replace 1 2", "//copy"]:
        r = c.cmd(verb, 0.5)
        check(said(r, "cells; the limit is"), f"{verb.split()[0]} refuses the oversized selection")

    # A radius shape never touches Selection at all, so it is capped by the box
    # it implies rather than by the selection cap.
    r = c.cmd("//sphere 300 2", 0.5)
    check(said(r, "cells; the limit is"), "//sphere is capped by its bounding box")
    r = c.cmd("//sphere 9999 2", 0.4)
    check(said(r, "Radius must be"), "an absurd radius is refused by name, not by overflow")

    # defect 7: the clipboard could only have been filled through the refused
    # //copy above, so //paste has nothing to outrun the cap with.
    r = c.cmd("//paste", 0.4)
    check(said(r, "clipboard is empty"), "defect 7 — a refused //copy leaves the clipboard empty")
    check(srv.alive(), "server survived the oversized-selection pass")
    c.close()


# --- group 2: defect 2 — permission escalation --------------------------------

EDIT_VERBS = ["//set 2", "//walls 2", "//replace 1 2", "//replacenear 4 1 2", "//paint 1",
              "//unpaint", "//strip", "//undo", "//redo", "//copy", "//paste",
              "//rotate 90", "//up 3", "//sphere 2 2", "//hsphere 2 2",
              "//cyl 2 2 2", "//hcyl 2 2 2", "//pos1", "//pos2"]

def group_permissions(srv):
    print("\n[2] defect 2 — a level-0 player is refused every editing verb")
    # Split across two connections: the per-connection command bucket would
    # silently drop the tail of a 19-command burst, and a silent drop is not the
    # refusal we are trying to observe.
    half = len(EDIT_VERBS) // 2
    for name, verbs in (("Bob", EDIT_VERBS[:half]), ("Carol", EDIT_VERBS[half:])):
        c = Client(); c.join(name); c.at(65536, 40, 65536)
        bad = []
        for v in verbs:
            r = c.cmd(v, 0.35)
            if not said(r, "do not have permission"):
                bad.append(v)
        check(not bad, f"level 0 is refused every verb ({name}): {bad or 'none got through'}")
        c.close()

    print("\n[2b] no spelling, casing or prefix trick reaches a handler")
    c = Client(); c.join("Dave"); c.at(65536, 40, 65536)
    for probe, why in [("//SET 2", "upper case is not the same verb"),
                       ("///set 2", "a third slash is not the verb"),
                       ("/ /set 2", "a space does not join the slashes"),
                       ("//set\t2", "a tab does not rename the verb"),
                       ("//se t 2", "a typo is not a prefix match")]:
        r = c.cmd(probe, 0.35)
        got = said(r, "Unknown command") or said(r, "do not have permission") or said(r, "Usage:")
        check(got and not said(r, "block(s) changed"), why)
    c.close()

    print("\n[2c] op / deop take effect on the next line, with no reconnect")
    c = Client(); c.join("Erin"); c.at(65536, 40, 65536)
    check(said(c.cmd("//pos1", 0.35), "do not have permission"), "level 0 cannot select")
    ctl_cmd(srv.sock, "op:Erin:1")
    check(said(c.cmd("//pos1", 0.35), "pos1 ="), "op:1 takes effect on the very next command")
    ctl_cmd(srv.sock, "deop:Erin")
    check(said(c.cmd("//pos2", 0.35), "do not have permission"), "deop takes effect just as fast")
    c.close()


# --- group 3: defect 8 — /tp <player> is level 2 ------------------------------

def group_tp_disclosure(srv):
    print("\n[3] defect 8 — /tp <player> discloses a position, so it is level 2")
    victim = Client(); victim.join("Victim"); victim.at(65000, 44, 65000)
    spy = Client(); spy.join("Spy"); spy.at(65536, 40, 65536)
    ctl_cmd(srv.sock, "op:Spy:1")

    r = spy.cmd("/tp Victim", 0.4)
    check(said(r, "do not have permission") and not said(r, "SPAWN"),
          "level 1 cannot /tp to a named player")
    r = spy.cmd("/tp 65600 40 65600", 0.4)
    check(said(r, "SPAWN"), "level 1 can still /tp to coordinates (self-scoped)")

    ctl_cmd(srv.sock, "op:Spy:2")
    r = spy.cmd("/tp Victim", 0.4)
    check(said(r, "SPAWN:65000"), "level 2 may /tp to a player, and lands on their position")
    victim.close(); spy.close()


# --- group 4: defects 6 + 9 — pre-JOIN state and duplicate names --------------

def group_prejoin_and_names(srv):
    print("\n[4] defects 6 + 9 — no state is conjured before JOIN, no name is shared")
    c = Client()
    for probe in ["MSG://set 2", "MSG://pos1", "MSG:/tp 1 2 3", "MSG:/help"]:
        c.send(probe)
    r = c.lines(0.5)
    check(r == [], "commands before JOIN are ignored in silence (no default-constructed player)")
    check(not c.closed, "...and do not kill the connection")
    r = c.join("Frank")
    check(said(r, "Frank"), "the same connection can still JOIN afterwards")

    dup = Client()
    r = dup.join("Frank")
    check(said(r, "name") or dup.closed_by_peer(1.0), "a duplicate username is refused at JOIN")
    dup.close(); c.close()


# --- group 5: defect 5 — undo is bounded by bytes over a long session ---------

def group_undo_growth(srv):
    print("\n[5] defect 5 — a long session evicts undo history, it does not accumulate")
    c = Client(); c.join("Grace")
    ctl_cmd(srv.sock, "op:Grace:1")

    # 40 full-cap batches. With --we-undo-budget at its floor (one batch) the
    # store must hold ~1 and evict the rest, oldest-first. If it kept all 40 the
    # patch's defect is back.
    batches = 40
    for i in range(batches):
        c.at(65536 + i * 20, 40, 65536)
        c.cmd("//pos1", 0.15)
        c.at(65536 + i * 20 + 15, 55, 65536 + 15)
        c.cmd("//pos2", 0.15)
        r = c.cmd(f"//set {2 + (i % 3)}", 0.35)
        if i == 0:
            check(said(r, "block(s) changed"), "the first capped edit applies")

    undone = 0
    for _ in range(batches + 2):
        r = c.cmd("//undo", 0.3)
        if said(r, "Nothing to undo"):
            break
        undone += 1
    check(1 <= undone <= 4,
          f"undo held {undone} of {batches} batches — bounded by bytes, oldest evicted first")
    check(srv.alive(), "server survived 40 capped edits plus the unwind")
    c.close()


# --- group 6 + 7: flooding and malformed arguments ---------------------------

FUZZ_ARGS = ["", "0", "-1", "999999999999999999999", "-999999999999999999999", "~", "~~",
             "~-99999", "0x10", "1e309", "nan", "inf", "1.5", "+", "::", ":", "%s%s%n",
             "../../etc/passwd", "\x01\x02", "A" * 300, "1 2 3 4 5 6 7 8 9", "١٢٣", "２"]

def group_flood_and_fuzz(srv):
    print("\n[6] command flooding leaves the server serving")
    c = Client(); c.join("Mallory"); c.at(65536, 40, 65536)
    ctl_cmd(srv.sock, "op:Mallory:1")
    blast = "".join(f"MSG://sphere 6 {i % 100}\n" for i in range(500))
    c.send_raw(blast.encode())
    c.lines(1.5)
    check(srv.alive(), "500 shape commands in one write did not kill the server")
    # The per-connection command bucket drops the tail silently; what matters is
    # that a *different* player is still served immediately afterwards.
    other = Client()
    check(said(other.join("Bystander"), "Bystander"), "another client can still JOIN during the flood")
    other.close()

    print("\n[7] malformed arguments are refused, never dispatched")
    verbs = [v.split()[0] for v in EDIT_VERBS] + ["/tp", "/id", "/help", "/msg", "/r",
                                                  "/searchblocks", "/searchcolors", "/resync"]
    random.seed(3407)
    sent = 0
    for verb in verbs:
        f = Client(); f.join(f"Fuzz{sent}"); f.at(65536, 40, 65536)
        ctl_cmd(srv.sock, f"op:Fuzz{sent}:1")
        for _ in range(8):
            args = " ".join(random.choice(FUZZ_ARGS) for _ in range(random.randint(0, 4)))
            f.send("MSG:" + verb + (" " + args if args else ""))
        f.lines(0.4)
        f.close()
        sent += 1
    check(srv.alive(), f"{sent * 8} malformed command lines did not kill the server")

    tail = Client()
    r = tail.join("Survivor")
    check(said(r, "Survivor"), "the server still completes a handshake after the fuzz pass")
    tail.at(65536, 40, 65536)
    ctl_cmd(srv.sock, "op:Survivor:1")
    check(said(tail.cmd("//pos1", 0.4), "pos1 ="), "...and still serves a normal command")

    # stage 3.6 — the community name tables back /id and the search commands.
    check(said(tail.cmd("/id stone", 0.4), "2"), "/id resolves a block name to its number")
    check(said(tail.cmd("/id 19", 0.4), "colour"), "/id resolves a number to its colour name")
    check(said(tail.cmd("/searchblocks slope_nw", 0.4), "stone_slope_nw=40"),
          "/searchblocks returns name=id rows")
    check(said(tail.cmd("/id mauveish", 0.4), "not a known"),
          "/id refuses an unknown name rather than guessing")
    tail.close(); c.close()


# --- group 8: stage 3.4 item 1 — the control-socket flood guard --------------

def group_control_flood(srv):
    print("\n[8] 3.4 — the control socket is paced, and its connections are bounded")
    # Concurrent-connection cap first: the flood test below ends in a
    # disconnect, and we want a clean count.
    held = []
    refused = None
    for i in range(12):
        try:
            c = Ctl(srv.sock)
        except OSError:
            refused = "connect() refused"
            break
        r = c.lines(0.25)
        if said(r, "too many control connections"):
            refused = r
            c.close()
            break
        held.append(c)
    check(refused is not None, f"the {len(held) + 1}th concurrent control connection is refused")
    check(len(held) <= 8, f"no more than 8 control connections are held open (held {len(held)})")
    for c in held:
        c.close()
    time.sleep(0.3)

    # Pacing: the default budget is 64 at once refilling at 16/s, so 300 lines in
    # one write must be throttled and then hung up on.
    c = Ctl(srv.sock)
    c.send_raw(b"who\n" * 300)
    r = c.lines(2.0)
    check(said(r, "rate limited, slow down"), "a burst past the budget is throttled")
    check(said(r, "rate limited (disconnecting)"), "a caller that ignores the throttle is hung up on")
    check(c.closed or c.closed_by_peer(1.0), "...and the socket really is closed")
    c.close()

    check(srv.alive(), "the server survived the control-socket flood")
    r = ctl_cmd(srv.sock, "who")
    check(said(r, "player(s)"), "a well-behaved control client is served immediately afterwards")


# --- group 9: stage 3.4 item 3 — one cap, derived ----------------------------

def group_fill_cap(server_dir):
    print("\n[9] 3.4 — the Tier 1 fill cap is derived from --we-max-cells, not typed twice")
    srv = Server(server_dir, "--we-max-cells", "1000")
    try:
        # 10 x 10 x 21 = 2100 > 2 x 1000
        r = ctl_cmd(srv.sock, "fill:65536:40:65536:65545:60:65545:2")
        check(said(r, "error: box is"), "a fill past the derived cap is refused")
        check(said(r, "max 2000"), "...and the cap moved with --we-max-cells (2 x 1000)")
        check(said(r, "--we-max-cells"), "...and the error names the flag that moves it")

        # 10 x 10 x 20 = 2000, exactly the cap
        r = ctl_cmd(srv.sock, "fill:65536:40:65536:65545:59:65545:2", 1.5)
        check(said(r, "ok: filled 2000 cells"), "a fill exactly at the cap is applied")
        check(srv.alive(), "server survived the fill boundary")
    finally:
        srv.stop()


# --- group 10: stage 3.4 item 5 — audit completeness -------------------------

def group_audit(server_dir):
    print("\n[10] 3.4 — every mutation is audited without --verbose")
    audit = os.path.join(server_dir, "audit.log")
    srv = Server(server_dir, "--audit-file", audit)   # note: no --verbose anywhere
    try:
        c = Client(); c.join("Auditee"); c.at(65536, 40, 65536)
        ctl_cmd(srv.sock, "op:Auditee:1")

        m = srv.mark()
        c.cmd("//pos1", 0.3)
        c.at(65540, 44, 65540)
        c.cmd("//pos2", 0.3)
        c.cmd("//set 3", 0.5)
        lines = srv.since(m)
        check(any("[Audit]" in l and "player:Auditee" in l and "//set" in l for l in lines),
              "a level-1 edit is audited with no --verbose")
        check(any("cell(s)" in l for l in lines if "[Audit]" in l),
              "...with the count of cells it actually changed")
        check(not any("[Audit]" in l and "//pos1" in l for l in lines),
              "a selection is not an edit and is not audited")

        m = srv.mark()
        c.cmd("//undo", 0.5)
        check(any("[Audit]" in l and "//undo" in l for l in srv.since(m)),
              "//undo mutates the world, so //undo is audited too")

        m = srv.mark()
        ctl_cmd(srv.sock, "say:hello from the operator")
        ctl_cmd(srv.sock, "setblock:65536:40:65536:5")
        lines = srv.since(m)
        check(any("[Audit]" in l and "control say" in l for l in lines), "control say is audited")
        check(any("[Audit]" in l and "control setblock" in l for l in lines),
              "control setblock is audited")

        m = srv.mark()
        r = ctl_cmd(srv.sock, "who")
        check(not any("[Audit]" in l for l in srv.since(m)),
              "a read-only control command is not audited (the channel stays signal)")

        # /tp <player> changes no cells, so it is audited on the way in instead.
        peer = Client(); peer.join("Peer"); peer.at(65100, 40, 65100)
        ctl_cmd(srv.sock, "op:Auditee:2")
        m = srv.mark()
        c.cmd("/tp Peer", 0.5)
        check(any("[Audit]" in l and "/tp Peer" in l for l in srv.since(m)),
              "a cross-player command is audited even though it edits nothing")
        peer.close(); c.close()

        # The stdout channel and the file must agree.
        time.sleep(0.4)
        with open(audit) as f:
            filed = [l.rstrip("\n") for l in f]
        stdout_audit = [l for l in srv.since(0, 0.1) if l.startswith("[Audit]")]
        check(len(filed) > 0, "--audit-file was written")
        check(filed == stdout_audit,
              f"--audit-file matches the stdout channel line for line ({len(filed)} vs {len(stdout_audit)})")
    finally:
        srv.stop()


# --- group 13: stage 5.3 — the world default spawn --------------------------

def group_world_spawn(server_dir):
    print("\n[13] 5.3 — eden_spawn.txt is the default spawn; a saved row still wins")
    spawn_file = os.path.join(server_dir, "eden_spawn.txt")
    players_file = os.path.join(server_dir, "eden_players.txt")

    with open(spawn_file, "w") as f:
        f.write("65540.00:33.92:65500.00\n")
    with open(players_file, "w") as f:
        f.write("Returning:1000.5:40.0:2000.5\n")

    srv = Server(server_dir)
    try:
        c = Client(); r = c.join("Fresh")
        check(any("SPAWN:65540.00:33.92:65500.00" in ln for ln in r),
              "a fresh name is sent the world spawn from eden_spawn.txt")
        c.close()

        c = Client(); r = c.join("Returning")
        check(any("SPAWN:1000.50:40.00:2000.50" in ln for ln in r),
              "a name with an eden_players.txt row gets the row, not the world spawn")
        check(not any("SPAWN:65540" in ln for ln in r), "...and not both")
        c.close()
    finally:
        srv.stop()

    # A malformed spawn file warns, is ignored, and is never fatal.
    os.remove(players_file)
    with open(spawn_file, "w") as f:
        f.write("not:coordinates:here:extra\n")
    srv = Server(server_dir)
    try:
        check(said(srv.since(0, 0.3), "ignoring malformed spawn line"),
              "a malformed eden_spawn.txt warns")
        c = Client(); r = c.join("Fresh2")
        check(not any(ln.startswith("SPAWN:") for ln in r),
              "no SPAWN is sent when the spawn file is unusable")
        check(srv.alive(), "a malformed eden_spawn.txt is not fatal")
        c.close()
    finally:
        srv.stop()


# --- group 11 + 12: notes on what a socket test cannot reach -----------------

def group_notes():
    print("\n[11] defect 3 — the edited-cell ceiling")
    print("     the edited-cell cap defaults to 4,000,000 (--max-world-cells); reaching it")
    print("     over a socket means relaying ~250 MB of ACTION. The projection is checked before every")
    print("     scan in weCommit/weEditBox and refuses rather than truncates; the")
    print("     arithmetic is covered offline. Not exercised here on purpose.")
    print("\n[12] the control socket's 300 s idle timeout")
    print("     Correct to have, 300 s too slow to assert in a pass that already takes")
    print("     ~2 minutes. Verify by hand with: nc -U <world>/edenserver.sock")


# --- main ---------------------------------------------------------------------

def main():
    if not os.path.exists(SERVER):
        print(f"no server at {SERVER} — run ./build_server.sh first")
        return 2

    d = tempfile.mkdtemp(prefix="edenphase3_")
    try:
        # Run A: shipping defaults. --default-level 0 is the default and is the
        # posture a public server must survive, so most of the attack surface is
        # tested exactly as it ships.
        srv = Server(d, "--default-level", "0")
        try:
            group_selection_cap(srv)
            group_permissions(srv)
            group_tp_disclosure(srv)
            group_prejoin_and_names(srv)
            group_flood_and_fuzz(srv)
            group_control_flood(srv)
        finally:
            srv.stop()

        # Run B: the undo store at its floor, with the cell budget off — which
        # also checks that --we-rate 0 removes the budget and not the cap.
        b = tempfile.mkdtemp(prefix="edenphase3b_")
        srv = Server(b, "--we-rate", "0", "--we-max-cells", "4096", "--we-undo-budget", "1")
        try:
            group_undo_growth(srv)
        finally:
            srv.stop()
            shutil.rmtree(b, ignore_errors=True)

        for prefix, group in (("edenphase3c_", group_fill_cap), ("edenphase3d_", group_audit),
                              ("edenphase3e_", group_world_spawn)):
            wd = tempfile.mkdtemp(prefix=prefix)
            try:
                group(wd)
            finally:
                shutil.rmtree(wd, ignore_errors=True)
        group_notes()
    finally:
        shutil.rmtree(d, ignore_errors=True)

    print()
    if fails:
        print(f"{len(fails)} check(s) FAILED")
        for f_ in fails:
            print("  - " + f_)
        return 1
    print("phase3_live_test: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
