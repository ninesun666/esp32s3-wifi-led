"""Capture a fresh boot log over COM7.

Usage: python capture_boot.py [seconds] [outfile]

The reset trick on this board is the *close*, not the line edges: open the port
with DTR and RTS asserted (that holds EN low, chip frozen and silent), close it
(EN released -> the chip boots), reopen quietly and read. A bare "pulse RTS"
toggle does nothing here -- the tell is the timestamps, which keep counting up
instead of restarting near zero, so a capture that looks short actually caught
the already-running app.
"""
import sys
import time

import serial

SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 18
OUTFILE = sys.argv[2] if len(sys.argv) > 2 else "emote_boot.txt"
PORT = "COM7"
BAUD = 115200


def reset_board():
    s = serial.Serial()
    s.port = PORT
    s.baudrate = BAUD
    s.dtr = True
    s.rts = True        # asserted at open: EN held low, chip frozen and silent
    s.open()
    time.sleep(0.6)
    s.close()          # EN released here: the boot starts now


def read(seconds):
    s = serial.Serial()
    s.port = PORT
    s.baudrate = BAUD
    s.dtr = False
    s.rts = False
    s.open()
    chunks = []
    deadline = time.time() + seconds
    while time.time() < deadline:
        n = s.in_waiting
        if n:
            chunks.append(s.read(n))
        else:
            time.sleep(0.05)
    s.close()
    return b"".join(chunks).decode("utf-8", "replace")


if __name__ == "__main__":
    reset_board()
    text = read(SECONDS)
    open(OUTFILE, "w", encoding="utf-8").write(text)
    lines = text.splitlines()
    first = lines[0] if lines else "(nothing)"
    print("captured %d bytes, %d lines, first: %s" % (len(text), len(lines), first[:60]))
    if "ESP-ROM" not in text:
        print("WARNING: no ROM banner -- the board did not actually restart")
