# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 这是什么

个人 ESP32-S3 练手沙箱。目前唯一跑通的项目是 `wifi_led/`——Wi-Fi station + WS2812 状态灯 + 板载 HTTP 监控页（外接 ST7789 屏 + 表情动画 + BLE 控制）——后续新项目拿它当底座。固件按 ESP-IDF 组件工程组织：`app_main.c` 只做接线，逻辑在 `components/`（link_state / status_light / display_tft / i2c_probe / monitor_server / ble_gatt）。两份 README（中文）是硬件事实和实测结论的权威来源；事实变化时要同步更新它们。

## 选型原则

用户明确要求：开发中优先使用开源方案和官方 SDK。落到这个仓库：功能先看 ESP-IDF 自带能力，再找 Espressif 官方组件（components.espressif.com 的 managed component），然后是成熟开源库；不重复造轮子，不用闭源替代品。新组件通过 `main/idf_component.yml` 引入（先例：`espressif/led_strip`）。

## 硬件事实（ESP32-S3-WROOM-1-N16R8）

- 16 MiB flash + 8 MiB **八线（Octal）** PSRAM：`sdkconfig.defaults` 里的 `CONFIG_SPIRAM_MODE_OCT=y` 是硬性要求。按四线配能开机但 PSRAM 初始化失败。
- 八线 PSRAM 占用 GPIO 26–37。可用引脚：0–21 和 38–48，另避开 19/20（原生 USB）、43/44（UART）、0/3（strapping）。
- 板载 WS2812 在 GPIO 48（物理确认）。串口桥 CH343，Windows 上是 COM7。
- **别用 ping 判断板子可达**——lwIP 默认不回 ICMP，直接 `curl http://<ip>/`。

## 编译 / 烧录 / 监视

ESP-IDF **v5.3.5**。Windows + Git Bash 下 `idf.py` 必须走包装脚本（直接跑会被拒：`MSys/Mingw is not supported`）。在仓库根目录执行：

```bash
cmd //c "powershell -NoProfile -ExecutionPolicy Bypass -File idf_run.ps1 -Dir wifi_led build"
cmd //c "powershell -NoProfile -ExecutionPolicy Bypass -File idf_run.ps1 -Dir wifi_led -p COM7 flash"
```

包装脚本参数叫 `-Dir` 而不是 `-ProjectDir`，因为 `idf.py` 自己的 `-p` 会前缀匹配到 `-ProjectDir`。

macOS/Linux 下在 `wifi_led/` 里直接 `idf.py set-target esp32s3`、`idf.py build`、`idf.py -p <端口> flash monitor`。这台 Mac（`~/esp/esp-idf`）要先 `export IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.3_py3.12_env` 再 source `export.sh`——系统 `python3` 是 3.13 而 IDF 装的是 py3.12 环境，不设的话 export 报 python doesn't exist。

板子平时就插在这台 Mac 上（S3 原生 USB，`/dev/cu.usbmodem1201`，USB 树里显示为 Espressif "USB JTAG_serial debug unit"）：`idf.py -p /dev/cu.usbmodem1201 flash` 直接烧；`sdkconfig.local` 已把控制台切到 USB_SERIAL_JTAG，所以日志也从这根线出（CH343/COM7 那条路只在 Windows 用）。esptool 连一次即硬复位，复位后开端口读就是完整开机日志。BLE 也能从 Mac 直连测试：`python + bleak`（Warp 需在「隐私与安全性 → 蓝牙」授权，否则子进程直接被 TCC 杀掉且无报错）。

换机后 `sdkconfig.local` 丢了、板子还插着的话，可以先从现役固件里找回凭据：`esptool read_flash` 读 app 分区，在 SSID 字面量附近 ±500 字节找可打印字符串当密码候选，逐个试（编译→烧录→扫网验证）。注意排除 `192.168.4.1`、探测 IP 这类形似密码的干扰串。凭据只写进 `sdkconfig.local`，不进日志和对话。

抓开机日志：`python capture_boot.py <秒数> <输出文件>`（这块板要 DTR+RTS 一起拉低才复位，且复位动作发生在关闭端口那一刻，脚本已按实测绕过，细节在文件头注释）。

## 配置分层——最大的坑

`sdkconfig`（gitignore）才是编译时真正生效的那份。`sdkconfig.defaults`（已提交：硬件配置）和 `sdkconfig.local`（gitignore：路由器 SSID/密码；模板 `sdkconfig.local.example`）由 `wifi_led/CMakeLists.txt` 里的 `set(SDKCONFIG_DEFAULTS ...)` 合并，但**只在 `sdkconfig` 不存在时才被读取**。改完其中任何一个：`rm wifi_led/sdkconfig` 再 build，否则不生效。Wi-Fi 凭据只放在 `sdkconfig.local`——绝不进代码、日志、提交或文档。

## 架构

组件工程，围绕一份共享的链路状态展开。之所以把连接和探测放到独立任务：`esp_wifi_connect()` 和外网探测都会阻塞，不能放在事件回调或主循环里。依赖单向：`monitor_server`/`status_light` 只读 `link_state`，`ble_gatt` 读 `status_light` 和 `monitor_server`，`link_state` 不知道其他组件存在。

- `components/link_state/`——`connect_task` 每次尝试前先扫描并打印所有可见 AP（任何连接失败的诊断记录），锁定目标 SSID 最强的 BSSID，然后按认证模式梯队 `k_attempts[]` 逐级尝试（WPA2 任意 BSS → WPA2 锁定 BSSID → WPA/TKIP → open → WPA3），成功后回到第 0 档；`probe_task` 每 5 s 向 `114.114.114.114:53` 发起 TCP 连接（裸 IP，本地 DNS 坏了也测得出真实外网连通性）；Wi-Fi/IP 事件回调把事件翻译成事件组位（`BIT_GOT_IP`/`BIT_LOST_IP`）并更新状态。
- `components/status_light/`——`led_task` 是 WS2812 的唯一写入者（RMT 后端），20 ms 刷新一次；自动模式渲染链路状态：蓝色呼吸 = 连接中，绿色 = 仅连上路由，青色 = 外网也通，紫色 = 有设备接入板子热点。手动接管模式（互斥锁保护的 `status_light_ctl_t`，网页和 BLE 共用）改渲染下发的灯效（solid/breathe/rainbow/off + 亮度），完全覆盖状态色；接管设定只存 RAM，重启回自动。
- `components/monitor_server/`——`http://<板子IP>/` 的仪表盘以 C11 原始字符串字面量放在 `web_index.h`（改完 JS 用 `check_page_js.py` 验证）；`/api/status` 每次请求现取现算 JSON（`esp_wifi_sta_get_ap_info()` + 堆统计），刻意不缓存，页面不会和驱动实际状态漂移；`/api/led` GET/POST 读写接管设定（字段越界返回 400）；`/api/screen` 驱动外接屏（纯色/三色条/表情）。
- `components/ble_gatt/`——NimBLE GATT 服务器（`CONFIG_EXAMPLE_BLE_ENABLE`，默认开）：一个 128-bit 自定义服务，LED 特征值收发与 `/api/led` 同款 JSON（非法值返回应用层错误 0x81），状态特征值与 `/api/status` 同字段但去掉热点客户端列表。在 Wi-Fi 完全启动后初始化（照抄官方共存示例顺序），空口分时由 IDF 共存层自动处理。明文免配对——射频内任何人可控灯，要加密加 `BLE_GATT_CHR_F_WRITE_ENC`。单连接，断开自动恢复广播。
- `components/display_tft/`——ST7789 SPI 屏 + `esp_emote` 表情栈（资源烧在 `anim_icon` 分区）；手动刷屏和渲染器共用 panel io 的应答区分是这个组件里最容易踩的死锁，见 README。
- `components/i2c_probe/`——I2C 总线扫描（默认编译关闭，GPIO 8/9 已被屏占用）。

- **内嵌 `<script>` 的语法错误会让整页 JS 静默不执行**——症状是页面永远停在初始占位文本（"连接中"/"等待数据…"），按钮全死，浏览器控制台才看得到 `Unexpected token`。改完页面跑 `python check_page_js.py`（把编译进固件的 JS 抽出来 `node --check`）。2026-10-08 真实踩中过（坏掉的三元表达式）。

SoftAP 兜底默认编译掉（`CONFIG_WIFI_LED_AP_ENABLE` 未设置）：Windows 电脑一连上就把板子提成默认网关，而 esp_wifi 的 AP 不会 NAT 到 STA 上行，这台电脑就断网直到断开热点。要打开：在 `main/Kconfig.projbuild` 里设 y，然后 `rm sdkconfig` 重新 build。

所有项目旋钮（SSID、密码、灯脚/数量、单色灯、AP 设置、屏引脚、BLE 开关/广播名）都是 `main/Kconfig.projbuild` 里的 Kconfig 选项，没有硬编码。分区表 `partitions.csv` 是 OTA 布局（ota_0/ota_1 各 4 MiB + anim_icon 3 MiB 放表情资源）。托管组件：`espressif/led_strip`（RMT 驱动）、`espressif/mdns`、`espressif2022/esp_emote_expression`（表情栈，连带 LVGL 等 14 个依赖）。
