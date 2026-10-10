"""Squeeze the screen API: no pauses between paints, liveness probe throughout.

The first cut of the emote integration wedge-d the httpd task when a manual
bars repaint was followed by a face request, because the panel transfer-done
callback is shared by both. This is the regression test for that.

  python emote_stress.py [rounds]
"""
import json
import random
import sys
import time

import board_api

FACES = ["happy", "laughing", "funny", "loving", "embarrassed", "confident",
         "delicious", "sad", "crying", "sleepy", "silly", "angry", "surprised",
         "shocked", "thinking", "winking", "relaxed", "confused", "neutral", "idle"]

rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 40
random.seed(7)
fails = []

for i in range(rounds):
    body = random.choice([
        {"emote": random.choice(FACES)},
        {"emote": random.choice(FACES)},
        {"pattern": "bars"},
        {"r": random.randrange(256), "g": random.randrange(256), "b": random.randrange(256)},
    ])
    try:
        status, payload = board_api.call("POST", "/api/screen", json.dumps(body))
        ok = json.loads(payload).get("ok")
        if not ok:
            fails.append((i, body, status, payload))
    except Exception as exc:
        print("round %d %s DEAD: %s" % (i, body, exc))
        sys.exit(1)
    if i % 5 == 4:
        try:
            _, st = board_api.call("GET", "/api/status")
            d = json.loads(st)
            print("round %d alive uptime=%d heap_int=%d emote=%s" %
                  (i, d["uptime_s"], d["heap_internal"], d["emote_ready"]))
        except Exception as exc:
            print("round %d status DEAD: %s" % (i, exc))
            sys.exit(1)

print("done: %d rounds, %d unexpected failures" % (rounds, len(fails)))
for f in fails:
    print("  ", f)
