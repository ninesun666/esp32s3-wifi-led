#!/usr/bin/env python3
"""Push a firmware image to the ESP32-S3 over BLE GATT (needs `pip install bleak`).

Flow: connect -> OTA control 0005 {"cmd":"begin","size","sha256"} -> raw chunks
to OTA data 0006 -> {"cmd":"end"} -> board reboots into the new image.

Usage:
  python tools/ble_ota_send.py ESP32-LED wifi_led.bin
  python tools/ble_ota_send.py <MAC-or-name> image.bin --fast
    --fast   write-without-response + tiny pause; the default uses
             write-with-response, which is slower but self-throttling.

Name matching is case-insensitive; a 17:xx:xx style string is used as a MAC
directly (Windows bleak needs the MAC; macOS needs the name UUID).
"""

import argparse
import asyncio
import hashlib
import sys

from bleak import BleakClient, BleakScanner

SVC = "5d400001-7a9b-4c8e-b2f4-38a1c6e0d951"
CH_OTA_CTL = "5d400005-7a9b-4c8e-b2f4-38a1c6e0d951"
CH_OTA_DATA = "5d400006-7a9b-4c8e-b2f4-38a1c6e0d951"
CH_STATUS = "5d400003-7a9b-4c8e-b2f4-38a1c6e0d951"

# MTU 512 on this board -> 509 payload bytes per write.
CHUNK = 480


async def find_target(name_or_mac: str):
    looks_like_mac = len(name_or_mac) == 17 and name_or_mac.count(":") == 5
    if looks_like_mac:
        return name_or_mac
    print(f"scanning for \"{name_or_mac}\" ...")
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: d.name and name_or_mac.lower() in d.name.lower(),
        timeout=15,
    )
    if not dev:
        sys.exit(f"device \"{name_or_mac}\" not found")
    return dev.address if sys.platform != "darwin" else dev


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("target", help="device name (substring ok) or MAC")
    ap.add_argument("image", help="firmware .bin to send")
    ap.add_argument("--fast", action="store_true",
                    help="write-without-response + throttle")
    args = ap.parse_args()

    with open(args.image, "rb") as f:
        image = f.read()
    total = len(image)
    sha = hashlib.sha256(image).hexdigest()
    print(f"image: {total} bytes, sha256 {sha[:16]}...")

    address = await find_target(args.target)
    async with BleakClient(address) as client:
        ok = client.services.get_service(SVC) is not None
        if not ok:
            sys.exit("custom service not on this device -- old firmware?")

        # begin
        import json
        cmd = json.dumps({"cmd": "begin", "size": total, "sha256": sha})
        await client.write_gatt_char(CH_OTA_CTL, cmd.encode(), response=True)
        print("begin accepted")

        # data
        mode_no_rsp = args.fast and any(
            "write-without-response" in c.properties
            for c in client.services.characteristics
            if c.uuid == CH_OTA_DATA
        )
        sent = 0
        for off in range(0, total, CHUNK):
            chunk = image[off:off + CHUNK]
            await client.write_gatt_char(CH_OTA_DATA, chunk, response=not mode_no_rsp)
            sent += len(chunk)
            if (sent // CHUNK) % 100 == 0:
                print(f"  {sent}/{total} ({sent * 100 // total}%)")
            if mode_no_rsp:
                await asyncio.sleep(0.005)
        print(f"data done: {sent}/{total}")

        # end
        try:
            await client.write_gatt_char(CH_OTA_CTL,
                                         json.dumps({"cmd": "end"}).encode(),
                                         response=True)
        except Exception as e:
            sys.exit(f"end rejected (sha mismatch or flash error?): {e}")
        print("end accepted -- board reboots into the new image in ~1 s")

    # reconnect and confirm
    await asyncio.sleep(6)
    address = await find_target(args.target)
    async with BleakClient(address) as client:
        status = await client.read_gatt_char(CH_STATUS)
        print(f"reconnected, status read ok ({len(status)} bytes): new image is alive")


if __name__ == "__main__":
    asyncio.run(main())
