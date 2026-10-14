"""Read-only gateway in front of the ESP32 monitor page for public tunneling.

The board's HTTP API has no authentication, so a tunnel straight to it would let
anyone reach POST /api/ota (overwrite firmware) and POST /api/wifi (repoint the
board at another access point). This proxy lets the page load and report live
telemetry, and refuses everything else.

Usage: python monitor_proxy.py [--listen 127.0.0.1:8788] [--board 192.168.1.6] [--allow-write]
"""

import argparse
import http.client
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

UP_HEADER = re.compile(rb"^(Connection|Keep-Alive|Transfer-Encoding|Content-Length|Proxy-)\b", re.I)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    board = "192.168.1.6"
    allow_write = False
    paths = {"/", "/index.html", "/favicon.ico", "/api/status", "/api/led"}

    def _gateway(self, method):
        path = self.path.split("?")[0]
        if path not in self.paths:
            self.send_error(404, "not found")
            return
        if method not in ("GET", "HEAD") and not self.allow_write:
            self.send_error(403, "tunnel is read-only")
            return
        body = b""
        if method != "GET" and method != "HEAD" and self.headers.get("Content-Length"):
            body = self.rfile.read(int(self.headers["Content-Length"]))
        try:
            conn = http.client.HTTPConnection(self.board, 80, timeout=10)
            conn.request(method, path, body=body, headers={"Host": self.board})
            resp = conn.getresponse()
            payload = resp.read()
            self.send_response(resp.status)
            for name, value in resp.getheaders():
                if not UP_HEADER.match((name + ":").encode()):
                    self.send_header(name, value)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            if method != "HEAD":
                self.wfile.write(payload)
            conn.close()
        except OSError as exc:
            self.send_error(502, "board unreachable: %s" % exc)

    def do_GET(self):
        self._gateway("GET")

    def do_HEAD(self):
        self._gateway("HEAD")

    def do_POST(self):
        self._gateway("POST")

    def do_PUT(self):
        self._gateway("PUT")

    def do_DELETE(self):
        self._gateway("DELETE")

    def log_message(self, fmt, *args):
        sys.stdout.write("%s %s\n" % (self.log_date_time_string(), fmt % args))
        sys.stdout.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", default="127.0.0.1:8788")
    ap.add_argument("--board", default="192.168.1.6")
    ap.add_argument("--allow-write", action="store_true",
                    help="forward the board's write endpoints too (public OTA/provisioning risk)")
    a = ap.parse_args()
    host, port = a.listen.rsplit(":", 1)
    Handler.board = a.board
    Handler.allow_write = a.allow_write
    ThreadingHTTPServer((host, int(port)), Handler).serve_forever()


if __name__ == "__main__":
    main()
