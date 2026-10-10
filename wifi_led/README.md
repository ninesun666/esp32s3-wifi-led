# wifi_led — Wi-Fi 状态灯 + 网页监控

用一颗 WS2812 把联网状态显示出来，同时在板子上跑一个 HTTP 服务，浏览器直接看实时状态。
这是这个沙箱里第一个跑通的项目，后面的项目可以拿它当底座。

## 当前状态（2026-10-10 实测）

| 项 | 结果 |
| --- | --- |
| Wi-Fi | 已连上家里路由，`esp_wifi_connect()` 后约 1.4 s 拿到 `192.168.1.6` |
| 外网 | TCP 探测 `114.114.114.114:53` 通过 |
| PSRAM | 8 MiB Octal @ 80 MHz，bootloader 自检 OK |
| Flash | 16 MiB |
| RGB 灯 | GPIO 48 上的 WS2812，物理确认会亮，串口打出 `rgb=(0,120,120)` |
| 网页 | `http://<板子IP>/` 返回 200（实测 5789 B），2 s 轮询 `/api/status`；`POST /api/led` 可控灯 |
| 分区 | OTA 布局生效：`ota_data_initial.bin` 烧在 `0xd000`，app 落在 `ota_0`（4 MiB 槽用了 23%） |
| SPI 屏 | ST7789 驱动起来了：`screen_ready:true`，`POST /api/screen` 红/绿/蓝/白全回 200。8 根线已接好，固件侧（开机日志 + API）验收过，**屏上的肉眼确认还欠着** |
| 表情 | `esp_emote` 栈跑通：`emote_ready:true`，20 张表情从 `anim_icon` 分区加载，`POST /api/screen {"emote":"happy"}` 回 200，60 轮混打 + 60 s 动画烤机 0 条报错日志 |

堆：内部空闲约 160 KiB（表情栈把 LVGL 渲染器、两块 DMA 缓冲和 `gfx_render` 任务的栈都吃进内部 RAM，
比没有表情时少约 45 KiB），PSRAM 空闲 8.0 MiB。

> 之前记的"网页只在刚开机时能通"是**误判**，成因有两个，都会复现：
> ① 验收脚本读到 EOF 才算完，而 `esp_http_server` 发完 `Content-Length` 就不关连接了；
> ② 打开串口本身会复位板子，紧接着发的请求撞进 ~5 秒的开机窗口。
> 结论：响应实际 0.1 秒就完整到达。用 `board_api.py`（按 `Content-Length` 读）而不是裸 `curl`。

## 灯语

| 颜色 | 含义 |
| --- | --- |
| 蓝色呼吸 | 还没关联上路由器 |
| 常亮绿 | 连上路由器，但外网探测不通 |
| 常亮青（绿+蓝） | 路由器 + 外网都通 |
| 常亮紫（红+蓝） | 有设备接入了板子自己的热点（只在热点开启时可能出现） |

状态由 Wi-Fi/IP 事件回调写、由独立的 `led_task` 每 20 ms 刷一次。连接和外网探测都会阻塞，
所以不能在主循环里点灯。

## 网页监控

- `/` —— 深色面板：连接状态、IP、SSID/加密方式、RSSI、信道、运行时长、内部堆与 PSRAM 空闲、
  外网连通、热点状态、接入设备列表，最近 60 次采样的信号强度柱状图，外加一排手动点灯和刷屏按钮。
- `/api/status` —— 同样内容的 JSON，由 cJSON 现取现算（`esp_wifi_sta_get_ap_info()` + 堆统计），
  不做缓存，所以页面不会和驱动的实际状态漂移。
- `/api/led` —— `POST`，`{"r":0-255,"g":0-255,"b":0-255,"hold_ms":0-600000}`，
  覆盖链路颜色若干毫秒后自动交还状态机；`hold_ms:0` 立刻交还。这是留给外部大脑
  （Home Assistant / 米家场景 / curl）控制的入口，见 [`../ROADMAP.md`](../ROADMAP.md)。
  **无鉴权**，只应在局域网内暴露。
- `/api/screen` —— `POST`，三种写法：
  - `{"emote":"happy"}` 刷表情（名字必须来自资源包，见"表情动画"一节；页面上的按钮就是这批名字）。
    不认识的名字回 `{"ok":false,"error":"no such emote in the asset pack"}`。
  - `{"pattern":"bars"}` 重画红/绿/蓝三色竖条（接线自检测，不用按板子的复位键）。
  - `{"r":0-255,"g":0-255,"b":0-255}` 整屏一色。
  手动刷屏会让表情隐身，直到下一次 `emote` 请求 —— 屏没有"交还状态机"的概念，画面一直保留。
  屏没起来时回 `{"ok":false,"error":"panel not ready"}`。
- mDNS —— 板子应答 `http://<WIFI_LED_MDNS_HOSTNAME>.local/`（默认 `esp32-monitor`），
  不用记住 DHCP 地址。个别 Windows 解析不了 `.local`，退回 IP 即可。
- `i2c` 字段 / 页面"I2C 总线"卡片 —— `i2c_probe` 组件在 GPIO 8(SDA)/9(SCL) 上扫 0x08–0x77，
  **同一个地址连续两次应答才算数**（空总线会浮出随机的假 ACK，实测过 0x0b 这种幽灵地址）。
  没扫到就每 5 秒重扫，所以外接模块可以随时插上看网页，不用重新烧录、也不用开串口。
  **现在默认编译关闭**（`WIFI_LED_I2C_PROBE_ENABLE=n`）：GPIO 8/9 已经是屏的 SCLK/MOSI，
  卡片会在字段缺失时自动隐藏。

页面标记在 `components/monitor_server/web_index.h`，用 C11 原始字符串字面量编译进固件，
不占 SPIFFS 分区；`monitor_server.c` 里只有路由和 JSON，没有 HTML。

## 工程结构

```
main/                 app_main.c 只做接线：link_state_init / status_light_start / display_tft_start / monitor_server_start
components/
  link_state/         Wi-Fi 上线、安全模式重试阶梯、外网探测、状态与 IP 发布
  status_light/       WS2812 渲染 + 手动覆盖（依赖 espressif/led_strip）
  display_tft/        ST7789 240x240 SPI 屏，走 IDF 内置 esp_lcd 面板驱动 + SPI2；表情渲染也在这里
  i2c_probe/          I2C 总线看门狗，识别外接模块是否应答（用 IDF 自带 esp_driver_i2c，默认关）
  monitor_server/     HTTP 页面与 API + mDNS（依赖 espressif/mdns）
assets/240_240/       表情资源包的分辨率描述（config.json / emote.json / layout.json），编译时打成 bin
partitions.csv        nvs / otadata / phy_init / ota_0 / ota_1 / anim_icon(3M) / storage
```

组件之间的依赖方向是单向的：`monitor_server` 和 `status_light` 只读 `link_state`，
`monitor_server` 另读 `display_tft` 的填充接口，`i2c_probe` 谁也不读，
`link_state` 不知道上面几位存在。加新功能时按这个方向放，不要出现回指。

## 外接屏（1.54 寸 240×240 IPS，ST7789，4 线 SPI）

块头是商品页给的，不是猜的：**驱动 IC = ST7789，240×240，8 针里带 `BLK`**。
之前按丝印 `SCL/SDA` 猜它是 I2C OLED 是错的，所以 `i2c_probe` 现在默认关掉
（那两根线已经是屏的 SCLK/MOSI，浮着的 I2C 还会报幽灵地址）。

驱动用的是 **IDF 自带的 `esp_lcd_panel_st7789`**，没有引 registry 组件 ——
一是这台机器当时拉不到外网，二是内置版本先查了就该用。

| 屏针 | GPIO | Kconfig |
| --- | --- | --- |
| GND | GND | |
| VCC | 3V3 | 别接 5V |
| SCL（时钟） | 9 | `WIFI_LED_TFT_SCLK_GPIO` |
| SDA（数据） | 8 | `WIFI_LED_TFT_MOSI_GPIO` |
| RES | 4 | `WIFI_LED_TFT_RST_GPIO`（-1 表示外部已接 3V3） |
| DC | 5 | `WIFI_LED_TFT_DC_GPIO` |
| CS | 6 | `WIFI_LED_TFT_CS_GPIO` |
| BLK | 7 | `WIFI_LED_TFT_BLK_GPIO`，**或直接接 3V3 并把该项设 -1** |

两个必须记住的点：

1. **`BLK` 出厂是浮空的，而浮空 = 背光关**。屏"不亮"多半就是这个，不是驱动问题。
   固件开机把 GPIO 7 拉高。
2. **IPS 屏必须开反色**，否则颜色是负片（刷白看起来是黑）。`display_tft.c` 里固定调
   `esp_lcd_panel_invert_color(panel, true)`。

一帧 RGB565 是 112 KiB，所以不整帧传：`display_tft.c` 只留 32 行 × 240 的 DMA 条带
（15 KiB），逐条 `esp_lcd_panel_draw_bitmap()`。开机自绘**红/绿/蓝三色竖条**，
它就是接线正确的证据：条序错乱或整体偏移 = 引脚表错；全黑但背光亮 = 数据没进去。

SPI 时钟默认 20 MHz（走 GPIO 矩阵的安全值）。若把 SCLK/MOSI/CS 挪到 IO_MUX 脚
（12 / 11 / 10）就可以提到 40 MHz。

## 表情动画（esp_emote 栈）

表情不是自己画的，用的是乐鑫的 `espressif2022/esp_emote_expression`（在
`components/display_tft/idf_component.yml` 里声明 `^1.0.0`）。它会带进 14 个依赖：
`esp_emote_gfx`（渲染器）、`esp_emote_assets`（资源打包）、`esp_mmap_assets`（分区挂载）、
LVGL 9.6、FreeType、heatshrink 等 —— 全在 `idf.py reconfigure` 时自动落到 `managed_components/`。

### 资源包

`assets/240_240/` 是本项目自己的一份"分辨率目录"，只有三个 JSON：

| 文件 | 作用 | 注意 |
| --- | --- | --- |
| `config.json` | 选字体、选表情集、选唤醒模型 | `wakenet_model` 填 `none`，否则白白多 291 KB |
| `emote.json` | 表情名 → `.eaf` 文件的映射 | 直接从组件自带的 `320_240` 拷的，动画文件在组件目录里 |
| `layout.json` | 各元素在屏上的位置（眼睛、状态图标、toast、时钟、二维码…） | 按 240×240 重排过 |

打包和烧录都挂在 `idf.py build flash` 上，不用单独跑脚本：`main/CMakeLists.txt` 里的
`build_speaker_assets_bin("anim_icon" "240_240" …)` 调组件的 `build_all.py` 生成
`build/anim_icon_assets.bin`，紧接着 `esptool_py_flash_to_partition(flash "anim_icon" …)` 把它烧进
`anim_icon` 分区（3 MiB @ `0x810000`，实测产物 2,448,784 B）。

三个必须对齐的值，错了就表现为"表情挂载失败/找不到文件"：

1. `CONFIG_MMAP_FILE_NAME_LENGTH=32` 必须等于打包时的 `--name_length 32`。
2. 分区标签在 `partitions.csv`、`build_speaker_assets_bin()` 和
   `CONFIG_WIFI_LED_TFT_EMOTE_PARTITION` 三处都要写成 `anim_icon`。
3. 20 px 的 PuHuiCJK 字体需要 `CONFIG_LV_FONT_FMT_TXT_LARGE=y`。

分辨率能不能用是**量出来的**，不是猜的：`.eaf` 容器每帧头里有宽高（`emoji_large` 最大的动画
125×160，`listen` 那条 100×46），所以 240×240 放得下。解析方法见
`managed_components/espressif2022__esp_emote_gfx/src/lib/eaf/gfx_eaf_dec.c`：
帧数据指针是 `frame_mem + 2`，宽高在 +12/+14。

表情名（资源包里 20 个）：`happy laughing funny loving embarrassed confident delicious sad crying
sleepy silly angry surprised shocked thinking winking relaxed confused neutral idle`。
开机流程会先刷三色条，再把表情切成 `happy`。

### 传输完成回调是契约的一部分（这里踩过一次死锁）

渲染任务每刷一块就 `xEventGroupWaitBits(..., portMAX_DELAY)` 等一个"这块字节发走了"的应答，
等的时候**一直握着 gfx 递归锁**；`emote_lock()` 就是同一把锁。由此三条：

- 忘了在 `on_color_trans_done` 里调 `emote_notify_flush_finished()`：第一帧就卡死。
- 手动刷屏（`display_tft_fill()` / `display_tft_bars()`）也走同一个 panel io，就会多出
  不属于渲染器的应答。所以 `display_tft.c` 用一个 `s_emote_awaits_ack` 标志把两类事务分开，
  只把来自 `emote_flush()` 的那次算数；并且手动刷屏整段包在 `panel_lock()/panel_unlock()` 里，
  保证不会和渲染器的事务交叠。
- `esp_lcd_panel_draw_bitmap()` 万一返回失败，**必须就地补一个应答**：没有事务就没有回调，
  渲染任务会永远等在 `portMAX_DELAY` 上，而 `esp_http_server` 只有一个任务处理所有请求，
  于是整个 API 一起没了呼吸 —— 当时看到的现象是"刷完三色条之后所有 HTTP 都不回，但板子不崩、
  也没有 WDT 打印"。

验收脚本 `../emote_stress.py`（60 轮三色条/纯色/表情混打，每 5 轮探一次 `/api/status`）和
`../emote_soak.py`（复位后让表情连续跑 60 s，同时盯着串口）就是这条回归的保险。

## 关于板子自带热点（默认关闭）


代码里保留了一个 SoftAP 兜底通道，但 **默认编译掉**（`# CONFIG_WIFI_LED_AP_ENABLE is not set`）。
原因实测过一次，很坑：

> SoftAP 起来后，电脑一连上它，Windows 就把默认网关切到 `192.168.4.1`（板子），
> 而 `esp_wifi` 的 AP 只有二层转发 + 本地 DHCP，**不会 NAT 到它自己的 STA 上行**。
> 结果就是这台电脑所有流量灌进板子然后消失，整机断网 —— 只有断开热点才恢复。

判断依据是路由表：热点接口的 `0.0.0.0/0` 跃点是 0，有线网卡那个是 9999，必输。

要临时打开：`main/Kconfig.projbuild` 里把 `WIFI_LED_AP_ENABLE` 设 y（SSID/密码也在同一处），
然后 `rm sdkconfig && idf.py build`。

顺带纠正一个曾经的错误结论：这个光猫**有线口和无线口是通的**（同一个 `192.168.1.0/24`，
互相可以访问 80 端口）。之前以为"隔离"，是因为 `ping` 不通 —— 而 lwIP 默认根本不回应 ICMP。
**别用 ping 判断 ESP32 是否可达，直接 curl 它的 80 端口。**

## 编译烧录

Windows + Git Bash 下必须走 `idf_run.ps1`（直接跑 `idf.py` 会被 ESP-IDF 以
`MSys/Mingw is not supported` 拒绝）。脚本自己解析参数（`$args`），因为 PowerShell 给声明式
参数做位置绑定：`-Dir wifi_led build` 里的 `build` 会被吞进第二个 `[string]` 参数，
`idf.py` 收到空动作就只打印 usage 然后返回 0 —— 这就是"烧录成功"其实什么都没烧的成因。
端口两种写法都行：`-Port COM7`（内部转 `ESPPORT`）或原样透传 `idf.py` 的 `-p COM7`。

```bash
# 在仓库根目录
cmd //c "powershell -NoProfile -ExecutionPolicy Bypass -File idf_run.ps1 -Dir wifi_led build"
cmd //c "powershell -NoProfile -ExecutionPolicy Bypass -File idf_run.ps1 -Dir wifi_led -Port COM7 flash"

# 先确认串口真的在（只有 COM3/COM4 这种 BTHENUM 蓝牙口的话，说明数据线没插）
powershell -NoProfile -Command "[System.IO.Ports.SerialPort]::GetPortNames()"

# 复位并抓 18 秒开机日志
python capture_boot.py 18 emote_boot.txt
```

抓开机日志有两个坑，`capture_boot.py` 已经按实测绕过去了：

1. 这块板的自动复位电路要 **DTR 和 RTS 一起拉低**才把边沿送到 EN。单独脉冲 RTS 时脚本会
   以为自己在复位，其实芯片没动 —— 判据是日志时间戳有没有回到个位数，一直往上涨就是没复位。
2. 复位动作发生在**关闭端口**那一刻：带着 DTR+RTS 断言打开端口等于把芯片按在复位里（一个字都不发），
   关掉端口才松手，所以要"关闭 → 立刻重开 → 读"。

烧完的验收不是"命令返回 0"，而是问板子要一个只有新固件才有的字段：

```bash
python board_api.py status                # 看 screen_ready / emote_ready / led_ready / heap_internal
python board_api.py screen bars           # 三色条（接线自检）
python board_api.py screen 255 0 0        # 整屏纯色
python board_api.py screen emote happy    # 刷表情
python board_api.py led 230 160 20 6000
python emote_stress.py 60                 # 混打回归：三色条和表情交替，全程探活
```

别用"读到连接关闭为止"的写法（`curl` 不带 `-m`、python 裸 recv 到 EOF 都算），
`esp_http_server` 不会替你先关连接 —— 那正是历史上那条"只有刚开机能通"的假结论的来源。

非 Windows 或非 Git Bash 环境直接用原生 `idf.py set-target esp32s3 / build / flash` 就行。

## 配置分层

| 文件 | 内容 | 是否提交 |
| --- | --- | --- |
| `sdkconfig.defaults` | 硬件相关：16 MiB flash、Octal PSRAM、自定义分区表、灯脚 | 是 |
| `sdkconfig.local` | 路由器 SSID + 密码 | **否**（gitignore） |
| `sdkconfig.local.example` | 上面那份的模板 | 是 |
| `sdkconfig` | 编译时真正生效的合并结果 | 否（gitignore） |

两份 defaults 的合并由 `CMakeLists.txt` 里 `project()` 之前的
`set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.local")` 完成。
