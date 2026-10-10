"""Soak: face animation running for N seconds while the serial console is watched.

The HTTP probes only prove the handlers answer; the render task failing shows up
as error lines on the console, not in the API. So reset, show a face, and keep
reading COM7 for the whole soak.

  python emote_soak.py [seconds] [face]
"""
import json
import sys
import time

import board_api
import capture_boot

SECONDS = int(sys.argv[1]) if len(sys.argv) > 1 else 60
FACE = sys.argv[2] if len(sys.argv) > 2 else "thinking"

capture_boot.reset_board()
boot = capture_boot.read(12)
print("waiting for Wi-Fi...")
time.sleep(4)
status, payload = board_api.call("GET", "/api/status")
d = json.loads(payload)
print("status:", status, "emote_ready", d["emote_ready"], "heap_int", d["heap_internal"])

print(board_api.call("POST", "/api/screen", json.dumps({"emote": FACE})))
soak = capture_boot.read(SECONDS)
status, payload = board_api.call("GET", "/api/status")
d = json.loads(payload)
print("after soak:", status, "uptime", d["uptime_s"], "heap_int", d["heap_internal"])

text = boot + soak
open("emote_soak.txt", "w", encoding="utf-8").write(text)
bad = [l for l in text.splitlines()
       if any(k in l for k in ("E (", "Guru", "abort", "error", "failed", "assert", "Backtrace"))]
print("log bytes:", len(text), "error lines:", len(bad))
for line in bad[:25]:
    print("  ", line.strip())
