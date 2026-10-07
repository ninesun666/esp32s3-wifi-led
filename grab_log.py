import time, io, sys
import serial

SECONDS = int(sys.argv[1]) if len(sys.argv) > 1 else 70
RESET = len(sys.argv) > 2 and sys.argv[2] == "reset"

s = serial.Serial("COM7", 115200, timeout=1)
if RESET:
    # esptool's post-flash reset: DTR low leaves IO0 high (normal boot), an RTS
    # pulse on EN restarts the chip. Opening the port without this leaves the
    # harness's default DTR asserted, which is what used to land in the bootloader.
    s.dtr = False
    s.rts = False
    time.sleep(0.1)
    s.rts = True
    time.sleep(0.1)
    s.rts = False

out = io.StringIO()
end = time.time() + SECONDS
while time.time() < end:
    data = s.read(4096)
    if data:
        out.write(data.decode("utf-8", "replace"))
s.close()

text = out.getvalue()
print("bytes: %d" % len(text))
KEYS = ("wifi_led", "reason", "scanning", "auth=", "connecting",
        "got IP", "probe", "WS2812", "psram", "heap", "LED", "mac", "alive")
keep = [ln for ln in text.splitlines() if any(k in ln for k in KEYS)]
print("\n".join(keep) if keep else "(no match; raw follows)\n" + text[-3000:])
