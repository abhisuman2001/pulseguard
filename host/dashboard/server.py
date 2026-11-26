#!/usr/bin/env python3
"""
PulseGuard clinician dashboard bridge (Python 3 standard library only).

    python3 host/dashboard/server.py --target <qnx-ip>:7777 [--port 8080]

Connects to pg_telemetryd on the QNX target, relays frames to browsers via
Server-Sent Events, and forwards whitelisted commands back to the target.
Open http://localhost:8080
"""
import argparse
import json
import os
import queue
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
ALLOWED_CMDS = {"settings", "fault", "crash", "accept_ai", "rogue", "stress"}


class Bridge:
    def __init__(self, host, port):
        self.addr = (host, port)
        self.sock = None
        self.lock = threading.Lock()
        self.clients = set()
        self.clients_lock = threading.Lock()
        self.connected = False
        threading.Thread(target=self._run, daemon=True).start()

    def _broadcast(self, line):
        with self.clients_lock:
            for q in list(self.clients):
                try:
                    q.put_nowait(line)
                except queue.Full:
                    pass  # slow browser: drop frame, never block the bridge

    def _status(self, ok, msg=""):
        self.connected = ok
        self._broadcast(json.dumps({"link": ok, "msg": msg}))

    def _run(self):
        while True:
            try:
                s = socket.create_connection(self.addr, timeout=3)
                s.settimeout(5)
                with self.lock:
                    self.sock = s
                self._status(True, "connected to %s:%d" % self.addr)
                buf = b""
                while True:
                    chunk = s.recv(65536)
                    if not chunk:
                        raise ConnectionError("target closed connection")
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        if line:
                            self._broadcast(line.decode("utf-8", "replace"))
            except Exception as e:  # noqa: BLE001 - keep retrying forever
                with self.lock:
                    self.sock = None
                self._status(False, "target %s:%d unreachable (%s), retrying" % (*self.addr, e))
                time.sleep(1.0)

    def send(self, obj):
        if obj.get("cmd") not in ALLOWED_CMDS:
            return False
        data = (json.dumps(obj, separators=(",", ":")) + "\n").encode()
        with self.lock:
            if not self.sock:
                return False
            try:
                self.sock.sendall(data)
                return True
            except OSError:
                return False

    def subscribe(self):
        q = queue.Queue(maxsize=200)
        with self.clients_lock:
            self.clients.add(q)
        return q

    def unsubscribe(self, q):
        with self.clients_lock:
            self.clients.discard(q)


def make_handler(bridge):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            if self.path in ("/", "/index.html"):
                with open(os.path.join(HERE, "index.html"), "rb") as f:
                    body = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            elif self.path == "/events":
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.end_headers()
                q = bridge.subscribe()
                try:
                    self.wfile.write(("data: %s\n\n" % json.dumps(
                        {"link": bridge.connected})).encode())
                    while True:
                        try:
                            line = q.get(timeout=10)
                            self.wfile.write(("data: %s\n\n" % line).encode())
                        except queue.Empty:
                            self.wfile.write(b": keepalive\n\n")
                        self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    pass
                finally:
                    bridge.unsubscribe(q)
            else:
                self.send_error(404)

        def do_POST(self):
            if self.path != "/cmd":
                self.send_error(404)
                return
            n = int(self.headers.get("Content-Length", "0"))
            if n > 4096:
                self.send_error(413)
                return
            try:
                obj = json.loads(self.rfile.read(n))
            except ValueError:
                self.send_error(400)
                return
            ok = isinstance(obj, dict) and bridge.send(obj)
            self.send_response(200 if ok else 503)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"ok": ok}).encode())

    return H


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target", default="127.0.0.1:7777", help="QNX target host:port")
    ap.add_argument("--port", type=int, default=8080, help="local HTTP port")
    ap.add_argument("--bind", default="127.0.0.1", help="local bind address")
    a = ap.parse_args()
    host, _, port = a.target.rpartition(":")
    bridge = Bridge(host or a.target, int(port or 7777))
    srv = ThreadingHTTPServer((a.bind, a.port), make_handler(bridge))
    print("PulseGuard dashboard: http://%s:%d  (target %s)" % (a.bind, a.port, a.target))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
