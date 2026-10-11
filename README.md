# ESP32-S3 沙箱

个人练手仓库：一块 ESP32-S3-WROOM-1-N16R8 开发板 + 已经跑通的 Wi-Fi 状态灯 / 网页监控项目。
代码按 ESP-IDF 工程组织，可以直接拿去做新项目的底座。

| 目录 | 说明 |
| --- | --- |
| [`ROADMAP.md`](ROADMAP.md) | **目标、路线、已定的决策、踩过的坑** —— 接手先读这份 |
| [`wifi_led/`](wifi_led/README.md) | 主项目：Wi-Fi 状态灯 + HTTP 监控页 + 网页/BLE 接管灯带（拆成组件，可当新项目的底座） |
| `idf_run.ps1` | Windows + Git Bash 下唯一能跑通 `idf.py` 的入口 |
| `board_api.py` | 按 `Content-Length` 读的 HTTP 探针（板子的 API 发完不关连接，裸 curl 会假死） |
| `capture_boot.py` | 复位板子并抓开机日志（这块板的复位配方写在文件头注释里） |
| `emote_stress.py` | 刷屏 API 交替压测 —— 三色条/纯色/表情混打，兼作死锁回归 |
| `emote_soak.py` | 复位后让表情连续跑 N 秒，同时盯串口有没有报错 |
| `check_page_js.py` | 把编译进固件的页面 JS 抽出来 `node --check`（JS 语法错误不会让任何 API 报错） |
| [`tools/ble_gatt_test.py`](tools/ble_gatt_test.py) | BLE 回归测试（bleak 直连开发板，7 项检查；用法见脚本头部） |
| [`docs/`](docs/README.md) | 乐鑫官方手册（芯片规格书 v2.2、WROOM-1 模组手册、TRM，中文 PDF） |
| [`docs/battery_wiring.svg`](docs/battery_wiring.svg) | 锂电池供电 / 充电 / 电量采样接线图（方案已定，**线还没接**） |

## 硬件事实

- 模块 **ESP32-S3-WROOM-1-N16R8**：16 MiB flash + 8 MiB **Octal（八线）** PSRAM，双核 LX7 @240 MHz。
- PSRAM 是八线的，固件必须 `CONFIG_SPIRAM_MODE_OCT=y`；按四线配会开机但 PSRAM 初始化失败。
- 八线 PSRAM 吃掉 **GPIO 26–37**，但排针照样引出来了 —— 接线避开这一段。
- 可用引脚：0–21、38–48；另避开 19/20（原生 USB）、43/44（UART）、0/3（strapping）。
- 板载 WS2812 在 **GPIO 48**（已物理确认）。串口桥是 CH343，Windows 上枚举为 COM7。
- 外接 1.54 寸 **ST7789 240×240 SPI 屏**已跑通：SCLK 9 / MOSI 8 / DC 5 / CS 6 / RES 4 / BLK 7，20 MHz。
  **`BLK` 出厂悬空 = 背光关**，这是"屏不亮"的第一嫌疑。表情动画用 `esp_emote` 栈，资源烧在 `anim_icon` 分区。
- ESP32-S3 **只有 BLE，没有经典蓝牙**，手机系统蓝牙设置里搜不到它，要用 nRF Connect。

## 换台电脑怎么跑起来

1. 装 ESP-IDF **v5.3.5**（离线安装包 `esp-idf-tools-setup-offline-5.3.5.exe`，
   下载地址 <https://dl.espressif.com/dl/idf-installer/>；注意不是 `/dl/esp-idf/`，那个 404）。
2. 克隆本仓库，进入 `wifi_led/`。
3. 造一份本地凭据文件（这个文件被 `.gitignore` 掉了，仓库里不含任何真实密码）：

   ```bash
   cp sdkconfig.local.example sdkconfig.local
   # 编辑 sdkconfig.local，填你自己路由器的 SSID 和密码
   ```

4. 激活环境后编译烧录：

   ```bash
   . $IDF_PATH/export.sh          # 或 Windows 上运行起 ESP-IDF 的终端
   idf.py set-target esp32s3
   idf.py build
   idf.py -p COM7 flash           # 端口按实际改
   idf.py -p COM7 monitor
   ```

5. 刷机后看串口里的 `got IP: 192.168.x.x -> http://192.168.x.x/`，浏览器直接打开就是这个页面。
   同一行还会打出 mDNS 地址 `http://esp32-monitor.local/`，板子换了 DHCP 地址也能找到它。
   **别用 `ping` 判断板子在不在** —— lwIP 默认不回应 ICMP，直接探 80 端口。

> `sdkconfig` 是编译时真正生效的那份，`sdkconfig.defaults` + `sdkconfig.local` 只在
> `sdkconfig` 不存在时被读取。改完这两个文件要 `rm sdkconfig` 再 build，否则不生效。

## 不要提交的东西

`build/`、`sdkconfig`、`sdkconfig.local`、`managed_components/`、`ap_profile.xml` 全在
`.gitignore` 里。路由器密码只存在于本地 `sdkconfig.local`，别写进日志、提交信息或文档。
