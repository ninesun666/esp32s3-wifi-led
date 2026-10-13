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
  python board_api.py wifi                         # 当前网络配置 + 最近一次扫描结果
  python board_api.py wifi scan                    # 让板子扫描周围网络（再跑一次 wifi 看结果）
  python board_api.py wifi set "MyRouter" "pass"   # 换网络：存进 NVS 并立刻重连
  python board_api.py wifi forget                  # 清掉保存的网络，回到编译期配置
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
    elif what == "wifi":
        # 配网验证走 PC 一侧的通道，协议与手机 App 的 BLE 特征值完全相同
        # （docs/wifi_provisioning.md）。密码出现在命令行里会进 shell 历史，
        # 所以这条路只用来做一次性验证，日常配网用 App。
        if len(args) == 1 or args[1] == "get":
            status, body = call("GET", "/api/wifi")
            print(status)
            print(json.dumps(json.loads(body), ensure_ascii=False, indent=2))
        elif args[1] == "scan":
            print(call("POST", "/api/wifi", json.dumps({"cmd": "scan"})))
        elif args[1] == "forget":
            status, body = call("POST", "/api/wifi", json.dumps({"cmd": "forget"}))
            print(status)
            print(body)
        elif args[1] == "set":
            ssid = args[2]
            pwd = args[3] if len(args) > 3 else ""
            status, body = call("POST", "/api/wifi",
                                json.dumps({"cmd": "set", "ssid": ssid, "password": pwd}))
            print(status)
            print(body)
        else:
            print(__doc__)
            sys.exit(2)
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == "__main__":
    main()
