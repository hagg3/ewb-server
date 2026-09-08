#!/usr/bin/env python3
"""Throwaway Eden matchmaker stub — ROADMAP-SERVER stage 1.8 rung 4 scaffolding.

The shipping game has no "connect to IP" field; the only way onto a private
server is to appear as a row in its in-game Server Browser. This fakes Eden's
matchmaker so `edenserver` shows up there.

Wire (CAPTURE-FINDINGS.md "Matchmaker SERVER: grammar — CONFIRMED"): raw TCP,
client connects and sends `LISTP\\n`, server writes

    SERVER:<name>:<ip>:<port>:<locked>:<players>:<mode>\\n
    ... more rows ...
    END\\n

then closes. Non-persistent (one list per connection).

Usage — see ROADMAP-SERVER.md §1.8 "the /etc/hosts -> local list2.php stub path".
Typical (raw-TCP path, client dials the hardcoded matchmaker IP):

    sudo ifconfig lo0 alias 45.79.193.87
    python3 matchmaker_stub.py --listen 45.79.193.87:27020 --advertise 45.79.193.87:27015
    ./edenserver --port 27015 --name "Local Test"
    # ... test, then ...
    sudo ifconfig lo0 -alias 45.79.193.87

This is NOT the Phase 2.3 matchmaker (that one is C++, persistent-registration,
lives in the server tree). It exists only to get a real client onto the server
for the 1.8 acceptance rungs.
"""
import argparse
import socket
import threading


def handle(conn: socket.socket, body: bytes) -> None:
    conn.settimeout(3.0)
    try:
        try:
            conn.recv(256)  # the client sends `LISTP\n` (and maybe a version line); we don't care which
        except socket.timeout:
            pass
        conn.sendall(body)
    except OSError:
        pass
    finally:
        conn.close()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--listen", default="0.0.0.0:27020", metavar="HOST:PORT",
                    help="where to accept matchmaker connections (default 0.0.0.0:27020)")
    ap.add_argument("--advertise", default="127.0.0.1:27015", metavar="HOST:PORT",
                    help="the ewb-server the client should connect to (default 127.0.0.1:27015)")
    ap.add_argument("--name", default="Local Test", help="server name shown in the browser")
    ap.add_argument("--locked", type=int, default=0, help="1 if the server has --password")
    ap.add_argument("--players", type=int, default=0)
    ap.add_argument("--mode", type=int, default=0)
    ap.add_argument("--extra", action="append", default=[], metavar="SERVER:...",
                    help="an additional verbatim SERVER: row (repeatable)")
    a = ap.parse_args()

    lhost, lport = a.listen.rsplit(":", 1)
    ahost, aport = a.advertise.rsplit(":", 1)
    rows = [f"SERVER:{a.name}:{ahost}:{aport}:{a.locked}:{a.players}:{a.mode}"]
    rows += [e[7:] if e.startswith("SERVER:") else e for e in a.extra]
    body = ("".join(r + "\n" for r in rows) + "END\n").encode()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((lhost, int(lport)))
    srv.listen(16)
    print(f"matchmaker stub listening on {a.listen}; LISTP ->")
    for r in rows:
        print("  " + r)
    print("  END")
    try:
        while True:
            conn, addr = srv.accept()
            print(f"  [list] {addr[0]}:{addr[1]}")
            threading.Thread(target=handle, args=(conn, body), daemon=True).start()
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()


if __name__ == "__main__":
    main()
