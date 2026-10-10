# 目标与路线

这份文件是仓库的"为什么"。代码注释只讲怎么做，这里讲要去哪、已经走到哪、哪些坑不要再踩。
换台电脑接手时先读这个。

## 长期目标

把一块 ESP32-S3-WROOM-1-N16R8 变成自己家里能说话、能看、能控的东西 —— 不靠厂商云，
服务端跑在自己机器上。学习方式是玩成品开源项目，不是看教程。

## 已经闭环（不要重开）

| 事项 | 结论 |
| --- | --- |
| 开发环境 | ESP-IDF v5.3.5 在 `C:\Espressif\frameworks\esp-idf-v5.3.5`，Windows + Git Bash 只能走 `idf_run.ps1 -Dir <工程> [-Port COMx] <action>` |
| 工具链 | `hello_world` 编译烧录运行通过 |
| Wi-Fi | 连上家里路由（2.4 GHz / WPA2），拿到 DHCP 地址，外网探测可达。之前几个月的 `reason=204` 就是**密码填错** |
| 板载 WS2812 | GPIO 48 确认，三色链路状态机（呼吸蓝=未关联 / 绿=仅内网 / 青=外网通） |
| Octal PSRAM | 8 MiB @80 MHz 实测活着，内存测试通过 |
| 网页监控 | `esp_http_server` 提供仪表盘 + `/api/status`，手机/电脑连同一个 Wi-Fi 直接访问 |
| 发布 | 已推到 GitHub 私有仓库，不含任何真实凭据 |

## 接力第一步：把新固件烧进去 —— 已完成（2026-10-08 00:40）

结论先说：**之前那次"烧录成功"是假的**，板子跑的一直是重构前的镜像。成因不是编译，
是启动器被 PowerShell 的参数绑定坑了两次：

1. `-p COM7` 这种未知短选项会被 PowerShell 整个丢掉，idf.py 收不到端口就去自动探测，
   探到的是蓝牙虚拟串口（板子没插 USB 时机器上只剩 COM3/COM4 两个 `BTHENUM` 口）。
2. 更阴的：脚本参数**默认按声明顺序做位置绑定**，所以加了 `[string]$Port` 之后，
   `-Dir wifi_led build` 里的 `build` 被塞给了 `-Port`，`idf.py` 收到空动作，
   只打印一遍 usage 就返回 **exit 0**。

修法：`idf_run.ps1` 改成手写 `$args` 解析，不再有声明式参数参与绑定；
`-Port COM7`（内部转 `ESPPORT`）和原样透传的 `-p COM7` 现在都对；
没有动作时脚本自己报错退出，不会再"静默成功"。

验收证据（都是量出来的，不是"命令返回 0"）：

| 检查 | 结果 |
| --- | --- |
| 烧录 | app 924,080 B @0x10000、otadata @0xd000、分区表 @0x8000，全部 `Hash of data verified` |
| `GET /api/status` | 出现 `led_ready:true` 与 `host:"esp32-monitor.local"` |
| `POST /api/led` | 200 + `{"ok":true}`，坏 body 回 400 |
| 灯 | 手动橙 `(230,160,20)` 覆盖 6 s 后交还青色状态机 |

## 现在卡在哪：外接那块屏（已确认是 SPI TFT，只差 4 根线）

2026-10-10 用户给出商品页：**1.54 寸 240×240 IPS，驱动 IC = ST7789，4 线 SPI**，
8 针定义 `1.GND 2.VCC 3.SCL(时钟) 4.SDA(数据) 5.RES 6.DC 7.CS 8.BLK(背光，默认悬空，低电平关)`，3.3 V。
所以之前按丝印猜的"I2C OLED"**是错的**，`i2c_probe` 在这块屏上没有意义，已默认关掉
（`WIFI_LED_I2C_PROBE_ENABLE=n`，GPIO 8/9 现在是 SPI 的 SCLK/MOSI）。

驱动已经写完并烧进去了，用的不是 registry 组件而是 **IDF 自带的 `esp_lcd` ST7789 面板驱动**
（`components/display_tft`，SPI2 + `esp_lcd_new_panel_st7789()`）—— 这台电脑当时没有外网，
拉不到 `espressif/esp_lcd_st7789`，而 IDF 内置版本本来就应该先查。

| 模块引脚 | 接到 | Kconfig | 状态 |
| --- | --- | --- | --- |
| GND | GND | | 已接 |
| VCC | 3V3 | | 已接 |
| SCL | GPIO 9 | `WIFI_LED_TFT_SCLK_GPIO` | 已接 |
| SDA | GPIO 8 | `WIFI_LED_TFT_MOSI_GPIO` | 已接 |
| RES | GPIO 4 | `WIFI_LED_TFT_RST_GPIO` | **待接** |
| DC | GPIO 5 | `WIFI_LED_TFT_DC_GPIO` | **待接** |
| CS | GPIO 6 | `WIFI_LED_TFT_CS_GPIO` | **待接** |
| BLK | GPIO 7（或直连 3V3） | `WIFI_LED_TFT_BLK_GPIO` | **待接** |

BLK 是这块屏"为什么不亮"的第一嫌疑：**它悬空，而悬空等于背光关**。固件已经把 GPIO 7 拉高，
所以要么接 GPIO 7，要么直接把 BLK 插到 3V3（`WIFI_LED_TFT_BLK_GPIO=-1` 表示后者）。

已经验过的（2026-10-10，板子 uptime 23 s）：

- 开机日志 `display_tft: ST7789 240x240 on SCLK=9 MOSI=8 DC=5 CS=6 RST=4 at 20000 kHz`
- `GET /api/status` → `screen_ready: true`、`led_ready: true`、内部堆 205 KiB 空闲
- `POST /api/screen {"r":..,"g":..,"b":..}` 红/绿/蓝/白全部 `200 {"ok":true}`

**剩下的验收是物理的**：接完那 4 根线，屏上应该出现左中右**红/绿/蓝三色竖条**（开机自绘，
是接线正确的证据）；网页"手动刷屏"按钮换色后仍然正确，说明 DC/CS 没接反。
三色条花乱或整体偏移 → 引脚表错了；全黑但背光亮 → 数据没进去；全白 → 忘开反色
（本驱动固定 `esp_lcd_panel_invert_color(true)`，IPS 屏不开就是负片）。

## 当前主线：小智（xiaozhi-esp32）语音助手

选型理由：固件是成熟基座（171 个板型），服务端全开源，浏览器端能先跑起来验证整条链路，
不用等硬件到货。

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| P0 | 服务端容器起来 + **digital-human 浏览器对话**（当它是"没有硬件时的假设备"） | 卡在 LLM/ASR 的 key |
| P1 | 智控台（`docker-compose_all.yml`：server + manager-web + manager-api + MySQL + Redis） | 未开始 |
| P2 | 装 ESP-IDF **6.1**（小智要求 ≥6.0，5.x 不支持）与现有 5.3.5 并存 | 未开始 |
| P3 | 音频硬件二选一：面包板 INMP441 + MAX98357A + 喇叭，或直接买带麦成品板 | 未决定 |
| P4 | 小智固件烧到自己的板子，OTA 地址指向自建服务端 | 未开始 |

已定的两个决策：先做 digital-human 再补智控台；凭据走云端兼容 key（阿里百炼 / 硅基流动 / OpenAI 格式），不自建模型。

## 米家：只接一边，另一边别去碰

- **米家设备 → 我的项目：可行，而且有现成的。** 小智服务端自带 Home Assistant 插件
  （`plugins_func/functions/hass_state.py` 的 `hass_get_state` / `hass_set_state`、
  `hass_play_music`、把 HA 当 LLM 用的 `core/providers/llm/homeassistant/`、
  以及预置好的 Home Assistant MCP server）。链路是
  `板子/浏览器 → 小智服务端 → HA REST API → XiaoMi/ha_xiaomi_home（小米官方集成）→ 米家设备`。
- **我的 ESP32 → 米家 App：不去做。** 小米 IoT 开发者平台要企业资质和产品认证；
  Matter 路线要求 CSA 颁发的 DAC 证书，个人只有测试证书，而米家能否给第三方 Matter 设备配网
  没有官方说法；ESPHome 核心至今没有 matter 组件。
- **折中（借壳）**：让外部大脑 POST 板子的 `/api/led`。小爱同学 → 米家智能场景 → HA →
  `http://<板子>/api/led`。代价是米家里那台设备永远叫"插座"。

```bash
curl -X POST -H "Content-Type: application/json" \
     -d '{"r":230,"g":160,"b":20,"hold_ms":6000}' http://<板子IP>/api/led
```

## 已知的坑（都踩过，别再推导一遍）

1. **板子自带热点会掐掉接入方的上网**。`esp_wifi` 的 SoftAP 只有二层 AP + 自己的 DHCP，
   不 NAT 到它的 STA 上行，Windows 又把热点提到默认网关（RouteMetric 0 对 9999）。
   所以 AP 默认编译关闭（`WIFI_LED_AP_ENABLE=n`）。
2. **不要 ping ESP32**。lwIP 不回 ICMP，ping 不通不代表设备不活着，探 TCP 80。
3. **编译成功 ≠ 板子配置对了**。stock 例程的 `sdkconfig` 默认是 2MB flash 且 PSRAM 关；
   验证要 grep 生成出来的 `sdkconfig`，不是看"它编过了"。
4. **`sdkconfig` 是真正生效的那份**，改 `sdkconfig.defaults` / `sdkconfig.local` 后要删掉它。
5. **打开串口会把板子复位进正常启动** —— 这正好是抓开机日志的办法；但手动拉 DTR/RTS
   会让这块板子停在 bootloader 且一个字节都不打。长抓和 `idf.py flash` 不能同时进行。
   反过来它也坑过验收：`HTTP 全不通` 有一次纯粹是因为我在抓日志后立刻发请求，撞进板子的
   5 秒开机窗口。
6. **"网页只在刚开机时能通"目前是证伪的**：2026-10-08 00:02 板子已运行 209 s，
   `GET /api/status` 仍然 200 / 60 ms，IP 也还是原来那个。剩下的复验要做的是长跑几小时后
   再连一次，别急着改重连逻辑。
7. **`http://esp32-monitor.local/` 在这台 Windows 上解析不出来**（`Resolve-DnsName` 无答案，
   Windows 不带头 mDNS 解析，LLMNR 也不应答 ESP32 的公告）。手机 / macOS 上可以。
   验收时用 IP，不要把 `.local` 失败当成固件坏了。
8. **验收脚本不要"读到连接关闭为止"**。`esp_http_server` 收到 `Connection: close` 也不会
   主动关，它按 `Content-Length` 把响应发完就静默。所以"发完请求 recv 到 EOF"这种写法会
   一直超时，看起来像板子挂了 —— 2026-10-10 就被这个骗过一次，实际响应 **0.1 秒就完整到了**。
   正确做法：解析头部 `Content-Length`，读够字节就走（`board_api.py` 就是这么写的）。
   同一次误判里还有第 5 条叠加：开串口把板子复位了，紧接着的请求撞进开机窗口。

## 结构约定（本次改造立的，后续照这个长）

```
wifi_led/
  main/              只做接线：app_main.c 20 行 + Kconfig.projbuild
  components/
    link_state/      Wi-Fi 上线、安全模式重试阶梯、外网探测、状态发布
    status_light/    WS2812 渲染 + 手动覆盖接口
    display_tft/     ST7789 240x240 SPI 屏（IDF 内置 esp_lcd 面板驱动）+ /api/screen 后端
    i2c_probe/       总线扫描，认外接模块用；默认编译关闭
    monitor_server/  HTTP 页面与 API、mDNS
  partitions.csv     otadata + 双 app 槽，给 OTA 留位
  sdkconfig.defaults 硬件事实（提交）
  sdkconfig.local    凭据（不提交）
```

依赖不自己造：`espressif/led_strip` 2.5.5、`espressif/mdns` 1.14.0、IDF 自带 `json`/`esp_http_server`。
