# wifi_led — Wi-Fi 状态灯 + 网页监控

用一颗 WS2812 把联网状态显示出来，同时在板子上跑一个 HTTP 服务，浏览器直接看实时状态。
这是这个沙箱里第一个跑通的项目，后面的项目可以拿它当底座。

## 当前状态（2026-10-07 实测）

| 项 | 结果 |
| --- | --- |
| Wi-Fi | 已连上家里路由，`esp_wifi_connect()` 后约 1.4 s 拿到 `192.168.1.6` |
| 外网 | TCP 探测 `114.114.114.114:53` 通过 |
| PSRAM | 8 MiB Octal @ 80 MHz，bootloader 自检 OK |
| Flash | 16 MiB |
| RGB 灯 | GPIO 48 上的 WS2812，物理确认会亮 |
| 网页 | `http://<板子IP>/` 返回 200，2 s 轮询 `/api/status` |

> 2026-10-08 起支持网页接管灯带，见下文「网页接管灯带」。
> 同日 BLE 上板实测（Mac 原生 USB 烧录）：广播、连接（MTU 512）、LED 写入/读回、
> 状态读取、非法写 0x81、断开重广播——7 项检查全过；SoftAP+BLE 同开时
> 开机内部堆余 345 KiB（BLE 连接中降到约 183 KiB）。

## 灯语

| 颜色 | 含义 |
| --- | --- |
| 蓝色呼吸 | 还没关联上路由器 |
| 常亮绿 | 连上路由器，但外网探测不通 |
| 常亮青（绿+蓝） | 路由器 + 外网都通 |
| 常亮紫（红+蓝） | 有设备接入了板子自己的热点（只在热点开启时可能出现） |

状态由 Wi-Fi/IP 事件回调写、由独立的 `led_task` 每 20 ms 刷一次。连接和外网探测都会阻塞，
所以不能在主循环里点灯。

## 网页接管灯带

状态灯语可以被网页临时接管：`/api/led`（GET 读当前设定，POST 下发），
POST 的 JSON 形如 `{"manual":true,"fx":"rainbow","rgb":[255,80,0],"brightness":60}`，
`fx` 取 `solid / breathe / rainbow / off`。接管期间灯效完全由这份设定决定，
不再反映链路状态；点页面上的「恢复自动」（或 POST `manual:false`）交还状态机。
设定只存在 RAM 里，重启后回到自动状态色。控件在监控页底部的「灯带控制」卡片：
取色器、亮度滑条、四个灯效按钮。共享设定由互斥锁保护，`led_task` 是灯的唯一写手。

## BLE 控制（手机 nRF Connect）

ESP32-S3 只有 BLE（没有经典蓝牙），手机系统蓝牙设置里搜不到，要用 nRF Connect 这类
扫描器。固件起一个 NimBLE GATT 服务器（`start_ble()`，在 Wi-Fi 完全启动之后初始化，
顺序照抄官方 Wi-Fi/BLE 共存示例；两个射频的空口分时由 IDF 共存层自动处理，零配置）。
广播名 `ESP32-LED`，改 `main/Kconfig.projbuild` 里的 `EXAMPLE_BLE_NAME`。

| 特征值（服务 `5d400001-7a9b-4c8e-b2f4-38a1c6e0d951`） | 权限 | 载荷 |
| --- | --- | --- |
| `…0002` LED | 读 / 写 | 与 `/api/led` 同款 JSON |
| `…0003` 状态 | 只读 | 与 `/api/status` 同字段，去掉热点客户端列表 |

写 `{"manual":true,"fx":"rainbow","rgb":[255,80,0],"brightness":60}` 到 0002，灯带立刻
变彩虹，读回当前生效设定；写非法值（如 `{"fx":"disco"}`）收到应用层错误 0x81。
读 0003 拿到 `{"state":"online","ssid":"…","ip":"…","rssi":…,…}` 现取现算。
BLE 和网页共用同一条 `led_ctl_from_json()` / `build_status_json()` 路径——两边同时操作，
看到的是同一份设定。

- 单连接（`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`）：连上后暂停广播，断开自动恢复。
- 状态 JSON 约 330 字节：协商到 512 MTU 的手机一次读走；MTU 较小的（iPhone 约 185）
  自动走 BLE 长读，NimBLE 服务器自己处理，客户端不用做任何事。
- 特征值明文、不配对：射频范围内任何人都能控灯——演示用途的取舍；
  以后要加密，给特征值加 `BLE_GATT_CHR_F_WRITE_ENC` 是小改。
- 回归测试不用手机：仓库根目录 `python tools/ble_gatt_test.py`（先 `pip install bleak`），
  直连开发板跑完上面全部路径。

## 网页监控

- `/` —— 深色面板：连接状态、IP、SSID/加密方式、RSSI、信道、运行时长、内部堆与 PSRAM 空闲、
  外网连通、热点状态、接入设备列表，外加最近 60 次采样的信号强度柱状图。
- `/api/status` —— 同样内容的 JSON，字段现取现算（`esp_wifi_sta_get_ap_info()` + 堆统计），
  不做缓存，所以页面不会和驱动的实际状态漂移。

嵌入的 HTML 用 C11 原始字符串字面量写在 `main/wifi_led_main.c` 里，不占 flash 分区以外的资源。

## 关于板子自带热点（默认关闭）

代码里保留了一个 SoftAP 兜底通道，但 **默认编译掉**（`# CONFIG_EXAMPLE_AP_ENABLE is not set`）。
原因实测过一次，很坑：

> SoftAP 起来后，电脑一连上它，Windows 就把默认网关切到 `192.168.4.1`（板子），
> 而 `esp_wifi` 的 AP 只有二层转发 + 本地 DHCP，**不会 NAT 到它自己的 STA 上行**。
> 结果就是这台电脑所有流量灌进板子然后消失，整机断网 —— 只有断开热点才恢复。

判断依据是路由表：热点接口的 `0.0.0.0/0` 跃点是 0，有线网卡那个是 9999，必输。

要临时打开：`main/Kconfig.projbuild` 里把 `EXAMPLE_AP_ENABLE` 设 y（SSID/密码也在同一处），
然后 `rm sdkconfig && idf.py build`。

顺带纠正一个曾经的错误结论：这个光猫**有线口和无线口是通的**（同一个 `192.168.1.0/24`，
互相可以访问 80 端口）。之前以为"隔离"，是因为 `ping` 不通 —— 而 lwIP 默认根本不回应 ICMP。
**别用 ping 判断 ESP32 是否可达，直接 curl 它的 80 端口。**

## 编译烧录

Windows + Git Bash 下必须走 `idf_run.ps1`（直接跑 `idf.py` 会被 ESP-IDF 以
`MSys/Mingw is not supported` 拒绝）。参数叫 `-Dir` 而不是 `-ProjectDir`，
因为 `idf.py` 自己的 `-p` 会被 PowerShell 前缀匹配到 `-ProjectDir` 上。

```bash
# 在仓库根目录
cmd //c "powershell -NoProfile -ExecutionPolicy Bypass -File idf_run.ps1 -Dir wifi_led build"
cmd //c "powershell -NoProfile -ExecutionPolicy Bypass -File idf_run.ps1 -Dir wifi_led -p COM7 flash"

# 抓 90 秒串口日志（打开端口本身会让板子正常重启，所以连开机日志一起能抓到）
python grab_log.py 90
```

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

BLE 的开关和广播名在 `main/Kconfig.projbuild`（`EXAMPLE_BLE_ENABLE` / `EXAMPLE_BLE_NAME`），
NimBLE 细节（MTU 512、主机内存池放 PSRAM、单连接、裁掉不用角色/服务）在 `sdkconfig.defaults`。
改这些同样要 `rm sdkconfig` 再 build。
