#!/usr/bin/env python3
"""Live socket test for ROADMAP-SERVER stages 7.3 / 7.4 / 7.9 / 7.16 / 7.17
(per-client output, shutdown behaviour, movement-field validation, the pre-JOIN
admission gate).

    ./build_server.sh && python3 phase7_live_test.py [path/to/edenserver]

`out_queue_test` proves the queue *policy* is right. This one proves the server
actually routes every write through it, over real sockets, under the conditions
that made the bug visible in the first place — and those conditions are the whole
point: the failure is invisible on a fast link, so a test that reads promptly
proves nothing. Every group here needs a client that reads *slowly*, which is why
this is a hand-run test and not part of build_server.sh.

Covers:
  1  7.3 — a slow reader taking a multi-frame SNAPZ burst while another player
         moves at 50 Hz: zero short frames, zero undecodable frames, zero orphan
         lines, and every record delivered. Before the fix this produced roughly
         one destroyed frame and one truncated frame per 45 s — and because
         records are sorted (z,x,y,flag) and framed at a flat 3000, a lost frame
         is one 1-block-wide row of the world, the "reset in strips" report.
  2  7.4 — a third client's JOIN completes in under 2 s while another client is
         backed up. Before the fix it waited 20 s and unblocked only when the
         stalled client's socket closed.
  3  7.3 — a client that never drains is disconnected by --client-write-timeout
         instead of parking a writer thread forever.
  4  7.5 (the half Part A fixes) — the name of a player who vanished mid-burst is
         free immediately, not after a TCP retransmit timeout. The auto-suffix
         behaviour itself is stage 7.5 and is not tested here.
  5  7.3 — a client over its region-queue depth has the request *refused* (it
         re-asks) rather than served a partial burst.
  6  7.3 — a client too far behind on *world state* is disconnected (it resyncs on
         rejoin) rather than quietly missing edits, and everyone else carries on.
  7  7.9 — SIGTERM (systemctl stop / docker stop / Ctrl-C) saves an edit made
         moments earlier and exits promptly, instead of the old behaviour of
         dying immediately and only ever persisting up to the last 15 s
         autosave tick.
  8  7.16 — a `POS` outside the world (`1e38`, `nan`, `inf`) is refused at ingest
         rather than stored, persisted, relayed, and handed back on the next join
         as a `SPAWN` line whose formatter over-read its 96-byte stack buffer and
         sent the spill to the client.
  9  7.17 — on a passworded server, a peer that never sent `JOIN` cannot edit the
         world, broadcast chat, or inject a phantom player. `PING` still answers,
         and the same edit from a joined player still lands.

Group 1 spends ~35 s deliberately reading at ~50 KB/s; the whole pass is ~1 min.
"""
import base64, os, re, shutil, socket, struct, subprocess, sys, tempfile, threading, time, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "edenserver")
PORT = 27097

fails = []
def check(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        fails.append(msg)


# --- the world ----------------------------------------------------------------

def write_dense_world(path):
    """A solid 201 x 201 x 11 slab around spawn: 444,411 cells, which is 149 SNAPZ
    frames / ~1.4 MB of wire for one REGION. Big enough that the burst takes tens
    of seconds to a weak link, which is the window the splice needed."""
    with open(path, "w") as f:
        for x in range(65400, 65601):
            for z in range(65400, 65601):
                for y in range(33, 44):
                    f.write("%d:%d:%d:5:0\n" % (x, y, z))


class Server:
    """A running ./edenserver with its stdout drained (an undrained pipe would
    deadlock it once full — this test makes the server log a lot)."""
    def __init__(self, world_dir, *extra):
        self.dir = world_dir
        self.out = []
        self._lock = threading.Lock()
        self.p = subprocess.Popen(
            [SERVER, "--port", str(PORT),
             "--world", os.path.join(world_dir, "eden_world.model"),
             "--signs", os.path.join(world_dir, "eden_signs.txt"),
             "--connect-limit", "0", "--no-control-socket",
             "--handshake-timeout", "0", "--idle-timeout-conn", "0", *extra],
            cwd=world_dir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self._t = threading.Thread(target=self._drain, daemon=True)
        self._t.start()
        for _ in range(400):
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.05)
        else:
            raise RuntimeError("server did not come up")

    def _drain(self):
        for line in self.p.stdout:
            with self._lock:
                self.out.append(line.rstrip("\n"))

    def mark(self):
        with self._lock:
            return len(self.out)

    def since(self, mark, settle=0.4):
        time.sleep(settle)
        with self._lock:
            return list(self.out[mark:])

    def alive(self):
        return self.p.poll() is None

    def stop(self):
        try:
            self.p.terminate()
            self.p.wait(timeout=5)
        except Exception:
            self.p.kill()


def join(name, rcvbuf=None, timeout=5.0):
    s = socket.socket()
    if rcvbuf:                       # before connect(), so it scales the window
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
    s.settimeout(timeout)
    s.connect(("127.0.0.1", PORT))
    s.sendall(("JOIN:%s:17:EDEN6:zr\n" % name).encode())
    return s


# --- group 1: frame integrity under a concurrent broadcast --------------------

KNOWN = (b"POS", b"VEL", b"POSVEL", b"ACTION", b"SIGNP", b"PONG", b"CAPS", b"SPAWN", b"SNAPZ")

def audit_stream(buf):
    """Split a client's received bytes into lines and grade every SNAPZ frame.

    A spliced frame shows up three ways, and all three are checked: the base64
    fails to decode, it inflates to fewer records than its own header claims
    (raw DEFLATE truncated mid-payload still inflates to a valid *prefix* — the
    reporter's "half reset" strip), or the bytes that displaced it surface as an
    orphan line belonging to no known verb.

    Only *complete* lines are graded. A trailing partial line is this test closing
    its own socket mid-line, not the server splitting one — counting it would fail
    the orphan check on our own timing."""
    res = dict(frames=0, ok=0, short=0, bad=0, orphan=0, records=0, lost=0, samples=[])
    lines = buf.split(b"\n")
    if not buf.endswith(b"\n"):
        lines = lines[:-1]
    for ln in lines:
        if not ln:
            continue
        if ln.startswith(b"SNAPZ:"):
            res["frames"] += 1
            try:
                _, cnt, b64 = ln.split(b":", 2)
                cnt = int(cnt)
            except Exception:
                res["bad"] += 1
                continue
            spliced = re.search(rb"(POSVEL|POS:|VEL:|\[Server\])", b64)
            try:
                raw = base64.b64decode(b64 + b"=" * (-len(b64) % 4), validate=False)
                out = zlib.decompressobj(-15).decompress(raw)
            except Exception:
                res["bad"] += 1
                res["samples"].append("undecodable frame (count=%d)%s" %
                                      (cnt, "  spliced: %r" % spliced.group(0) if spliced else ""))
                continue
            n = len(out) // 20
            res["records"] += n
            if n == cnt:
                res["ok"] += 1
            else:
                res["short"] += 1
                res["lost"] += cnt - n
                res["samples"].append("short frame: header %d, inflated %d%s" %
                                      (cnt, n, "  spliced: %r" % spliced.group(0) if spliced else ""))
        elif ln.split(b":", 1)[0] in KNOWN or ln.startswith(b"[Server]") or ln.startswith(b"["):
            pass
        else:
            res["orphan"] += 1
            if len(res["samples"]) < 6:
                res["samples"].append("orphan line (%d B): %r..." % (len(ln), ln[:40]))
    return res


def group1_frame_integrity(srv, total_records):
    print("\n[1] 7.3 — a SNAPZ burst to a slow reader, with another player moving")
    stop = threading.Event()

    # The mover has to be walking *before* the victim asks for terrain: the window
    # only opens while the victim's send buffer is already full.
    mover = join("mover")
    def spam():
        i = 0
        while not stop.is_set():
            try:
                mover.sendall(("POSVEL:65500.0:33.92:%0.2f:0.1:0:0.1\n" % (65500 + i % 40)).encode())
            except OSError:
                return
            i += 1
            time.sleep(0.02)                         # 50 Hz
    threading.Thread(target=spam, daemon=True).start()
    time.sleep(0.5)

    victim = join("victim", rcvbuf=2048)             # a weak link: tiny receive window
    buf = bytearray()
    def slow_reader():
        victim.settimeout(3.0)
        while not stop.is_set():
            try:
                d = victim.recv(512)
            except (socket.timeout, OSError):
                return
            if not d:
                return
            buf.extend(d)
            time.sleep(0.01)                         # ~50 KB/s
    threading.Thread(target=slow_reader, daemon=True).start()
    time.sleep(0.3)
    victim.sendall(b"REGION:65500:65500\n")

    # Long enough for all 149 frames to drain at ~50 KB/s, plus slack.
    deadline = time.time() + 60
    while time.time() < deadline:
        time.sleep(0.5)
        if audit_stream(bytes(buf))["records"] >= total_records:
            break
    stop.set()
    time.sleep(1.0)
    for s in (victim, mover):
        try:
            s.close()
        except OSError:
            pass

    r = audit_stream(bytes(buf))
    print("       %d frame(s): %d ok, %d short, %d undecodable; %d orphan line(s); "
          "%d/%d records" % (r["frames"], r["ok"], r["short"], r["bad"], r["orphan"],
                             r["records"], total_records))
    for s in r["samples"]:
        print("       ! " + s)
    check(r["frames"] > 1, "the burst was multi-frame (the test is meaningful)")
    check(r["short"] == 0, "no SNAPZ frame arrived short of its own header count")
    check(r["bad"] == 0, "no SNAPZ frame arrived undecodable")
    check(r["orphan"] == 0, "no orphan bytes: nothing was displaced out of a line")
    check(r["lost"] == 0, "no records lost to truncation")
    check(r["records"] == total_records, "every record of the region was delivered")


# --- group 2: one slow client must not stall the server -----------------------

def group2_join_not_blocked(srv):
    print("\n[2] 7.4 — a backed-up client must not freeze anyone else's JOIN")
    stuck = join("stuckA", rcvbuf=2048)
    stuck.sendall(b"REGION:65500:65500\n")           # ...and never reads a byte of it
    time.sleep(2.0)

    mover = join("moverB")
    for _ in range(100):
        mover.sendall(b"POSVEL:65500:33.92:65500:0:0:0\n")
        time.sleep(0.01)

    t0 = time.time()
    c = socket.socket()
    c.settimeout(10.0)
    c.connect(("127.0.0.1", PORT))
    c.sendall(b"JOIN:newcomerC:17:EDEN6:zr\n")
    try:
        welcome = c.recv(256).split(b"\n")[0]
    except socket.timeout:
        welcome = b"<nothing>"
    dt = time.time() - t0
    print("       JOIN answered in %.2f s: %r" % (dt, welcome))
    check(b"Welcome, newcomerC" in welcome, "the third client was welcomed")
    check(dt < 2.0, "...in under 2 s while another client was backed up")

    # ...and so does a chat line, which took the same lock.
    t0 = time.time()
    mover.sendall(b"MSG:hello\n")
    try:
        c.settimeout(3.0)
        c.recv(512)
        chat_dt = time.time() - t0
    except socket.timeout:
        chat_dt = 99.0
    check(chat_dt < 2.0, "a chat line relays in under 2 s under the same stall")

    for s in (stuck, mover, c):
        try:
            s.close()
        except OSError:
            pass


# --- group 3: a client that never drains is dropped, not parked ---------------

def group3_write_timeout():
    print("\n[3] 7.3 — a client that never drains hits --client-write-timeout")
    d = tempfile.mkdtemp(prefix="ewb7-")
    try:
        write_dense_world(os.path.join(d, "eden_world.model"))
        open(os.path.join(d, "eden_signs.txt"), "w").close()
        srv = Server(d, "--client-write-timeout", "5")
        try:
            m = srv.mark()
            # ⚠️ This client must never recv() a single byte. Reading even slowly
            # counts as drain progress and resets the timeout — which is correct
            # behaviour, and was this group's own first bug.
            dead = join("neverreads", rcvbuf=2048)
            dead.sendall(b"REGION:65500:65500\n")
            t0 = time.time()
            logged = False
            while time.time() - t0 < 45:
                if "output stalled" in "\n".join(srv.since(m, settle=0.0)):
                    logged = True
                    break
                time.sleep(0.5)
            dt = time.time() - t0
            print("       server gave up on it after %.1f s" % dt)
            check(logged, "the writer stopped waiting and said so")

            # ...and it closed the connection: the peer sees a reset, not a
            # half-open socket it can keep writing to forever.
            gone = False
            for _ in range(20):
                try:
                    dead.sendall(b"PING\n")
                except OSError:
                    gone = True
                    break
                time.sleep(0.25)
            check(gone, "...and the stalled client's connection was closed")
            check(srv.alive(), "the server is still serving")
            dead.close()
        finally:
            srv.stop()
    finally:
        shutil.rmtree(d, ignore_errors=True)


# --- group 4: the name is free the moment the socket goes ---------------------

def group4_name_released(srv):
    print("\n[4] 7.5 (the half 7.3 fixes) — a name is free as soon as its socket is")
    a = join("td0", rcvbuf=2048)
    a.recv(256)
    a.sendall(b"REGION:65500:65500\n")
    time.sleep(1.0)                                  # let the burst start and block
    a.close()                                        # the player "leaves" mid-burst

    t0 = time.time()
    b = join("td0")
    try:
        reply = b.recv(256).split(b"\n")[0]
    except socket.timeout:
        reply = b"<nothing>"
    dt = time.time() - t0
    print("       rejoined %.2f s later: %r" % (dt, reply))
    check(b"Welcome, td0" in reply, "the same name was free again immediately")
    check(dt < 2.0, "...without waiting on the old thread's blocked send")
    b.close()


# --- group 5: backpressure refuses, it never truncates ------------------------

def group5_region_refused(srv):
    print("\n[5] 7.3 — over the region-queue depth, a REGION is refused, not truncated")
    c = join("greedy", rcvbuf=2048)
    m = srv.mark()
    # --client-region-queue is 2 by default; the 750 ms gap paces these, so four
    # requests to a client reading nothing is more than it can hold.
    for _ in range(4):
        c.sendall(b"REGION:65500:65500\n")
        time.sleep(0.9)
    log = "\n".join(srv.since(m, settle=1.0))
    refused = log.count("refused: this client's region queue")
    queued = log.count("frame(s) queued")
    print("       of 4 requests: %d queued a whole burst, %d refused" % (queued, refused))
    check(refused >= 1, "a request beyond the queue depth was refused")
    # The point of refusing: every request either produced a whole reply or none
    # of one. There is no third outcome, which is what "reset in strips" was.
    check(queued + refused == 4, "every request was either queued whole or refused")
    check(srv.alive(), "the server is still serving")
    c.close()


# --- group 6: a client too far behind on world state is dropped, not diverged ---

def group6_world_backlog():
    print("\n[6] 7.3 — a client too far behind on world state is dropped, not diverged")
    d = tempfile.mkdtemp(prefix="ewb7-")
    try:
        write_dense_world(os.path.join(d, "eden_world.model"))
        open(os.path.join(d, "eden_signs.txt"), "w").close()
        # A tiny world-state budget, so a few hundred relayed edits reach it. The
        # real default is 16 MB; the branch is the same one.
        srv = Server(d, "--client-world-max", "4096", "--client-write-timeout", "120")
        try:
            m = srv.mark()
            # A stalls with its socket buffer full, so nothing drains past it...
            a = join("backlogA", rcvbuf=2048)
            a.sendall(b"REGION:65500:65500\n")
            time.sleep(2.0)

            # ...and B edits the world, which has to reach A and cannot.
            b = join("editorB")
            c = join("watcherC")
            for i in range(600):
                b.sendall(("ACTION:%d:60:%d:0:5\n" % (65450 + i % 40, 65450 + i // 40)).encode())
                if i % 50 == 0:
                    time.sleep(0.05)

            log = "\n".join(srv.since(m, settle=2.0))
            print("       %s" % next((l for l in log.split("\n")
                                      if "behind on world updates" in l), "<no drop logged>"))
            check("behind on world updates" in log,
                  "the client over its world-state budget was dropped, with the numbers")
            check("Connection too slow" not in log, "...without a stray server-side error")

            gone = False
            for _ in range(40):
                try:
                    a.sendall(b"PING\n")
                except OSError:
                    gone = True
                    break
                time.sleep(0.25)
            check(gone, "...and its connection was closed")

            # The point of dropping it: everyone else is untouched.
            c.settimeout(3.0)
            try:
                c.recv(1 << 16)
            except socket.timeout:
                pass
            b.sendall(b"MSG:still here\n")
            try:
                heard = b"still here" in c.recv(1 << 16)
            except socket.timeout:
                heard = False
            check(heard, "another player is unaffected and still receiving")
            check(srv.alive(), "the server is still serving")
            for s_ in (a, b, c):
                try:
                    s_.close()
                except OSError:
                    pass
        finally:
            srv.stop()
    finally:
        shutil.rmtree(d, ignore_errors=True)


# --- group 7: SIGTERM saves instead of losing the last autosave window --------

def edmb_has_cell(blob, x, y, z, type_, color):
    """Is (x,y,z) stored as `type_`/`color` in a saved world? (stage 7.6)

    The save format is EDMB — a 16^3 chunk store — since 7.6; a world saved by an
    older build (or with --world-format text) is still the `x:y:z:type:color`
    text this checks for as a fallback. See docs/configuration.md.
    """
    if not blob.startswith(b"EDMB"):
        return ("%d:%d:%d:%d:%d" % (x, y, z, type_, color)) in blob.decode("utf-8", "replace")
    ver, chunks = struct.unpack_from("<IQ", blob, 4)
    if ver != 1:
        raise AssertionError("unknown EDMB version %d" % ver)
    want_chunk = (x >> 4, y >> 4, z >> 4)
    want_idx = ((y & 15) << 8) | ((z & 15) << 4) | (x & 15)
    o = 16
    for _ in range(chunks):
        cx, cy, cz, n = struct.unpack_from("<iiiI", blob, o)
        o += 16
        if (cx, cy, cz) == want_chunk:
            for _ in range(n):
                idx, t, col = struct.unpack_from("<HBB", blob, o)
                o += 4
                if idx == want_idx:
                    return t == type_ and col == color
        else:
            o += 4 * n
    return False


def group7_sigterm_saves():
    print("\n[7] 7.9 — SIGTERM saves in-flight edits, not just the last autosave tick")
    d = tempfile.mkdtemp(prefix="ewb7-")
    try:
        open(os.path.join(d, "eden_world.model"), "w").close()
        open(os.path.join(d, "eden_signs.txt"), "w").close()
        srv = Server(d)
        c = join("sigtermer")
        try:
            c.recv(256)                                   # the welcome/CAPS burst
            # An edit with the client still connected and never disconnecting —
            # only the SIGTERM path, not the per-client disconnect save, can be
            # what persists this.
            c.sendall(b"ACTION:65530:34:65530:0:9\n")
            time.sleep(0.3)

            t0 = time.time()
            srv.stop()                                    # Server.stop() -> SIGTERM
            dt = time.time() - t0
            print("       server exited %.2f s after SIGTERM" % dt)
            check(dt < 3.0, "shutdown was prompt, not a wait for the next accept()/idle tick")
            check(not srv.alive(), "the process actually exited")
        finally:
            try:
                c.close()
            except OSError:
                pass

        with open(os.path.join(d, "eden_world.model"), "rb") as f:
            saved = f.read()
        check(edmb_has_cell(saved, 65530, 34, 65530, 9, 0),
              "the in-flight block edit was on disk after SIGTERM, not lost")
    finally:
        shutil.rmtree(d, ignore_errors=True)


# --- group 8: movement floats are validated at ingest (stage 7.16) ------------

SPAWN_RE = re.compile(rb"^SPAWN:(-?\d+\.\d\d):(-?\d+\.\d\d):(-?\d+\.\d\d)$")

def read_for(sock, seconds):
    """Everything the peer sends us within `seconds` (it never closes on its own)."""
    buf = bytearray()
    end = time.time() + seconds
    while time.time() < end:
        sock.settimeout(max(0.05, end - time.time()))
        try:
            d = sock.recv(1 << 16)
        except socket.timeout:
            break
        except OSError:
            break
        if not d:
            break
        buf.extend(d)
    return bytes(buf)


def group8_movement_validation():
    print("\n[8] 7.16 — POS floats are validated at ingest, and SPAWN cannot over-read")
    d = tempfile.mkdtemp(prefix="ewb7-")
    try:
        open(os.path.join(d, "eden_world.model"), "w").close()
        open(os.path.join(d, "eden_signs.txt"), "w").close()
        srv = Server(d)
        try:
            # An observer stays joined for the whole group: the relay is the other
            # place a poisoned field used to reach — every peer, verbatim.
            obs = join("observer")
            read_for(obs, 0.5)

            victim = join("poisoner")
            read_for(victim, 0.5)
            # One good position first, so there is something legitimate to restore
            # to and the test can tell "refused" from "never stored".
            victim.sendall(b"POS:65540.00:34.92:65500.00\n")
            time.sleep(0.2)
            # The repro: a finite float that formats to 132 bytes in a 96-byte
            # buffer, plus the non-finite spellings that got there the same way.
            mark = srv.mark()
            # `POS:1e38:...` goes *last*: whatever a pre-fix server accepted last is
            # what it persisted, so anything after it would mask the defect.
            for bad in (b"POS:nan:nan:nan\n",
                        b"POS:inf:34:65500\n",
                        b"POS:-1:34:65500\n",
                        b"POSVEL:65540:34.92:65500:1e38:0:0\n",
                        b"VEL:inf:0:0\n",
                        b"POSVEL:1e38:1e38:1e38:0:0:0\n",
                        b"POS:1e38:1e38:1e38\n"):
                victim.sendall(bad)
                time.sleep(0.05)
            logged = srv.since(mark)
            check(any("Invalid POS" in l or "Invalid POSVEL" in l or "Invalid VEL" in l
                      for l in logged),
                  "the server logged the refusal (rate-limited, so one line is enough)")

            relayed = read_for(obs, 0.6)
            check(b"1e38" not in relayed and b"nan" not in relayed and b"inf" not in relayed,
                  "no poisoned movement field reached another player")
            check(srv.alive(), "the server survived the poisoned packets")

            victim.close()
            time.sleep(0.4)          # the disconnect path persists eden_players.txt

            # The over-read fired on the *next* join under the same name.
            again = join("poisoner")
            burst = read_for(again, 1.0)
            spawn = [l for l in burst.split(b"\n") if l.startswith(b"SPAWN:")]
            check(len(spawn) == 1, "the rejoin got exactly one SPAWN line")
            if spawn:
                m = SPAWN_RE.match(spawn[0])
                check(m is not None, "the SPAWN line is well formed (%r)" % spawn[0][:64])
                check(len(spawn[0]) < 96,
                      "the SPAWN line fits the buffer it is formatted in (%d B)" % len(spawn[0]))
                if m:
                    x, y, z = (float(g) for g in m.groups())
                    check((x, y, z) == (65540.00, 34.92, 65500.00),
                          "it restored the last *valid* position, not the poisoned one")
            # A 96-byte over-read spills printable junk from recvBuffer/username;
            # the join burst is all known verbs, so anything else is the leak.
            for ln in burst.split(b"\n"):
                if ln and not (ln.split(b":", 1)[0] in KNOWN or ln.startswith(b"[")):
                    check(False, "unexpected line in the join burst: %r" % ln[:64])
            again.close()
            obs.close()
        finally:
            srv.stop()

        with open(os.path.join(d, "eden_players.txt"), "rb") as f:
            rows = f.read()
        check(b"1e+38" not in rows and b"nan" not in rows and b"inf" not in rows,
              "nothing out of range was persisted to eden_players.txt (%r)" % rows[:80])
    finally:
        shutil.rmtree(d, ignore_errors=True)


# --- group 9: nothing but JOIN/PING is acted on before the handshake ----------

def group9_prejoin_gate():
    print("\n[9] 7.17 — ACTION/MSG/POS/VEL/POSVEL are gated on JOIN")
    d = tempfile.mkdtemp(prefix="ewb7-")
    try:
        open(os.path.join(d, "eden_world.model"), "w").close()
        open(os.path.join(d, "eden_signs.txt"), "w").close()
        # A passworded server is the case that matters: JOIN is the only
        # authentication there is, so a verb that skips it skips the password.
        # `join()` supplies EDEN6 as the password field the retail client sends.
        srv = Server(d, "--password", "EDEN6")
        try:
            obs = join("observer")
            read_for(obs, 0.5)

            mark = srv.mark()
            anon = socket.socket()
            anon.settimeout(5.0)
            anon.connect(("127.0.0.1", PORT))
            # PING brackets the batch: it is the one verb allowed through, so two
            # PONGs and nothing between them is the whole reply budget of an
            # unauthenticated peer.
            anon.sendall(b"PING\n"
                         b"ACTION:65500:34:65500:0:9\n"      # edit the world
                         b"MSG:hello from nobody\n"          # chat as Player<n>
                         b"POS:65500.00:34.92:65500.00\n"    # phantom player
                         b"VEL:1.00:0.00:1.00\n"
                         b"POSVEL:65501.00:34.92:65501.00:1.00:0.00:1.00\n"
                         b"REGION:65500:65500\n"             # already gated pre-7.17
                         b"SIGNQ\n"
                         b"SIGNP:65500:34:65500:0:0:0:nobody was here\n"
                         b"PING\n")
            reply = read_for(anon, 0.8)
            check(reply == b"PONG\nPONG\n",
                  "an unauthenticated peer gets PONG and nothing else (%r)" % reply[:120])

            relayed = read_for(obs, 0.6)
            check(relayed == b"",
                  "nothing from the unauthenticated peer reached a joined player (%r)" % relayed[:120])

            logged = [l for l in srv.since(mark) if "before JOIN" in l]
            check(len(logged) == 1,
                  "the refusal is logged once per connection, not once per line (%d)" % len(logged))
            check(srv.alive(), "the server survived the pre-JOIN batch")
            anon.close()

            # Positive control. The same ACTION, from the same kind of peer, after
            # a JOIN that carries the password — so a gate that simply dropped
            # everything would fail here rather than pass the checks above.
            mark = srv.mark()
            late = join("latecomer")
            read_for(late, 0.5)
            late.sendall(b"ACTION:65510:34:65510:0:9\n"
                         b"POS:65510.00:34.92:65510.00\n")
            time.sleep(0.3)
            seen = read_for(obs, 0.6)
            check(b"ACTION:latecomer:" in seen,
                  "a joined player's identical edit is still relayed (%r)" % seen[:120])
            late.close()
            obs.close()
            time.sleep(0.4)          # the disconnect path persists the world
        finally:
            srv.stop()

        with open(os.path.join(d, "eden_world.model"), "rb") as f:
            saved = f.read()
        check(not edmb_has_cell(saved, 65500, 34, 65500, 9, 0),
              "the pre-JOIN edit never reached the world model")
        check(edmb_has_cell(saved, 65510, 34, 65510, 9, 0),
              "...and the post-JOIN one did")

        players = os.path.join(d, "eden_players.txt")
        rows = open(players, "rb").read() if os.path.exists(players) else b""
        # Pre-JOIN movement was stored under the auto-assigned `Player<clientId>`;
        # the joined player's own POS still is, which is what tells the two apart.
        check(b"Player" not in rows,
              "no phantom Player<n> row in eden_players.txt (%r)" % rows[:80])
        check(b"latecomer:" in rows,
              "...but a joined player's position is still saved (%r)" % rows[:80])
    finally:
        shutil.rmtree(d, ignore_errors=True)


def main():
    if not os.path.exists(SERVER):
        print("no %s — run ./build_server.sh first" % SERVER)
        return 1
    d = tempfile.mkdtemp(prefix="ewb7-")
    print("world: %s" % d)
    write_dense_world(os.path.join(d, "eden_world.model"))
    open(os.path.join(d, "eden_signs.txt"), "w").close()
    total_records = 201 * 201 * 11        # one record per solid cell in the slab

    srv = Server(d)
    try:
        group1_frame_integrity(srv, total_records)
        group2_join_not_blocked(srv)
        group4_name_released(srv)
        group5_region_refused(srv)
        check(srv.alive(), "the server survived the whole pass")
    finally:
        srv.stop()
        shutil.rmtree(d, ignore_errors=True)

    group3_write_timeout()
    group6_world_backlog()
    group7_sigterm_saves()
    group8_movement_validation()
    group9_prejoin_gate()

    print()
    if fails:
        print("phase7_live_test: %d check(s) FAILED" % len(fails))
        for f in fails:
            print("  - " + f)
        return 1
    print("phase7_live_test: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
