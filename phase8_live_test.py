#!/usr/bin/env python3
"""Live socket test for ROADMAP-SERVER stage 8.2 (protected zones: server enforcement
and the visible undo) against ./edenserver.

    ./build_server.sh && python3 phase8_live_test.py [path/to/edenserver]

protocol_test proves the pieces are right offline: the explosion hook, the restore
wire, the revert queue's coalescing, the audit folding. This one proves the server
actually puts them at every player edit path, over real sockets, with a second
client watching what gets relayed. Deliberately NOT wired into build_server.sh — it
binds a port and spawns processes.

Every server here runs on a scratch world with one zone, `spawn`, covering
x 65500..65520, z 65500..65520, the whole column. What a real retail client does
with the lines is a separate question (LIVE-FINDINGS, stage 8.0); this checks
what the server sends.

Covers:
  1  ACTION build / mine / paint inside the zone: not relayed to a peer, not in the
     model, and the sender gets the cell put back (model value, or the natural block
     for an untouched cell). An edit outside the zone still relays. Notice + audit.
  2  A sign write in the zone: not stored, not relayed; an existing sign in that slot
     is sent back to its writer. Mining a signed protected block resends the sign
     with the restore.
  3  A burn outside the zone whose blast reaches in: relayed, the unprotected part
     applies, the protected part does not, a protected TNT is not chained — and the
     restore goes to both clients, after the relay.
  4  WorldEdit across the zone edge: the outside cells change, the inside cells are
     skipped and reported; //undo is held to the same rule.
  5  The control socket bypasses zones: `fill` and `setblock` inside one apply.
  5b `zone:add|set|flags|rm|reload` (stage 8.3): each takes effect on the next
     edit with no restart, persists atomically, refuses a duplicate name / bad
     flag / unknown name, and `reload` picks up a hand-edited file or refuses
     a malformed one without touching the in-memory set.
  6  With a revert delay: 100 mines on one protected cell produce one restore, not
     before the delay; the audit channel gets one line for them.
  7  Denied edits still spend the sender's ACTION budget.
  8  A malformed eden_zones.txt stops the server from starting.
  9  Player identity (stage 8.6): `passwd` issues a PIN shown once and stored only
     hashed (0600); a PIN-protected name gets --default-level until `/login`; the PIN
     never reaches the log; wrong PINs are paced per connection and locked out per
     IP; a re-issued PIN logs the live session out; `who` shows the auth state; only
     a logged-in session bypasses a levelled zone; a malformed eden_auth.txt is fatal.
 10  `topmap` (stage 8.5): the surface rule on known cells, the sample cap, a full
     256x256 reply's shape.
 11  In-game `/zone` (stage 8.7): level 2 *and* logged in; create from the selection
     (whole column by default, `exact` keeps its heights), enforced at once, list,
     here, rm.

The whole pass is ~20 s.
"""
import os, shutil, socket, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "edenserver")
PORT = 27096

ZONE = "spawn:65500:0:65500:65520:255:65520:all"
ZX0, ZX1, ZZ0, ZZ1 = 65500, 65520, 65500, 65520

fails = []
def check(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        fails.append(msg)


# --- transports (the phase3_live_test.py shapes) --------------------------------

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

    def at(self, x, y, z):
        self.send(f"POS:{x}:{y}:{z}")
        time.sleep(0.08)

    def cmd(self, text, wait=0.5):
        self.send("MSG:" + text)
        return self.lines(wait)


def ctl_cmd(path, line, wait=0.5):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(path)
    c = LineSock(s, 4.0)
    try:
        c.send(line)
        return c.lines(wait)
    finally:
        c.close()


class Server:
    """A running ./edenserver in `world_dir`, stdout drained into a list (the audit
    channel is asserted on, and an undrained pipe would deadlock the server)."""
    def __init__(self, world_dir, *extra):
        self.dir = world_dir
        self.sock = os.path.join(world_dir, "edenserver.sock")
        self.out = []
        self._lock = threading.Lock()
        self.p = subprocess.Popen(
            [SERVER, "--port", str(PORT), "--connect-limit", "0", "--world-format", "text", *extra],
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

    def mark(self):
        with self._lock:
            return len(self.out)

    def since(self, mark, settle=0.3):
        time.sleep(settle)
        with self._lock:
            return list(self.out[mark:])

    def alive(self):
        return self.p.poll() is None

    def model(self):
        """The saved world as {(x, y, z): (type, color)} — a control `save` first."""
        ctl_cmd(self.sock, "save", 0.4)
        cells = {}
        with open(os.path.join(self.dir, "eden_world.model")) as f:
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

def restores(lines):
    return [ln for ln in lines if ln.startswith("ACTION:server:0:")]

def player_relays(lines):
    return [ln for ln in lines if ln.startswith("ACTION:") and not ln.startswith("ACTION:server:")]

def in_zone(x, z):
    return ZX0 <= x <= ZX1 and ZZ0 <= z <= ZZ1


def make_world(d, cells=(), signs=(), zones=(ZONE,)):
    with open(os.path.join(d, "eden_world.model"), "w") as f:
        for (x, y, z, t, c) in cells:
            f.write(f"{x}:{y}:{z}:{t}:{c}\n")
    with open(os.path.join(d, "eden_signs.txt"), "w") as f:
        for s in signs:
            f.write(s + "\n")
    with open(os.path.join(d, "eden_zones.txt"), "w") as f:
        f.write("# test zones\n")
        for z in zones:
            f.write(z + "\n")


# --- group 1: ACTION build / mine / paint --------------------------------------

def group_action(srv):
    print("\n[1] ACTION build / mine / paint inside a zone")
    a, b = Client(), Client()
    a.join("Alice"); b.join("Bob")
    a.lines(0.2); b.lines(0.2)

    m = srv.mark()
    # Mine untouched grass: the natural grass comes back.
    a.send("ACTION:65510:32:65510:1")
    ra = a.lines(0.5)
    check(restores(ra) == ["ACTION:server:0:65510:32:65510:1", "ACTION:server:0:65510:32:65510:0:8"],
          "a mined natural block is put back as grass (mine, build 8)")
    check(said(ra, "[Server] This area is protected ('spawn')."), "the player is told why")
    # Mine a stored painted block: it comes back painted.
    a.send("ACTION:65511:40:65511:1")
    ra = a.lines(0.5)
    check(restores(ra) == ["ACTION:server:0:65511:40:65511:1", "ACTION:server:0:65511:40:65511:0:5",
                           "ACTION:server:0:65511:40:65511:3:12"],
          "a mined stored block is put back as stored (mine, build, paint)")
    check(not said(ra, "This area is protected"), "the notice is paced, not repeated per edit")
    # Build into the sky: the block is taken back out.
    a.send("ACTION:65512:45:65512:0:5")
    check(restores(a.lines(0.5)) == ["ACTION:server:0:65512:45:65512:1"],
          "a refused build is taken back out (a mine)")
    # Paint the stored block: its real colour comes back.
    a.send("ACTION:65511:40:65511:3:30")
    check(restores(a.lines(0.5))[-1:] == ["ACTION:server:0:65511:40:65511:3:12"],
          "a refused paint restores the stored colour")
    # And outside the zone, nothing is different.
    a.send("ACTION:65530:33:65530:0:5")
    ra = a.lines(0.4)
    rb = b.lines(0.6)
    check(not restores(ra), "an edit outside the zone gets no restore")
    check(player_relays(rb) == ["ACTION:Alice:17:65530:33:65530:0:5"],
          "the peer saw the outside edit and none of the four refused ones")

    model = srv.model()
    check(model.get((65511, 40, 65511)) == (5, 12), "the protected stored block is unchanged in the model")
    check((65510, 32, 65510) not in model and (65512, 45, 65512) not in model,
          "refused edits stored nothing")
    check(model.get((65530, 33, 65530)) == (5, 0), "the outside build is stored")
    audit = srv.since(m)
    check(said(audit, "player:Alice denied mine in zone spawn at 65510,32,65510"),
          "the first refusal is audited")
    check(sum("denied" in ln for ln in audit) == 1,
          "the other three fold into the same window (one audit line)")
    a.close(); b.close()


# --- group 2: signs ------------------------------------------------------------

def group_signs(srv):
    print("\n[2] sign writes inside a zone")
    a, b = Client(), Client()
    a.join("Carol"); b.join("Dan")
    a.lines(0.2); b.lines(0.2)
    # Overwrite the existing sign: the original comes back to the writer only.
    a.send("SIGNP:65505:33:65505:0:0:0:griefed")
    ra, rb = a.lines(0.5), b.lines(0.3)
    check("SIGNP:server:65505:33:65505:0:0:0:hello" in ra, "the writer gets the original sign back")
    check(not any("griefed" in ln for ln in rb) and not any(ln.startswith("SIGNP") for ln in rb),
          "the peer sees nothing")
    check(said(ra, "This area is protected ('spawn')"), "the writer is told why")
    # A new sign in an empty slot: nothing to send back, nothing stored.
    a.send("SIGNP:65505:33:65505:2:0:0:new sign")
    check(not any(ln.startswith("SIGNP") for ln in a.lines(0.4)), "a refused new sign gets no SIGNP back")
    a.send("SIGNQ")
    burst = [ln for ln in a.lines(0.6) if ln.startswith("SIGNP:")]
    check(burst == ["SIGNP:server:65505:33:65505:0:0:0:hello"], "SIGNQ still answers with the original sign only")
    # Mining the signed block: the block comes back, and its sign is sent after it.
    a.send("ACTION:65505:33:65505:1")
    ra = a.lines(0.5)
    check(ra[-1:] == ["SIGNP:server:65505:33:65505:0:0:0:hello"] and
          "ACTION:server:0:65505:33:65505:0:5" in ra,
          "a restored signed block is followed by its SIGNP")
    ctl_cmd(srv.sock, "save", 0.4)
    with open(os.path.join(srv.dir, "eden_signs.txt")) as f:
        body = f.read()
    check("griefed" not in body and "new sign" not in body and "hello" in body,
          "eden_signs.txt holds only the original sign")
    a.close(); b.close()


# --- group 3: a burn at the zone edge -----------------------------------------

def group_burn(srv):
    print("\n[3] a burn outside the zone whose blast reaches in")
    a, b = Client(), Client()
    a.join("Erin"); b.join("Finn")
    a.lines(0.2); b.lines(0.2)
    a.send("ACTION:65497:60:65510:2")          # TNT 3 west of the zone edge
    ra, rb = a.lines(0.8), b.lines(0.8)

    check(player_relays(rb) == ["ACTION:Erin:17:65497:60:65510:2"], "the burn is relayed to the peer")
    rest_b, rest_a = restores(rb), restores(ra)
    check(bool(rest_b) and bool(rest_a), "both clients get a restore")
    check(rb.index("ACTION:Erin:17:65497:60:65510:2") < rb.index(rest_b[0]),
          "the peer's restore arrives after the relay that makes it run the blast")
    check("ACTION:server:0:65501:60:65510:0:5" in rest_a and "ACTION:server:0:65501:60:65510:0:5" in rest_b,
          "the protected block is redrawn on both screens")
    check("ACTION:server:0:65502:60:65512:0:9" in rest_b,
          "the protected TNT is redrawn (it went off on the clients)")
    # The protected TNT's own sphere is restored too: a cell outside the zone that only
    # its blast (which the clients ran and the server did not) could have reached.
    check("ACTION:server:0:65499:60:65517:1" in rest_b,
          "the protected TNT's sphere is restored, beyond the zone")
    # A restore is the model as it is now, so a cell the server did destroy — the
    # burned TNT, the block beside it — is at most redrawn as air, never rebuilt.
    check(not any(ln.startswith(("ACTION:server:0:65497:60:65510:0:", "ACTION:server:0:65495:60:65510:0:"))
                  for ln in rest_b),
          "nothing the server destroyed is rebuilt")
    check(said(ra, "This area is protected ('spawn')"), "the burner is told")

    model = srv.model()
    check(model.get((65501, 60, 65510)) == (5, 0), "the protected block is intact in the model")
    check(model.get((65502, 60, 65512)) == (9, 0), "the protected TNT is intact: not chained")
    check(model.get((65495, 60, 65510)) == (0, 0), "the unprotected block in the blast is gone")
    check(not any(in_zone(x, z) for (x, y, z) in model if y >= 54 and model[(x, y, z)] == (0, 0)),
          "no cell inside the zone was stored as air")
    a.close(); b.close()


# --- group 4: WorldEdit --------------------------------------------------------

def group_worldedit(srv):
    print("\n[4] WorldEdit across the zone edge")
    ctl_cmd(srv.sock, "op:Gwen:1")
    a = Client(); a.join("Gwen"); a.lines(0.2)
    a.cmd("//pos1 65518 80 65505", 0.3)
    a.cmd("//pos2 65523 80 65505", 0.3)
    r = a.cmd("//set 5", 0.6)
    check(said(r, "//set: 3 block(s) changed"), "the three outside cells change")
    check(said(r, "3 cell(s) skipped: protected area 'spawn'."), "the three inside are skipped and reported")
    replies = [ln for ln in r if ln.startswith("[Server]")]
    check(bool(replies) and replies[-1] == "[Server] //set: 3 block(s) changed.",
          "the completion line is still the last reply (commands.md contract)")
    xs = sorted(int(ln.split(":")[3]) for ln in restores(r) if ln.endswith(":0:5"))
    check(xs == [65521, 65522, 65523], "only the outside cells are relayed")
    model = srv.model()
    check(all((x, 80, 65505) not in model for x in (65518, 65519, 65520)), "the inside cells are not stored")
    r = a.cmd("//undo", 0.6)
    check(said(r, "Undid 3 block(s)"), "//undo reverses the three that changed")
    # A //paste straight into the zone: everything skipped.
    a.cmd("//pos1 65530 80 65530", 0.3)
    a.cmd("//pos2 65531 80 65530", 0.3)
    a.cmd("//set 7", 0.5)
    a.at(65530, 81, 65530)
    a.cmd("//copy", 0.4)
    a.at(65510, 81, 65510)
    r = a.cmd("//paste", 0.6)
    check(said(r, "//paste: 0 block(s) changed") and said(r, "2 cell(s) skipped"),
          "a //paste into the zone changes nothing")
    a.close()


# --- group 5: the control socket bypasses zones --------------------------------

def group_control_bypass(srv):
    print("\n[5] the control socket is not subject to zones")
    b = Client(); b.join("Hank"); b.lines(0.2)
    r = ctl_cmd(srv.sock, "fill:65510:90:65510:65511:90:65511:7")
    check(said(r, "ok: filled 4 cells"), "`fill` inside the zone applies")
    r = ctl_cmd(srv.sock, "setblock:65512:90:65512:6")
    check(said(r, "ok: set 1 block"), "`setblock` inside the zone applies")
    rb = restores(b.lines(0.5))
    check("ACTION:server:0:65510:90:65510:0:7" in rb and "ACTION:server:0:65512:90:65512:0:6" in rb,
          "both are relayed to players")
    model = srv.model()
    check(model.get((65511, 90, 65511)) == (7, 0) and model.get((65512, 90, 65512)) == (6, 0),
          "both are in the model")
    b.close()


# --- group 5b: the zone:* control verbs (stage 8.3) ----------------------------

def group_control_verbs(d):
    print("\n[5b] zone:* control verbs (stage 8.3)")
    make_world(d, zones=())   # start with none — this group manages its own
    srv = Server(d, "--zone-revert-delay-ms", "0")
    try:
        r = ctl_cmd(srv.sock, "zones")
        check(r == ["no protected zones"], "empty set")

        r = ctl_cmd(srv.sock, "zone:add:hut:100:0:100:110:20:110")
        check(said(r, "ok: zone 'hut' added"), "add with default flags (all)")
        r = ctl_cmd(srv.sock, "zones")
        check(said(r, "1 zone(s):") and said(r, "hut") and said(r, "(100,0,100)..(110,20,110)") and said(r, "all"),
              "listed with bounds and flags")

        # enforced immediately, no restart: a player edit inside is denied.
        a = Client(); a.join("Zoe"); a.lines(0.2)
        a.send("ACTION:105:10:105:1")
        check(said(a.lines(0.5), "[Server] This area is protected ('hut')."), "newly-added zone enforces at once")

        # duplicate name is refused, original untouched
        r = ctl_cmd(srv.sock, "zone:add:hut:0:0:0:1:1:1")
        check(said(r, "error:") and said(r, "duplicate"), "duplicate add refused")

        # unknown flag is refused, nothing added
        r = ctl_cmd(srv.sock, "zone:add:bad:0:0:0:1:1:1:nope")
        check(said(r, "error:"), "unknown flag refused")
        check(not said(ctl_cmd(srv.sock, "zones"), "bad"), "the rejected zone was not added")

        # set moves/resizes, keeping the flag
        r = ctl_cmd(srv.sock, "zone:set:hut:200:0:200:210:20:210")
        check(said(r, "ok: zone 'hut' resized"), "set resizes")
        r = ctl_cmd(srv.sock, "zones")
        check(said(r, "(200,0,200)..(210,20,210)"), "the new bounds are listed")
        # the old box is unprotected now, the new one is. The notice is paced
        # (<= 1 per 10 s per connection — stage 8.2), so a rapid repeat denial
        # is asserted through the restore, not the (suppressed) chat line.
        a.send("ACTION:105:10:105:1")
        check(not restores(a.lines(0.3)), "the old box no longer protects")
        a.send("ACTION:205:10:205:1")
        check(bool(restores(a.lines(0.5))), "the new box protects")

        # set on an unknown name is refused
        r = ctl_cmd(srv.sock, "zone:set:ghost:0:0:0:1:1:1")
        check(said(r, "error:") and said(r, "no zone named"), "set on an unknown name refused")

        # flags toggles enforcement without touching bounds
        r = ctl_cmd(srv.sock, "zone:flags:hut:off")
        check(said(r, "ok: zone 'hut' flags updated"), "flags off")
        a.send("ACTION:205:10:205:0:5")
        check(not restores(a.lines(0.4)), "an 'off' zone no longer enforces")
        r = ctl_cmd(srv.sock, "zone:flags:hut:all")
        check(said(r, "ok: zone 'hut' flags updated"), "flags back to all")
        a.send("ACTION:206:10:206:1")
        check(bool(restores(a.lines(0.5))), "re-enforced")

        # a level is accepted and explained; with no PINs issued yet, nobody can use it
        r = ctl_cmd(srv.sock, "zone:flags:hut:all:2")
        check(said(r, "bypassed by players logged in at level 2+") and said(r, "no name has a PIN yet"),
              "level accepted, with who can bypass it (nobody yet)")
        r = ctl_cmd(srv.sock, "zone:flags:hut:all:3")
        check(said(r, "error:") and said(r, "level out of range"), "a level outside 0..2 is refused")
        r = ctl_cmd(srv.sock, "zones")
        check(said(r, "level 2"), "the level is listed")

        # rm removes it; a further edit there is unprotected
        r = ctl_cmd(srv.sock, "zone:rm:hut")
        check(said(r, "ok: removed zone 'hut'"), "rm removes")
        r = ctl_cmd(srv.sock, "zone:rm:hut")
        check(said(r, "error:") and said(r, "no zone named"), "rm on a missing name refused")
        r = ctl_cmd(srv.sock, "zones")
        check(r == ["no protected zones"], "the set is empty again")

        # on-disk file round-trips: a fresh add followed by an out-of-band edit + reload
        ctl_cmd(srv.sock, "zone:add:museum:0:0:0:10:10:10")
        with open(os.path.join(d, "eden_zones.txt")) as f:
            saved = f.read()
        check("museum:0:0:0:10:10:10:all" in saved, "add is durably persisted, not just in memory")
        with open(os.path.join(d, "eden_zones.txt"), "a") as f:
            f.write("gate:20:0:20:21:5:21:all\n")
        r = ctl_cmd(srv.sock, "zone:reload")
        check(said(r, "ok: reloaded 2 zone(s)"), "reload picks up a hand-edited addition")
        r = ctl_cmd(srv.sock, "zones")
        check(said(r, "museum") and said(r, "gate"), "both zones present after reload")

        # a malformed file on reload is refused, in-memory zones unchanged
        with open(os.path.join(d, "eden_zones.txt"), "a") as f:
            f.write("broken:not:a:number:here:1:2:all\n")
        r = ctl_cmd(srv.sock, "zone:reload")
        check(said(r, "error:"), "a malformed reload is refused")
        r = ctl_cmd(srv.sock, "zones")
        check(said(r, "museum") and said(r, "gate"), "the last-good in-memory set survives a bad reload")

        a.close()
        check(srv.alive(), "the server survived the pass")
    finally:
        srv.stop()


# --- group 6: coalescing under a revert delay ---------------------------------

def group_coalescing(d):
    print("\n[6] 100 mines on one protected cell, --zone-revert-delay-ms 400")
    make_world(d)
    srv = Server(d, "--zone-revert-delay-ms", "400")
    try:
        a = Client(); a.join("Ivan"); a.lines(0.2)
        m = srv.mark()
        t0 = time.time()
        for _ in range(100):
            a.send("ACTION:65510:32:65510:1")
        early = restores(a.lines(0.25))
        check(not early, "no restore before the delay")
        late = restores(a.lines(0.8))
        check(late == ["ACTION:server:0:65510:32:65510:1", "ACTION:server:0:65510:32:65510:0:8"],
              f"one restore for 100 denials ({len(late)} line(s))")
        check(time.time() - t0 > 0.4, "it arrived after the delay")
        audit = srv.since(m)
        check(sum("denied mine in zone spawn" in ln for ln in audit) == 1,
              "one audit line for the burst (the rest are folded)")
        # After it has gone out, a fresh denial queues a fresh restore.
        a.send("ACTION:65510:32:65510:1")
        check(len(restores(a.lines(0.8))) == 2, "a later denial is restored again")
        check(srv.alive(), "the server survived")
        a.close()
    finally:
        srv.stop()


# --- group 7: denied edits spend the ACTION budget ----------------------------

def group_budget(d):
    print("\n[7] a refused edit still spends the sender's ACTION budget")
    make_world(d)
    srv = Server(d, "--zone-revert-delay-ms", "0", "--action-burst", "5", "--action-rate", "0.01")
    try:
        a, b, c = Client(), Client(), Client()
        a.join("Jo"); b.join("Kim"); c.join("Lee")
        for s in (a, b, c):
            s.lines(0.2)
        for _ in range(5):
            a.send("ACTION:65510:32:65510:1")        # five refused mines: the whole burst
        a.lines(0.3)
        a.send("ACTION:65540:33:65540:0:5")          # outside the zone, but no tokens left
        c.send("ACTION:65541:33:65541:0:5")          # another player's own budget is untouched
        rb = player_relays(b.lines(0.6))
        check("ACTION:Lee:17:65541:33:65541:0:5" in rb, "control: a fresh player's edit relays")
        check(not any(ln.startswith("ACTION:Jo:") for ln in rb),
              "the edit after five refusals was dropped by the rate limit")
        for s in (a, b, c):
            s.close()
    finally:
        srv.stop()


# --- group 8: a malformed zones file is fatal ---------------------------------

def group_bad_file(d):
    print("\n[8] a malformed eden_zones.txt stops the server")
    make_world(d, zones=(ZONE, "museum:1:2:3:4:5:6:nobuild"))
    p = subprocess.run([SERVER, "--port", str(PORT), "--no-control-socket"], cwd=d,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=10)
    check(p.returncode == 2, f"exit code 2 (got {p.returncode})")
    check("eden_zones.txt:3: unknown flag" in p.stdout and "Refusing to start" in p.stdout,
          "the file, line and reason are named")


# --- group 9: player identity (stage 8.6) --------------------------------------

import re, stat

def who_row(lines, name):
    for ln in lines:
        if ln.startswith("  " + name + " (T"):
            return ln
    return ""

def issue_pin(srv, name):
    r = ctl_cmd(srv.sock, "passwd:" + name)
    m = re.search(r"ok: PIN for .*?: (\d{8}) ", " ".join(r))
    return m.group(1) if m else None

def group_identity(d):
    print("\n[9] PINs, /login, levels and zone bypass (stage 8.6)")
    make_world(d, zones=("vault:65600:0:65600:65610:255:65610:all:2",))
    srv = Server(d, "--zone-revert-delay-ms", "0")
    try:
        ctl_cmd(srv.sock, "op:Mia:2")
        ctl_cmd(srv.sock, "op:Oz:2")
        pin = issue_pin(srv, "Mia")
        check(pin is not None, "passwd replies with an 8-digit PIN")
        auth = os.path.join(d, "eden_auth.txt")
        check(os.path.exists(auth) and stat.S_IMODE(os.stat(auth).st_mode) == 0o600, "eden_auth.txt is 0600")
        with open(auth) as f:
            body = f.read()
        check("Mia:pbkdf2-sha256:" in body and pin not in body, "the file holds a hash, never the PIN")
        r = ctl_cmd(srv.sock, "pins")
        check(said(r, "1 name(s) with a PIN") and said(r, "Mia  offline"), "pins lists Mia, offline")

        mia, peer, oz = Client(), Client(), Client()
        j = mia.join("Mia")
        check(said(j, "The name Mia is protected. Type /login <pin>"), "a PIN-protected name is told how to log in")
        peer.join("Pip"); oz.join("Oz")
        for c in (mia, peer, oz):
            c.lines(0.2)
        r = ctl_cmd(srv.sock, "who")
        check("level 0" in who_row(r, "Mia") and who_row(r, "Mia").endswith("auth unverified"),
              "before /login: who shows Mia at the default level, unverified")
        check("level 2" in who_row(r, "Oz") and who_row(r, "Oz").endswith("auth none"),
              "a name with no PIN keeps its level by name (auth none)")
        r = mia.cmd("//pos1 1 40 1")
        check(said(r, "You do not have permission"), "unverified: the op's WorldEdit is refused")

        wrong = pin[:-1] + str((int(pin[-1]) + 1) % 10)
        r = mia.cmd("/login " + wrong)
        check(said(r, "Wrong PIN."), "a wrong PIN is refused")
        r = mia.cmd("/login 12ab")
        check(said(r, "Wrong PIN."), "a malformed PIN is refused")
        r = mia.cmd("/login " + pin)
        check(said(r, "Logged in as Mia. Your level is 2."), "the right PIN logs in, at the file level")
        r = mia.cmd("/login " + pin)
        check(said(r, "already logged in"), "a second /login is a no-op")
        r = ctl_cmd(srv.sock, "who")
        check("level 2" in who_row(r, "Mia") and who_row(r, "Mia").endswith("auth verified"),
              "after /login: who shows verified, level 2")
        r = ctl_cmd(srv.sock, "pins")
        check(said(r, "Mia  online verified"), "pins shows the live session")
        r = mia.cmd("//pos1 1 40 1")
        check(said(r, "pos1 = 1, 40, 1"), "verified: WorldEdit works")
        r = peer.cmd("/login 12345678")
        check(said(r, "has no PIN"), "/login on a name without a PIN says so")

        # Zone bypass: only the logged-in op gets through the level-2 zone.
        peer.lines(0.1)
        mia.send("ACTION:65605:33:65605:0:5")
        rm = mia.lines(0.4)
        rp = player_relays(peer.lines(0.4))
        check(not restores(rm) and "ACTION:Mia:17:65605:33:65605:0:5" in rp,
              "a logged-in level-2 player bypasses a level-2 zone (relayed, not restored)")
        oz.send("ACTION:65606:33:65606:0:5")
        check(bool(restores(oz.lines(0.4))), "an op by name alone (no PIN) is still held by the zone")
        check(not any(ln.startswith("ACTION:Oz:") for ln in player_relays(peer.lines(0.3))),
              "...and not relayed")
        check(srv.model().get((65605, 33, 65605)) == (5, 0), "the bypassing build is stored")

        # Re-issuing the PIN logs the live session out.
        pin2 = issue_pin(srv, "Mia")
        check(pin2 is not None and said(mia.lines(0.4), "Your PIN was changed"), "passwd again: Mia is told")
        r = ctl_cmd(srv.sock, "who")
        check(who_row(r, "Mia").endswith("auth unverified") and "level 0" in who_row(r, "Mia"),
              "...and is logged out")
        # A fresh connection: this one has spent its login attempts.
        mia.close(); mia = Client(); mia.join("Mia"); mia.lines(0.2)
        check(said(mia.cmd("/login " + pin), "Wrong PIN."), "the old PIN no longer works")
        check(said(mia.cmd("/login " + pin2), "Logged in as Mia"), "the new one does")

        # unpasswd: back to by-name.
        r = ctl_cmd(srv.sock, "unpasswd:Mia")
        check(said(r, "ok: removed the PIN for Mia"), "unpasswd")
        r = ctl_cmd(srv.sock, "who")
        check(who_row(r, "Mia").endswith("auth none") and "level 2" in who_row(r, "Mia"),
              "after unpasswd: level by name again, auth none")
        check(said(ctl_cmd(srv.sock, "unpasswd:Mia"), "has no PIN"), "unpasswd twice is refused")
        check(said(ctl_cmd(srv.sock, "passwd:bad]name"), "not a valid player name"), "passwd validates the name")

        # Per-IP lockout: five wrong PINs in a minute from one address, across
        # connections (a reconnect resets only the per-connection budget).
        ctl_cmd(srv.sock, "passwd:Ned")
        m = srv.mark()
        locked = False
        for _ in range(6):
            c = Client(); c.join("Ned"); c.lines(0.2)
            r = c.cmd("/login 00000000", 0.4)
            c.close()
            if said(r, "Too many wrong PINs from your address"):
                locked = True
                break
        check(locked, "repeated wrong PINs lock the address out")
        check(any("/login locked out 127.0.0.1" in ln for ln in srv.since(m)), "the lockout is audited")

        # Nothing typed after /login ever reached the log.
        log = "\n".join(srv.since(0, 0.2))
        check(pin not in log and pin2 not in log and wrong not in log and "00000000" not in log,
              "no PIN (right or wrong) appears in the server output")
        check(said(srv.since(0, 0), "control passwd Mia (issued)"), "passwd is audited (without the PIN)")
        for c in (mia, peer, oz):
            c.close()
        check(srv.alive(), "the server survived")
    finally:
        srv.stop()

    # Per-connection pacing, with the per-IP lockout off so it is the only guard.
    srv = Server(d, "--auth-fail-limit", "0")
    try:
        ctl_cmd(srv.sock, "passwd:Ned")
        ned = Client(); ned.join("Ned"); ned.lines(0.2)
        r = ned.cmd("/login 1", 0.3)
        check(said(r, "A PIN is 8 digits"), "a malformed PIN is refused for free")
        replies = [ned.cmd("/login 00000000", 0.4) for _ in range(4)]
        check(all(said(x, "Wrong PIN.") for x in replies[:3]) and said(replies[3], "Too many login attempts"),
              "3 attempts at once, the 4th paced (the malformed one cost nothing)")
        ned.close()
    finally:
        srv.stop()

    # A malformed auth file refuses the start.
    with open(os.path.join(d, "eden_auth.txt"), "a") as f:
        f.write("Bob:pbkdf2-sha256:100000:zz:zz\n")
    p = subprocess.run([SERVER, "--port", str(PORT), "--no-control-socket"], cwd=d,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=10)
    check(p.returncode == 2 and "eden_auth.txt:" in p.stdout and "bad salt" in p.stdout,
          f"a malformed eden_auth.txt: exit 2 naming line and reason (got {p.returncode})")


# --- group 10: topmap (stage 8.5) ---------------------------------------------

def group_topmap(d):
    print("\n[10] topmap")
    make_world(d, cells=[(1000, 32, 2000, 0, 0),      # mined grass
                         (1001, 33, 2000, 5, 3),      # a block on top
                         (1002, 32, 2000, 255, 12)],  # painted grass
               zones=())
    srv = Server(d)
    try:
        r = ctl_cmd(srv.sock, "topmap:1000:2000:1003:2001:1")
        check(r[:1] == ["ok: topmap 1000 2000 1003 2001 1 4 2"], "header")
        check(r[1:3] == ["31,3,0 33,5,3 32,8,12 -", "- - - -"], f"surface rule per column ({r[1:3]})")
        r = ctl_cmd(srv.sock, "topmap:0:0:1000:1000:1")
        check(said(r, "error: too many samples") and said(r, "use step >= 4"), "the cap is enforced, with a step that fits")
        r = ctl_cmd(srv.sock, "topmap:1:2:3")
        check(said(r, "usage:"), "wrong field count")
        r = ctl_cmd(srv.sock, "topmap:0:0:4080:4080:16", 1.5)
        rows = r[1:]
        check(r[0] == "ok: topmap 0 0 4080 4080 16 256 256" and len(rows) == 256 and
              all(len(x.split(" ")) == 256 for x in rows), "a full 256x256 reply arrives whole")
        check(srv.alive(), "the server survived")
    finally:
        srv.stop()


# --- group 11: in-game /zone (stage 8.7) ---------------------------------------

def group_zone_cmd(d):
    print("\n[11] in-game /zone")
    make_world(d, zones=())
    srv = Server(d, "--zone-revert-delay-ms", "0")
    try:
        ctl_cmd(srv.sock, "op:Pat:2")
        ctl_cmd(srv.sock, "op:Quin:2")
        ctl_cmd(srv.sock, "op:Ray:1")
        pin = issue_pin(srv, "Pat")
        pat, quin, ray = Client(), Client(), Client()
        pat.join("Pat"); quin.join("Quin"); ray.join("Ray")
        for c in (pat, quin, ray):
            c.lines(0.2)
        check(said(pat.cmd("/zone list"), "You do not have permission"),
              "a PIN-protected op not logged in is at the default level: refused")
        check(said(quin.cmd("/zone list"), "need you to be logged in"), "an op with no PIN at all: refused")
        check(said(ray.cmd("/zone list"), "You do not have permission"), "level 1: refused")
        pat.cmd("/login " + pin)
        check(said(pat.cmd("/zone list"), "No protected zones."), "logged-in op: list works")
        check(said(pat.cmd("/zone create gate"), "Set //pos1 and //pos2 first"), "create needs a selection")
        pat.cmd("//pos1 3000 40 3000", 0.3)
        pat.cmd("//pos2 3005 45 3004", 0.3)
        r = pat.cmd("/zone create gate")
        check(said(r, "Protected zone 'gate' (3000,0,3000)..(3005,255,3004) created"),
              "create: the selection's x/z, the whole column")
        check(said(pat.cmd("/zone create gate"), "duplicate name"), "a duplicate name is refused")
        r = pat.cmd("/zone create keep exact")
        check(said(r, "'keep' (3000,40,3000)..(3005,45,3004) created"), "`exact` keeps the selection's heights")
        check(said(pat.cmd("/zone create bad!name"), "invalid name"), "zone name rules apply")
        with open(os.path.join(d, "eden_zones.txt")) as f:
            check("gate:3000:0:3000:3005:255:3004:all" in f.read(), "persisted to eden_zones.txt")
        quin.send("ACTION:3002:10:3002:1")
        check(bool(restores(quin.lines(0.4))), "enforced at once, with no restart")
        r = pat.cmd("/zone list")
        check(said(r, "--- zones 1/1 (2) ---") and said(r, "gate (3000,0,3000)..(3005,255,3004)"), "list")
        pat.at(3002, 41, 3002)
        check(said(pat.cmd("/zone here"), "You are in: gate, keep."), "here names the zones at your feet")
        pat.at(3100, 41, 3100)
        check(said(pat.cmd("/zone here"), "not in a protected zone"), "here, outside")
        check(said(pat.cmd("/zone rm gate"), "Removed zone 'gate'."), "rm")
        check(said(pat.cmd("/zone rm gate"), "no zone named 'gate'"), "rm twice")
        check(said(pat.cmd("/zone frobnicate"), "Usage: /zone"), "unknown sub-command: usage")
        quin.send("ACTION:3002:10:3002:1")
        check(not restores(quin.lines(0.4)), "a removed zone no longer enforces")
        out = srv.since(0, 0.2)
        check(any("player:Pat (level 2) /zone create gate" in ln for ln in out), "zone changes are audited")
        for c in (pat, quin, ray):
            c.close()
        check(srv.alive(), "the server survived")
    finally:
        srv.stop()


# --- main ---------------------------------------------------------------------

WORLD = [
    (65511, 40, 65511, 5, 12),    # a painted stored block in the zone
    (65505, 33, 65505, 5, 0),     # the signed block
    (65501, 60, 65510, 5, 0),     # in the zone, inside the burn's blast
    (65502, 60, 65512, 9, 0),     # a protected TNT inside the blast
    (65495, 60, 65510, 5, 0),     # outside the zone, inside the blast
    (65497, 60, 65510, 9, 0),     # the TNT that gets burned
]
SIGNS = ["65505:33:65505:0:0:0:hello"]


def main():
    if not os.path.exists(SERVER):
        print(f"no server at {SERVER} — run ./build_server.sh first")
        return 2
    d = tempfile.mkdtemp(prefix="edenphase8_")
    try:
        make_world(d, WORLD, SIGNS)
        srv = Server(d, "--zone-revert-delay-ms", "0")
        try:
            group_action(srv)
            group_signs(srv)
            group_burn(srv)
            group_worldedit(srv)
            group_control_bypass(srv)
            check(srv.alive(), "the server survived the pass")
        finally:
            srv.stop()
        for group in (group_control_verbs, group_coalescing, group_budget, group_bad_file,
                      group_identity, group_topmap, group_zone_cmd):
            wd = tempfile.mkdtemp(prefix="edenphase8x_")
            try:
                group(wd)
            finally:
                shutil.rmtree(wd, ignore_errors=True)
    finally:
        shutil.rmtree(d, ignore_errors=True)

    print()
    if fails:
        print(f"phase8_live_test: {len(fails)} check(s) FAILED")
        for f_ in fails:
            print("  - " + f_)
        return 1
    print("phase8_live_test: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
