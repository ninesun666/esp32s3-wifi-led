"""BLE GATT 回归测试：直连开发板跑完整控制链路（不依赖手机）。

用法（板子在广播中、未连接其他设备时；macOS 没有裸 `python` 命令，
bleak 也不让装进系统 Python，所以走仓库内 venv，一次性建好）：

    python3 -m venv tools/.venv
    tools/.venv/bin/pip install bleak
    tools/.venv/bin/python tools/ble_gatt_test.py

Windows 下把 bin/ 换成 Scripts\\（`tools\\.venv\\Scripts\\python tools\\ble_gatt_test.py`）。

11 项检查：广播发现 → 连接 → 服务 UUID → 读状态 0003 →
写 LED 0002 并读回 → 非法写收应用错误 0x81 → 配网特征值 0008 可读且不含密码 →
过短密码被拒（不改动板子现有网络）→ 扫描落到 done/error → 恢复自动 → 断开后广播恢复。
跑完灯带被交还自动状态机，不留手动接管。扫描会让板子短暂离开信道，属预期。
"""
import asyncio, json, sys
from bleak import BleakScanner, BleakClient, BleakError

NAME = "ESP32-LED"  # CONFIG_EXAMPLE_BLE_NAME
SVC = "5d400001-7a9b-4c8e-b2f4-38a1c6e0d951"
CHR_LED = "5d400002-7a9b-4c8e-b2f4-38a1c6e0d951"
CHR_STATUS = "5d400003-7a9b-4c8e-b2f4-38a1c6e0d951"
CHR_WIFI = "5d400008-7a9b-4c8e-b2f4-38a1c6e0d951"

RAINBOW = b'{"manual":true,"fx":"rainbow","rgb":[255,80,0],"brightness":60}'
GARBAGE = b'{"fx":"disco"}'
AUTO = b'{"manual":false}'
# 校验发生在写进 NVS 之前，所以这条一定改不动板子上真实的网络
BAD_SET = b'{"cmd":"set","ssid":"ble-test","password":"short"}'
SCAN = b'{"cmd":"scan"}'
fails = []
total = 0


def check(label, ok, detail=""):
    global total
    total += 1
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

        # ---- 配网特征值（0008）：只读 + 一条非法写 + 一次扫描，绝不覆盖板子上真实的网络
        try:
            raw = await client.read_gatt_char(CHR_WIFI)
            cfg = json.loads(raw.decode())
            print(f"wifi 0008: source={cfg.get('source')} ssid={cfg.get('ssid')} "
                  f"state={cfg.get('state')} scan={cfg.get('scan')} "
                  f"has_password={cfg.get('has_password')}")
            check("wifi 0008 parses with provisioning fields",
                  {"proto", "source", "ssid", "state", "scan"} <= set(cfg))
            check("wifi 0008 never returns the passphrase",
                  "password" not in raw.decode())

            try:
                await client.write_gatt_char(CHR_WIFI, BAD_SET, response=True)
                check("over-short passphrase rejected with 0x81", False, "no error raised")
            except Exception as e:
                msg = str(e)
                print(f"bad wifi set error: {msg}")
                check("over-short passphrase rejected with 0x81",
                      "0x81" in msg or "129" in msg, msg)

            await client.write_gatt_char(CHR_WIFI, SCAN, response=True)
            nets = {}
            for _ in range(12):
                await asyncio.sleep(2)
                cfg = json.loads((await client.read_gatt_char(CHR_WIFI)).decode())
                if cfg.get("scan") in ("done", "error", "idle"):
                    nets = cfg
                    break
            print(f"scan settled as: {nets.get('scan')} "
                  f"networks={len(nets.get('networks', []))}")
            check("wifi 0008 scan reaches done/error",
                  nets.get("scan") in ("done", "error"), str(nets.get("scan")))
        except Exception as e:
            check("wifi 0008 present", False,
                  f"old firmware without provisioning support? {e}")
    print("disconnected")

    await asyncio.sleep(3)  # board restarts advertising after the disconnect event
    again = await find(timeout=10)
    check("advertising resumed after disconnect", again is not None)

    print("\n== %d/%d passed ==" % (total - len(fails), total))
    sys.exit(1 if fails else 0)


asyncio.run(main())
