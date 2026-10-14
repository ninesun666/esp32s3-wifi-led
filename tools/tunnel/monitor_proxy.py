"""Gateway in front of the ESP32 monitor page, for phone remote control.

The board's HTTP API has no authentication at all, so a public URL pointing
straight at it hands anyone POST /api/ota (overwrite firmware) and POST
/api/wifi (repoint the board at another access point). This proxy puts a key
gate in front of the whole board and, in readonly mode, refuses every write.

Modes:
  readonly  GET of the board page and /api/status,/api/led only
  control   the board's own write endpoints too (LED, screen, emotes, OTA)

Both modes require the unlock key (see /unlock) before the board is visible,
because GET /api/status alone leaks SSID, MAC and the board's LAN address.

Usage: python monitor_proxy.py [--listen 127.0.0.1:8788] [--board 192.168.1.6]
                               [--mode readonly|control] [--key-file PATH]
"""

import argparse
import hmac
import http.client
import re
import secrets
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs

UP_HEADER = re.compile(rb"^(Connection|Keep-Alive|Transfer-Encoding|Content-Length|Proxy-|Set-Cookie)\b", re.I)
COOKIE = "esp32_ctl"
MAX_BODY = 8 * 1024 * 1024  # an OTA .bin is ~1.5 MB and arrives raw
HERE = Path(__file__).resolve().parent
UNLOCK_PAGE = """<!doctype html><html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>解锁</title><style>
body{font:16px/1.5 system-ui,"Microsoft YaHei",sans-serif;background:#0e1116;color:#e6edf3;
display:flex;min-height:100vh;align-items:center;justify-content:center;margin:0}
form{background:#161b22;border:1px solid #232b36;border-radius:12px;padding:22px;width:min(92vw,340px)}
input{width:100%;box-sizing:border-box;padding:12px;font:inherit;margin:10px 0;border-radius:8px;
border:1px solid #232b36;background:#0e1116;color:#e6edf3}
button{width:100%;padding:12px;font:inherit;border:0;border-radius:8px;background:#1f6feb;color:#fff}
p.err{color:#f85149;font-size:13px;margin:0}
</style></head><body><form method="post" action="/unlock">
<div>板子遥控解锁</div><input name="key" type="password" autocomplete="off" autofocus placeholder="密钥">
<button type="submit">解锁</button><!--ERR--></form></body></html>"""
REMOTE_PAGE = (HERE / "remote.html").read_bytes() if (HERE / "remote.html").exists() else b""


def load_key(path, create=True):
    try:
        value = Path(path).read_text(encoding="ascii").strip()
        if value:
            return value.encode()
    except FileNotFoundError:
        if not create:
            raise
    key = secrets.token_hex(16).encode()
    Path(path).write_bytes(key + b"\n")
    return key


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    board = "192.168.1.6"
    mode = "readonly"
    key = b""
    paths = {"/", "/api/status", "/api/led", "/api/wifi", "/api/screen", "/api/ota"}
    writes_ok = {"/api/led", "/api/wifi", "/api/screen", "/api/ota"}

    # -- helpers ---------------------------------------------------------
    def unlocked(self):
        for part in (self.headers.get("Cookie") or "").split(";"):
            name, _, value = part.strip().partition("=")
            if name == COOKIE:
                return hmac.compare_digest(value.encode(), self.key)
        return False

    def send_bytes(self, code, body, content_type="text/html; charset=utf-8", extra=()):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        for name, value in extra:
            self.send_header(name, value)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def deny(self, code, message):
        if code == 401:
            self.send_bytes(401, UNLOCK_PAGE.encode())
        else:
            self.send_bytes(code, ('{"ok":false,"error":"%s"}' % message).encode(),
                            "application/json")

    # -- board passthrough ------------------------------------------------
    def forward(self, method, path, body):
        try:
            conn = http.client.HTTPConnection(self.board, 80, timeout=15)
            conn.request(method, path, body=body, headers={"Host": self.board,
                                                           "Connection": "close"})
            resp = conn.getresponse()
            payload = resp.read()
            conn.close()
        except OSError as exc:
            self.send_bytes(502, ('{"ok":false,"error":"board unreachable: %s"}' % exc).encode(),
                            "application/json")
            return
        self.send_response(resp.status)
        for name, value in resp.getheaders():
            if not UP_HEADER.match((name + ":").encode()):
                self.send_header(name, value)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        if method != "HEAD":
            self.wfile.write(payload)

    def handle_verb(self, method):
        path = self.path.split("?")[0]
        # Drain the request body before deciding anything: replying while the
        # body is still in the socket desynchronises cloudflared's keep-alive
        # connection, and the next request on it comes back as 501.
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(min(length, MAX_BODY)) if length else b""
        if length > MAX_BODY:
            self.close_connection = True
            self.deny(413, "body too large")
            return
        if path in ("/remote", "/remote.html"):
            self.send_bytes(200, REMOTE_PAGE)
            return
        if path == "/favicon.ico":
            self.send_bytes(204, b"")
            return
        if path.startswith("/unlock"):
            self.unlock(method, body)
            return
        if not self.unlocked():
            self.deny(401, "unlock required")
            return
        if path not in self.paths:
            self.deny(404, "not found")
            return
        if method in ("GET", "HEAD"):
            self.forward(method, path, b"")
            return
        if self.mode != "control":
            self.deny(403, "tunnel is read-only")
            return
        if path not in self.writes_ok:
            self.deny(405, "method not allowed here")
            return
        self.forward(method, path, body)

    def unlock(self, method, body):
        if method != "POST":
            self.send_bytes(200, UNLOCK_PAGE.encode())
            return
        value = (parse_qs(body.decode("utf-8", "replace"), keep_blank_values=True)
                 .get("key") or [""])[0].encode()
        if not hmac.compare_digest(value, self.key):
            self.send_bytes(401, UNLOCK_PAGE.replace('<!--ERR-->', '<p class="err">密钥不对</p>').encode())
            return
        self.send_bytes(302, b"", extra=[
            ("Location", "/remote"),
            ("Set-Cookie", "%s=%s; Path=/; Max-Age=2592000; HttpOnly; SameSite=Lax; Secure"
             % (COOKIE, self.key.decode()))])

    do_GET = lambda self: self.handle_verb("GET")
    do_HEAD = lambda self: self.handle_verb("HEAD")
    do_POST = lambda self: self.handle_verb("POST")
    do_PUT = lambda self: self.handle_verb("PUT")
    do_DELETE = lambda self: self.handle_verb("DELETE")

    def log_message(self, fmt, *args):
        sys.stdout.write("%s %s\n" % (self.log_date_time_string(), fmt % args))
        sys.stdout.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", default="127.0.0.1:8788")
    ap.add_argument("--board", default="192.168.1.6")
    ap.add_argument("--mode", choices=("readonly", "control"), default="readonly")
    ap.add_argument("--key-file", default=str(HERE / "control.key"))
    a = ap.parse_args()
    host, port = a.listen.rsplit(":", 1)
    Handler.board = a.board
    Handler.mode = a.mode
    Handler.key = load_key(a.key_file)
    ThreadingHTTPServer((host, int(port)), Handler).serve_forever()


if __name__ == "__main__":
    main()
