#!/usr/bin/env python3
"""Live socket test for ROADMAP-SERVER stage 12.0 (the perf hotfix): coalesced
departure saves (12.0a) and the REGION encoder cap + per-connection record budget
(12.0b); and stage 12.1: deflate level 1 (12.1a) and the encoded-region cache
(12.1b).

    ./build_server.sh && python3 phase12_live_test.py [path/to/edenserver]

`save_sched_test`, `out_queue_test`, `protocol_test`, `region_test` and
`region_cache_test` prove the policies. This proves the server runs them over
real sockets.

Covers:
  1  12.0a — one player builds while 20 bare TCP connect/close (no JOIN) arrive in
         10 s. Before the fix each close forced a full world save (10 bare closes
         measured 11 saves). Now a never-joined close requests nothing.
  2  12.0a — 20 join -> build -> leave cycles back to back. Before the fix: 20
         saves in ~4 s. Now at most one per 5 s, and the last player's edit is
         still saved soon after they leave.
  3  12.0a — an edit, a leave, then `kill -9` 6 s later: the edit is on disk. The
         coalescing delays a departure's save by seconds, never drops it.
  4  12.0b — four clients re-ask for a dense box every 760 ms with
         --region-encoders 1. At most one frame deflates at a time, every frame
         decodes, a bystander's movement keeps reaching a flooder, and a client
         joining mid-flood gets its whole burst.
  5  12.0b — over the record budget a REGION is refused with silence, logged once,
         and the client stays connected and is served again once the budget refills.
  6  12.1b — a second client asking for the same box is answered from the cache
         with the same bytes; an ACTION in the box makes the next answer fresh, and
         it carries the edit; the one after that is cached again. Eight joiners at
         one spawn cost one encode.
  7  12.1a — the same box served at --region-deflate-level 1 (the default) and 6
         decodes to the same records in the same order; level 1 is the larger wire.

The whole pass is ~1.5 min.
"""
import base64, os, re, shutil, socket, subprocess, sys, tempfile, threading, time, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from phase7_live_test import (audit_stream, ctl_cmd, edmb_has_cell,  # noqa: E402
                              write_dense_world)

SERVER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "edenserver")
PORT = 27112
DENSE_RECORDS = 201 * 201 * 11   # write_dense_world's slab: one record per cell

fails = []
def check(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        fails.append(msg)


class Server:
    """A running ./edenserver with its stdout drained."""
    def __init__(self, world_dir, *extra, ctl=None):
        self.out = []
        self._lock = threading.Lock()
        self.p = subprocess.Popen(
            [SERVER, "--port", str(PORT),
             "--world", os.path.join(world_dir, "eden_world.model"),
             "--signs", os.path.join(world_dir, "eden_signs.txt"),
             "--connect-limit", "0",
             *(["--control-socket", ctl] if ctl else ["--no-control-socket"]),
             "--handshake-timeout", "0", "--idle-timeout-conn", "0", *extra],
            cwd=world_dir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        threading.Thread(target=self._drain, daemon=True).start()
        # Wait for the listener by watching the log, not by connecting: a probe
        # connection is itself a bare connect/close, which group 1 counts.
        deadline = time.time() + 20
        while time.time() < deadline:
            with self._lock:
                if any("REGION limits" in l for l in self.out):
                    break
            time.sleep(0.05)
        time.sleep(0.3)   # the listener binds just after the limits line

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

    def cpu_seconds(self):
        """User + system CPU the process has used, from ps (macOS and Linux)."""
        t = subprocess.run(["ps", "-o", "time=", "-p", str(self.p.pid)],
                           capture_output=True, text=True).stdout.strip()
        days = 0.0
        if "-" in t:
            d, t = t.split("-", 1)
            days = float(d)
        secs = 0.0
        for part in t.split(":"):
            secs = secs * 60 + float(part)
        return days * 86400 + secs


def fresh_world(dense=False):
    d = tempfile.mkdtemp(prefix="ewb12-")
    if dense:
        write_dense_world(os.path.join(d, "eden_world.model"))
    else:
        with open(os.path.join(d, "eden_world.model"), "w") as f:
            f.write("65500:33:65500:5:0\n")
    open(os.path.join(d, "eden_signs.txt"), "w").close()
    return d


def join(name, timeout=5.0):
    s = socket.create_connection(("127.0.0.1", PORT), timeout=timeout)
    s.sendall(("JOIN:%s:17:EDEN6:zr\n" % name).encode())
    s.settimeout(timeout)
    try:
        s.recv(4096)          # the welcome burst: JOIN has been processed
    except socket.timeout:
        pass
    return s


def saves(lines):
    return [l for l in lines if "Saved world (" in l]


class Reader:
    """Drains a socket on a thread: keeps every byte, and the arrival time of
    every POSVEL line (another player's movement)."""
    def __init__(self, sock):
        self.sock = sock
        self.buf = bytearray()
        self.moves = []
        self._partial = b""
        self.stop = threading.Event()
        self.sock.settimeout(0.5)
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while not self.stop.is_set():
            try:
                d = self.sock.recv(1 << 16)
            except socket.timeout:
                continue
            except OSError:
                return
            if not d:
                return
            self.buf.extend(d)
            now = time.time()
            lines = (self._partial + d).split(b"\n")
            self._partial = lines.pop()
            for ln in lines:
                if ln.startswith(b"POSVEL:"):
                    self.moves.append(now)


# --- group 1: bare connect/close no longer forces a save ---------------------

def group1_bare_closes():
    print("\n[1] 12.0a — 20 bare connect/close while someone builds")
    d = fresh_world()
    srv = Server(d)
    try:
        b = join("builder")
        m = srv.mark()
        t0 = time.time()
        i = 0
        while time.time() - t0 < 10.0:
            b.sendall(("ACTION:%d:34:65500:0:5\n" % (65400 + i)).encode())
            if i % 2 == 0 and i < 40:
                socket.create_connection(("127.0.0.1", PORT), timeout=2).close()
            i += 1
            time.sleep(0.25)
        lines = saves(srv.since(m, settle=0.5))
        print("       %d save(s) in 10 s: %s" % (len(lines), [l.rsplit("[", 1)[-1] for l in lines]))
        check(len(lines) <= 3, "20 bare closes during building cost <= 3 saves (was one each)")
        check(not any("[departure]" in l for l in lines), "a close that never joined requested no save")
        b.close()
        check(srv.alive(), "server alive")
    finally:
        srv.stop()
        shutil.rmtree(d, ignore_errors=True)


# --- group 2: join/build/leave churn is coalesced ----------------------------

def group2_churn():
    print("\n[2] 12.0a — 20 join -> build -> leave cycles")
    d = fresh_world()
    srv = Server(d)
    try:
        m = srv.mark()
        t0 = time.time()
        for i in range(20):
            c = join("churn%d" % i)
            c.sendall(("ACTION:%d:35:65500:0:5\n" % (65400 + i)).encode())
            time.sleep(0.05)
            c.close()
        dur = time.time() - t0
        mid = saves(srv.since(m, settle=0.2))
        deps = [l for l in mid if "[departure]" in l]
        allowed = int(dur // 5) + 2        # ceil(dur / 5) + 1
        print("       %d cycles in %.1f s -> %d save(s) (%d departure), allowed <= %d"
              % (20, dur, len(mid), len(deps), allowed + 1))
        check(len(deps) <= allowed, "departure saves coalesced to <= one per 5 s")
        check(len(mid) <= allowed + 1, "...plus at most one periodic autosave")
        # The last player's edit must still reach disk soon after they leave.
        after = saves(srv.since(m, settle=6.0))
        check(len(after) > len(mid), "a departure save ran within ~5 s of the last leave")
        with open(os.path.join(d, "eden_world.model"), "rb") as f:
            blob = f.read()
        check(edmb_has_cell(blob, 65400 + 19, 35, 65500, 5, 0), "...and holds the last player's edit")
    finally:
        srv.stop()
        shutil.rmtree(d, ignore_errors=True)


# --- group 3: kill -9 after a leave keeps the edit ----------------------------

def group3_kill_after_leave():
    print("\n[3] 12.0a — edit, leave, kill -9 6 s later: the edit is on disk")
    d = fresh_world()
    srv = Server(d)
    try:
        c = join("leaver")
        c.sendall(b"ACTION:65531:36:65531:0:9\n")
        time.sleep(0.3)
        c.close()
        time.sleep(6.0)
        srv.p.kill()                     # SIGKILL: no shutdown save
        srv.p.wait(timeout=5)
        with open(os.path.join(d, "eden_world.model"), "rb") as f:
            blob = f.read()
        check(edmb_has_cell(blob, 65531, 36, 65531, 9, 0),
              "the leaving player's edit survived a kill -9 six seconds later")
    finally:
        srv.stop()
        shutil.rmtree(d, ignore_errors=True)


# --- group 4: REGION flood under the encoder cap ------------------------------

def group4_flood():
    print("\n[4] 12.0b — four clients flood a dense box with --region-encoders 1")
    d = fresh_world(dense=True)
    ctl = os.path.join(d, "ctl.sock")
    # The cache off: with it, a flooder's repeats are hits and never reach an
    # encoder, which is 12.1b's point but not what this group measures.
    srv = Server(d, "--region-encoders", "1", "--region-cache-mb", "0", ctl=ctl)
    try:
        flooders = [join("flood%d" % i) for i in range(4)]
        readers = [Reader(s) for s in flooders]
        mover = join("mover")
        stop = threading.Event()

        def move():
            i = 0
            while not stop.is_set():
                try:
                    mover.sendall(("POSVEL:65500.0:33.92:%0.2f:0.1:0:0.1\n" % (65500 + i % 40)).encode())
                except OSError:
                    return
                i += 1
                time.sleep(0.05)                 # 20 Hz
        threading.Thread(target=move, daemon=True).start()

        def flood(s):
            while not stop.is_set():
                try:
                    s.sendall(b"REGION:65500:65500\n")
                except OSError:
                    return
                time.sleep(0.76)
        cpu0, w0 = srv.cpu_seconds(), time.time()
        for s in flooders:
            threading.Thread(target=flood, args=(s,), daemon=True).start()

        time.sleep(4.0)
        # A newcomer mid-flood: one REGION, and the whole burst must arrive.
        late = join("latecomer")
        lr = Reader(late)
        late.sendall(b"REGION:65500:65500\n")
        time.sleep(8.0)
        stop.set()
        cpu = (srv.cpu_seconds() - cpu0) / (time.time() - w0)

        # Let the late burst finish draining if it is still going.
        deadline = time.time() + 30
        while time.time() < deadline and audit_stream(bytes(lr.buf))["records"] < DENSE_RECORDS:
            time.sleep(0.5)

        stats = ctl_cmd(ctl, "region-stats")
        most = re.search(r"most in use at once (\d+)", stats)
        waits = re.search(r"encode waits\s*:\s*(\d+)", stats)
        print("       server CPU %.2f cores; region-stats: most in use %s, encode waits %s"
              % (cpu, most.group(1) if most else "?", waits.group(1) if waits else "?"))
        check(most is not None and int(most.group(1)) == 1, "never more than one frame deflating at once")
        check(cpu < 2.0, "server CPU stayed near the one-encoder cap (< 2 cores)")

        r0 = readers[0]
        window = [t for t in r0.moves if w0 + 1.0 <= t <= w0 + 12.0]
        gaps = [b - a for a, b in zip(window, window[1:])]
        print("       a flooder saw %d of ~220 movement lines in 11 s; worst gap %.2f s"
              % (len(window), max(gaps) if gaps else 99))
        check(len(window) >= 150, "a bystander's movement kept reaching a flooding client")
        check(gaps and max(gaps) < 1.5, "...with no stall over 1.5 s")

        bad = 0
        for r in readers:
            a = audit_stream(bytes(r.buf))
            bad += a["short"] + a["bad"] + a["orphan"]
        check(bad == 0, "every frame the flooders received decoded whole")
        la = audit_stream(bytes(lr.buf))
        print("       latecomer: %d frame(s), %d/%d records" % (la["frames"], la["records"], DENSE_RECORDS))
        check(la["records"] == DENSE_RECORDS and la["short"] == 0 and la["bad"] == 0,
              "a client joining mid-flood got its whole burst")

        for r in readers + [lr]:
            r.stop.set()
        for s in flooders + [mover, late]:
            s.close()
        check(srv.alive(), "server alive")
    finally:
        srv.stop()
        shutil.rmtree(d, ignore_errors=True)


# --- group 5: the record budget -----------------------------------------------

def group5_budget():
    print("\n[5] 12.0b — over the record budget a REGION is refused, and the client stays")
    d = fresh_world(dense=True)
    srv = Server(d, "--region-record-burst", "600000", "--region-record-rate", "100000")
    try:
        c = join("budget")
        r = Reader(c)
        m = srv.mark()
        c.sendall(b"REGION:65500:65500\n")       # 444 k records: fits the 600 k burst
        time.sleep(0.9)
        c.sendall(b"REGION:65500:65500\n")       # ~165 k left: refused
        time.sleep(0.9)
        c.sendall(b"REGION:65500:65500\n")       # still short: refused, not logged again
        log = srv.since(m, settle=2.0)
        queued = sum(1 for l in log if "frame(s) queued" in l)
        refusedLog = sum(1 for l in log if "over this client's record budget" in l)
        print("       3 requests: %d queued, %d budget-refusal log line(s)" % (queued, refusedLog))
        check(queued == 1, "only the request inside the budget was served")
        check(refusedLog == 1, "the refusal was logged once, not once per request")

        # Still connected: PING answers.
        n0 = len(r.buf)
        c.sendall(b"PING\n")
        time.sleep(0.5)
        check(b"PONG" in bytes(r.buf[n0:]), "the refused client is still connected (PING -> PONG)")

        # Refill (100 k/s: 444 k needs ~3 s on top of what was left) and ask again.
        time.sleep(3.5)
        m = srv.mark()
        c.sendall(b"REGION:65500:65500\n")
        log = srv.since(m, settle=1.0)
        check(any("frame(s) queued" in l for l in log), "served again once the budget refilled")
        r.stop.set()
        c.close()
    finally:
        srv.stop()
        shutil.rmtree(d, ignore_errors=True)


# --- helpers for 12.1 ------------------------------------------------------------

def snapz_lines(buf):
    """Every complete SNAPZ line a client received, as bytes."""
    return [l for l in bytes(buf).split(b"\n")[:-1] if l.startswith(b"SNAPZ:")]


def decode_records(lines):
    """The concatenated raw 20-byte records of some SNAPZ lines."""
    out = bytearray()
    for l in lines:
        b64 = l.split(b":", 2)[2]
        z = base64.b64decode(b64 + b"=" * (-len(b64) % 4))
        out += zlib.decompress(z, -15)
    return bytes(out)


def has_record(raw, x, y, z, flag, type_):
    import struct
    want = struct.pack("<5i", x, y, z, flag, type_)
    return any(raw[i:i + 20] == want for i in range(0, len(raw), 20))


def wait_records(reader, n, timeout=30.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if len(decode_records(snapz_lines(reader.buf))) >= n * 20:
            return True
        time.sleep(0.2)
    return False


# --- group 6: the encoded-region cache ------------------------------------------

def group6_cache():
    print("\n[6] 12.1b — the same box twice: cached, same bytes; an edit makes it fresh")
    d = fresh_world(dense=True)
    ctl = os.path.join(d, "ctl.sock")
    srv = Server(d, ctl=ctl)
    try:
        a = join("cacheA")
        ra = Reader(a)
        m = srv.mark()
        a.sendall(b"REGION:65500:65500\n")
        check(wait_records(ra, DENSE_RECORDS), "first request: the whole burst arrived")
        log = srv.since(m, settle=0.5)
        check(any("frame(s) queued" in l and "from cache" not in l for l in log), "...as a fresh scan")

        b = join("cacheB")
        rb = Reader(b)
        m = srv.mark()
        b.sendall(b"REGION:65500:65500\n")
        check(wait_records(rb, DENSE_RECORDS), "second client, same box: the whole burst arrived")
        log = srv.since(m, settle=0.5)
        check(any("from cache ->" in l for l in log), "...answered from the cache")
        check(snapz_lines(rb.buf) == snapz_lines(ra.buf), "...byte-identical to the fresh reply")

        # An edit inside the box, then B asks again: fresh, and it has the edit.
        a.sendall(b"ACTION:65500:50:65500:0:9\n")
        time.sleep(1.0)                          # also clears B's 750 ms request gap
        n0 = len(snapz_lines(rb.buf))
        m = srv.mark()
        b.sendall(b"REGION:65500:65500\n")
        check(wait_records(rb, 2 * DENSE_RECORDS + 1), "after an edit in the box: the whole burst again")
        log = srv.since(m, settle=0.5)
        check(any("frame(s) queued" in l and "from cache" not in l for l in log), "...as a fresh scan")
        second = decode_records(snapz_lines(rb.buf)[n0:])
        check(has_record(second, 65500, 50, 65500, 0, 9), "...carrying the edit")

        # And the re-filed reply serves the next asker.
        c = join("cacheC")
        rc = Reader(c)
        m = srv.mark()
        c.sendall(b"REGION:65500:65500\n")
        check(wait_records(rc, DENSE_RECORDS + 1), "a third client: the whole burst")
        log = srv.since(m, settle=0.5)
        check(any("from cache ->" in l for l in log), "...from the re-filed cache entry")
        check(snapz_lines(rc.buf) == snapz_lines(rb.buf)[n0:], "...the same bytes B got fresh")

        # Eight joiners at one spawn: one more encode at most, the rest hits.
        joiners = [join("spawn%d" % i) for i in range(8)]
        jr = [Reader(s) for s in joiners]
        m = srv.mark()
        for s in joiners:
            s.sendall(b"REGION:65500:65500\n")
        check(all(wait_records(r, DENSE_RECORDS + 1) for r in jr), "eight joiners each got the whole burst")
        log = srv.since(m, settle=0.5)
        hits = sum(1 for l in log if "from cache ->" in l)
        print("       8 joiners: %d from cache" % hits)
        check(hits == 8, "all eight answered from the cache (no encode)")
        stats = ctl_cmd(ctl, "region-stats")
        hm = re.search(r"cache hits\s*:\s*(\d+)", stats)
        print("       region-stats: cache hits %s" % (hm.group(1) if hm else "?"))
        check(hm is not None and int(hm.group(1)) >= 10, "region-stats counts the hits")

        for r in [ra, rb, rc] + jr:
            r.stop.set()
        for s in [a, b, c] + joiners:
            s.close()
        check(srv.alive(), "server alive")
    finally:
        srv.stop()
        shutil.rmtree(d, ignore_errors=True)


# --- group 7: deflate level ---------------------------------------------------------

def group7_level():
    print("\n[7] 12.1a — level 1 and level 6 decode to the same records")
    d = fresh_world(dense=True)
    got = {}
    try:
        for level in ("1", "6"):
            srv = Server(d, "--region-deflate-level", level)
            try:
                start = "\n".join(srv.since(0, settle=0))
                check(("deflate level %s" % level) in start, "startup line says level %s" % level)
                c = join("level%s" % level)
                r = Reader(c)
                c.sendall(b"REGION:65500:65500\n")
                check(wait_records(r, DENSE_RECORDS), "level %s: the whole burst" % level)
                lines = snapz_lines(r.buf)
                got[level] = (decode_records(lines), sum(len(l) + 1 for l in lines), len(lines))
                r.stop.set()
                c.close()
            finally:
                srv.stop()
        (r1, b1, f1), (r6, b6, f6) = got["1"], got["6"]
        print("       level 1: %d B in %d frames; level 6: %d B in %d frames" % (b1, f1, b6, f6))
        check(r1 == r6, "same records, same order")
        check(f1 == f6, "same frame split")
        check(b1 >= b6, "level 1 is the larger wire")
    finally:
        shutil.rmtree(d, ignore_errors=True)


def main():
    if not os.path.exists(SERVER):
        print("no %s — run ./build_server.sh first" % SERVER)
        return 1
    group1_bare_closes()
    group2_churn()
    group3_kill_after_leave()
    group4_flood()
    group5_budget()
    group6_cache()
    group7_level()
    print()
    if fails:
        print("phase12_live_test: %d check(s) FAILED" % len(fails))
        for f in fails:
            print("  - " + f)
        return 1
    print("phase12_live_test: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
