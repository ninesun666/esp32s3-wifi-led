"""BLE GATT 回归测试：直连开发板跑完整控制链路（不依赖手机）。

用法（板子在广播中、未连接其他设备时）：

    pip install bleak
    python tools/ble_gatt_test.py

7 项检查：广播发现 → 连接 → 服务 UUID → 读状态 0003 →
写 LED 0002 并读回 → 非法写收应用错误 0x81 → 断开后广播恢复。
跑完灯带被交还自动状态机，不留手动接管。
"""
import asyncio, json, sys
from bleak import BleakScanner, BleakClient, BleakError

NAME = "ESP32-LED"  # CONFIG_EXAMPLE_BLE_NAME
SVC = "5d400001-7a9b-4c8e-b2f4-38a1c6e0d951"
CHR_LED = "5d400002-7a9b-4c8e-b2f4-38a1c6e0d951"
CHR_STATUS = "5d400003-7a9b-4c8e-b2f4-38a1c6e0d951"

RAINBOW = b'{"manual":true,"fx":"rainbow","rgb":[255,80,0],"brightness":60}'
GARBAGE = b'{"fx":"disco"}'
AUTO = b'{"manual":false}'
fails = []


def check(label, ok, detail=""):
    print(f"{'PASS' if ok else 'FAIL'}  {label}{('  ' + detail) if detail else ''}")
    if not ok:
        fails.append(label)


async def find(timeout=15):
    print(f"scanning up to {timeout}s for '{NAME}' ...")
    return await BleakScanner.find_device_by_name(NAME, timeout=timeout)


async def main():
    dev = await find()
    check("advertising found", dev is not None)
    if not dev:
        sys.exit(1)

    async with BleakClient(dev, timeout=15) as client:
        print(f"connected: {client.is_connected}, mtu={client.mtu_size}")
        check("services contain our 128-bit UUID",
              SVC.lower() in [str(s.uuid).lower() for s in client.services])

        raw = await client.read_gatt_char(CHR_STATUS)
        status = json.loads(raw.decode())
        print(f"status 0003: state={status.get('state')} ssid={status.get('ssid')} "
              f"rssi={status.get('rssi')} heap_internal={status.get('heap_internal')}")
        check("status 0003 parses as JSON with expected keys",
              {"state", "ssid", "rssi", "heap_internal"} <= set(status))

        before = json.loads((await client.read_gatt_char(CHR_LED)).decode())
        await client.write_gatt_char(CHR_LED, RAINBOW, response=True)
        after = json.loads((await client.read_gatt_char(CHR_LED)).decode())
        print(f"led 0002: before fx={before['fx']} manual={before['manual']}")
        print(f"led 0002: after  fx={after['fx']} rgb={after['rgb']} "
              f"brightness={after['brightness']} manual={after['manual']}")
        check("write 0002 rainbow applies",
              after["fx"] == "rainbow" and after["manual"] is True
              and after["rgb"] == [255, 80, 0] and after["brightness"] == 60)

        try:
            await client.write_gatt_char(CHR_LED, GARBAGE, response=True)
            check("garbage write rejected with 0x81", False, "no error raised")
        except Exception as e:
            msg = str(e)
            print(f"garbage write error: {msg}")
            check("garbage write rejected with 0x81",
                  "0x81" in msg or "129" in msg, msg)

        await client.write_gatt_char(CHR_LED, AUTO, response=True)
        back = json.loads((await client.read_gatt_char(CHR_LED)).decode())
        check("back to auto", back["manual"] is False)
    print("disconnected")

    await asyncio.sleep(3)  # board restarts advertising after the disconnect event
    again = await find(timeout=10)
    check("advertising resumed after disconnect", again is not None)

    print("\n== %d/%d passed ==" % (7 - len(fails), 7))
    sys.exit(1 if fails else 0)


asyncio.run(main())
