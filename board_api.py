"""Talk to the board's HTTP API without waiting for a connection close.

esp_http_server sends Content-Length and then just leaves the socket open, so the
naive "recv until EOF" probe hangs and looks like the board is dead. This reads
exactly the announced number of bytes.

  python board_api.py status
  python board_api.py screen 255 0 0
  python board_api.py screen bars
  python board_api.py screen emote happy
  python board_api.py led                          # read the current strip setting
  python board_api.py led 230 160 20               # take over: solid rgb(230,160,20)
  python board_api.py led rainbow 255 80 0 60      # take over: fx colour brightness
  python board_api.py led auto                     # hand the strip back to link state
"""
import json
import socket
import sys

HOST = "192.168.1.6"
PORT = 80


def call(method, path, body=None):
    s = socket.create_connection((HOST, PORT), timeout=6)
    head = "%s %s HTTP/1.1\r\nHost: %s\r\n" % (method, path, HOST)
    if body is not None:
        head += "Content-Type: application/json\r\nContent-Length: %d\r\n" % len(body)
    s.sendall((head + "\r\n" + (body or "")).encode())

    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += s.recv(4096)
    headers, payload = buf.split(b"\r\n\r\n", 1)
    status = headers.split(b"\r\n")[0].decode()
    length = 0
    for line in headers.decode(errors="replace").split("\r\n")[1:]:
        if line.lower().startswith("content-length:"):
            length = int(line.split(":")[1])
    while len(payload) < length:
        payload += s.recv(4096)
    s.close()
    return status, payload.decode(errors="replace")


def main():
    args = sys.argv[1:]
    what = args[0] if args else "status"
    if what == "status":
        status, body = call("GET", "/api/status")
        print(status)
        print(json.dumps(json.loads(body), ensure_ascii=False, indent=2))
    elif what == "led":
        fx_names = ("solid", "breathe", "rainbow", "off")
        if len(args) == 1:
            status, body = call("GET", "/api/led")
            print(status)
            print(json.dumps(json.loads(body), ensure_ascii=False, indent=2))
        elif args[1] == "auto":
            print(call("POST", "/api/led", '{"manual":false}'))
        elif args[1] in fx_names:
            r, g, b = (int(x) for x in args[2:5])
            bri = int(args[5]) if len(args) > 5 else 50
            payload = {"manual": True, "fx": args[1], "rgb": [r, g, b], "brightness": bri}
            print(call("POST", "/api/led", json.dumps(payload)))
        else:
            r, g, b = (int(x) for x in args[1:4])
            bri = int(args[4]) if len(args) > 4 else 50
            payload = {"manual": True, "fx": "solid", "rgb": [r, g, b], "brightness": bri}
            print(call("POST", "/api/led", json.dumps(payload)))
    elif what == "screen":
        if args[1:2] == ["bars"]:
            print(call("POST", "/api/screen", '{"pattern":"bars"}'))
        elif args[1:2] == ["emote"]:
            print(call("POST", "/api/screen", json.dumps({"emote": args[2]})))
        else:
            r, g, b = (int(x) for x in args[1:4])
            print(call("POST", "/api/screen", json.dumps({"r": r, "g": g, "b": b})))
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == "__main__":
    main()
