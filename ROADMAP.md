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
| 外接屏 + 表情 | ST7789 240×240 已点亮（8 根线全接），`esp_emote` 20 张表情从 `anim_icon` 分区跑通（2026-10-11）。**只剩屏上的肉眼确认** |
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

## 外接那块屏：已接好并点亮（2026-10-11）

2026-10-10 用户给出商品页：**1.54 寸 240×240 IPS，驱动 IC = ST7789，4 线 SPI**，
8 针定义 `1.GND 2.VCC 3.SCL(时钟) 4.SDA(数据) 5.RES 6.DC 7.CS 8.BLK(背光，默认悬空，低电平关)`，3.3 V。
所以之前按丝印猜的"I2C OLED"**是错的**，`i2c_probe` 在这块屏上没有意义，已默认关掉
（`WIFI_LED_I2C_PROBE_ENABLE=n`，GPIO 8/9 现在是 SPI 的 SCLK/MOSI）。

驱动用的是 **IDF 自带的 `esp_lcd` ST7789 面板驱动**（`components/display_tft`，SPI2 +
`esp_lcd_new_panel_st7789()`）。2026-10-11 外网恢复后，表情这一层是**直接从 registry 拉的组件**，
没有自己写渲染器 —— 见下一节。

| 模块引脚 | 接到 | Kconfig | 状态 |
| --- | --- | --- | --- |
| GND | GND | | 已接 |
| VCC | 3V3 | | 已接 |
| SCL | GPIO 9 | `WIFI_LED_TFT_SCLK_GPIO` | 已接 |
| SDA | GPIO 8 | `WIFI_LED_TFT_MOSI_GPIO` | 已接 |
| RES | GPIO 4 | `WIFI_LED_TFT_RST_GPIO` | 已接 |
| DC | GPIO 5 | `WIFI_LED_TFT_DC_GPIO` | 已接 |
| CS | GPIO 6 | `WIFI_LED_TFT_CS_GPIO` | 已接 |
| BLK | GPIO 7（或直连 3V3） | `WIFI_LED_TFT_BLK_GPIO` | 已接 |

接线正确性的固件侧证据（2026-10-11，`python capture_boot.py` 抓的新鲜开机日志）：

- `display_tft: ST7789 240x240 on SCLK=9 MOSI=8 DC=5 CS=6 RST=4 at 20000 kHz`
- `Expression_load: Found 20 emoji items / 8 icon items / 10 layout items`
- `display_tft: emote ready: 240x240, assets in partition anim_icon`
- `GET /api/status` → `screen_ready:true`、`emote_ready:true`、内部堆 ~161 KiB 空闲
- `POST /api/screen` 的 `bars` / 纯色 / `emote` 三种写法全部 `200 {"ok":true}`，
  60 轮交替压测 + 表情连续跑 60 s，串口 0 条报错

**还欠一步物理验收**：屏上肉眼看到的东西只有用户能确认 —— 开机应先出现红/绿/蓝三色竖条，
然后切 `happy` 表情。花乱或整体偏移 = 引脚表错；全黑但背光亮 = 数据没进去；
全白 = 忘开反色（本驱动固定 `esp_lcd_panel_invert_color(true)`，IPS 屏不开就是负片）。

## 屏上的表情：用了 `esp_emote`，2026-10-11 已在板子上跑通

当初查注册表时猜的版本有两处是错的，按 `dependencies.lock` 的实际结果纠正：

| 组件 | 实际装上 | 说明 |
| --- | --- | --- |
| `espressif2022/esp_emote_expression` | 1.0.2 | 表情管理器：`emote_init()` / `emote_set_anim_emoji()` / 事件 `EMOTE_MGR_EVT_*` |
| `espressif2022/esp_emote_gfx` | 3.0.5 | 渲染层。**它把 LVGL 9.6.0 拖进来了**（`lvgl__lvgl`），当初"不依赖 LVGL"的说法只对早期 1.x 成立 |
| `espressif2022/esp_emote_assets` | 0.1.2 | 资源包：自带 `build_all.py` 和 320_240 / 536_240 等分辨率目录，字体是 PuHuiCJK |
| `espressif__esp_mmap_assets` | 1.4.0 | 按名字从分区 mmap 资源 |

一共 14 个依赖（还有 `esp_new_jpeg`、`freetype`、`heatshrink`、`cjson`、`cmake_utilities`、
`esp_lcd_touch`），`idf.py reconfigure` 一次拉齐，在 `5.3.5` 上没有版本冲突。声明的位置是
`wifi_led/components/display_tft/idf_component.yml`，所以依赖归属清晰：**只有屏组件认识表情栈**。

做法要点（细节都在 `wifi_led/README.md` 的"表情动画"一节）：

- 240×240 不是猜的：直接解析组件自带的 `.eaf` 动画头，`emoji_large` 最大帧 **125×160**，
  `listen` 100×46，所以 240×240 装得下，不需要退到 320×240。
- 分辨率目录放在仓库里（`wifi_led/assets/240_240/`），不是塞进 `managed_components/`——
  那个目录会被 `reconfigure` 重新生成，塞进去的东西留不住。
- 打包 + 烧录挂在 `idf.py build flash` 上（`build_speaker_assets_bin()` +
  `esptool_py_flash_to_partition()`），资源分区 `anim_icon` 3 MiB @ `0x810000`，产物 2,448,784 B。
- `CONFIG_MMAP_FILE_NAME_LENGTH=32` 必须与打包参数 `--name_length 32` 一致，否则挂载时按名字取不到文件。
- 这套资源和小智同源，以后 P4 做带屏板型时能直接复用，不是白干。

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
5. **串口复位这件事，2026-10-11 把之前的结论推翻了**。单独脉冲 RTS（`dtr=False` + `rts` 打一下）
   在这块板上**根本不会复位** —— 脚本以为复位了，其实日志时间戳一直在往上跑；判据就是看时间戳
   有没有回到个位数。真正有效的配方（`capture_boot.py` 用的）是：
   **带着 DTR 和 RTS 同时断言打开端口（此时芯片被按在复位里、一个字节都不发）→ 关闭端口（松手 = 开始开机）→
   立刻重开设成不拉线 → 读**。另外在板子活着的时候反复开关端口，偶尔会把它留在 bootloader 里且不打字，
   看到"0 字节"别当成固件挂了，先用 `board_api.py status` 确认。
   长抓日志和 `idf.py flash` 不能同时进行（端口独占）。
6. **别在中文 Windows 控制台上直接跑打包脚本**。GBK 码页下 idf.py 用子进程的 GBK 编码器转发 stdout，
   `build_all.py` 自己打的 `✓`（U+2713）会抛 `UnicodeEncodeError` 把 build 打断 —— 而且 ninja 其实已经干完活了，
   留下的 ELF 还是旧的。修法在 `idf_run.ps1` 里：`$env:PYTHONIOENCODING='utf-8'`。
7. **中断过的 ninja 会喂坏 ccache**。一次 Ctrl+C 之后 `lvgl__lvgl` 的某个 `.obj` 变成 1403 字节全零，
   删掉重编也会被 ccache 再端出来，链接时报 `file format not recognized`。要
   `ccache -C` 清缓存再删那个 obj（用 python 扫全部 obj 的头 4 字节看是不是 ELF，才知道坏的是哪一个）。
8. **表情渲染器和手动刷屏共用一个 panel io，应答信号必须分派**，否则整台板子的 API 会"无声猝死"：
   渲染任务每块 flush 等一个 `emote_notify_flush_finished()`，等的时候握着 gfx 锁；
   多出来 / 丢失的应答让它永久等下去，而 `esp_http_server` 只有一个任务处理所有请求，
   于是一起死，**不崩、不重启、也没有 WDT 打印**。`display_tft.c` 里的 `s_emote_awaits_ack` +
   手动刷屏全程 `panel_lock()` + 提交失败就地补应答，三条都是为了这个。回归脚本 `emote_stress.py`。
9. **"网页只在刚开机时能通"目前是证伪的**：2026-10-08 00:02 板子已运行 209 s，
   `GET /api/status` 仍然 200 / 60 ms，IP 也还是原来那个。剩下的复验要做的是长跑几小时后
   再连一次，别急着改重连逻辑。
10. **`http://esp32-monitor.local/` 在这台 Windows 上解析不出来**（`Resolve-DnsName` 无答案，
   Windows 不带头 mDNS 解析，LLMNR 也不应答 ESP32 的公告）。手机 / macOS 上可以。
   验收时用 IP，不要把 `.local` 失败当成固件坏了。
11. **验收脚本不要"读到连接关闭为止"**。`esp_http_server` 收到 `Connection: close` 也不会
   主动关，它按 `Content-Length` 把响应发完就静默。所以"发完请求 recv 到 EOF"这种写法会
   一直超时，看起来像板子挂了 —— 2026-10-10 就被这个骗过一次，实际响应 **0.1 秒就完整到了**。
   正确做法：解析头部 `Content-Length`，读够字节就走（`board_api.py` 就是这么写的）。
   同一次误判里还有第 5 条叠加：开串口把板子复位了，紧接着的请求撞进开机窗口。


## 结构约定（本次改造立的，后续照这个长）

```
wifi_led/
  main/              只做接线：app_main.c 20 行 + Kconfig.projbuild；表情资源 bin 的 CMake 挂钩也在这层的 CMakeLists
  components/
    link_state/      Wi-Fi 上线、安全模式重试阶梯、外网探测、状态发布
    status_light/    WS2812 渲染 + 手动覆盖接口
    display_tft/     ST7789 240x240 SPI 屏（IDF 内置 esp_lcd 面板驱动）+ esp_emote 表情渲染 + /api/screen 后端
    i2c_probe/       总线扫描，认外接模块用；默认编译关闭
    monitor_server/  HTTP 页面与 API、mDNS
  assets/240_240/    表情资源包的分辨率描述（config/emote/layout 三个 json），资源本体在 managed_components 里
  partitions.csv     otadata + 双 app 槽 + anim_icon(3M) + storage
  sdkconfig.defaults 硬件事实（提交）
  sdkconfig.local    凭据（不提交）
```

仓库根上还有四个验收脚本：`board_api.py`（按 Content-Length 读 HTTP）、`capture_boot.py`（复位+抓开机日志）、
`emote_stress.py`（刷屏 API 交替压测）、`emote_soak.py`（表情烤机 + 盯串口）。改固件后的验收先看它们，别手点浏览器。

依赖不自己造：`espressif/led_strip` 2.5.5、`espressif/mdns` 1.14.0、
`espressif2022/esp_emote_expression` 1.0.2（连带 LVGL 9.6 等 14 个），IDF 自带 `json`/`esp_http_server`/`esp_lcd`。
