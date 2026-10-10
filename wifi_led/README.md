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
| RGB 灯 | GPIO 48 上的 WS2812，物理确认会亮，串口打出 `rgb=(0,120,120)` |
| 网页 | `http://<板子IP>/` 返回 200（实测 5789 B），2 s 轮询 `/api/status`；`POST /api/led` 可控灯 |
| 分区 | OTA 布局生效：`ota_data_initial.bin` 烧在 `0xd000`，app 落在 `ota_0`（4 MiB 槽用了 22%） |

> **未解决**：网页只在刚开机的一小段时间内响应，之后 TCP 能连上但服务端不回数据，再久一点直接超时。
> 已排除固件崩溃（请求失败时串口没有 panic / 重启，灯仍按状态机刷新），当前最大嫌疑是掉线重连后被路由器换了 IP。

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
  外网连通、热点状态、接入设备列表，最近 60 次采样的信号强度柱状图，外加一排手动点灯按钮。
- `/api/status` —— 同样内容的 JSON，由 cJSON 现取现算（`esp_wifi_sta_get_ap_info()` + 堆统计），
  不做缓存，所以页面不会和驱动的实际状态漂移。
- `/api/led` —— `POST`，`{"r":0-255,"g":0-255,"b":0-255,"hold_ms":0-600000}`，
  覆盖链路颜色若干毫秒后自动交还状态机；`hold_ms:0` 立刻交还。这是留给外部大脑
  （Home Assistant / 米家场景 / curl）控制的入口，见 [`../ROADMAP.md`](../ROADMAP.md)。
  **无鉴权**，只应在局域网内暴露。
- mDNS —— 板子应答 `http://<WIFI_LED_MDNS_HOSTNAME>.local/`（默认 `esp32-monitor`），
  不用记住 DHCP 地址。个别 Windows 解析不了 `.local`，退回 IP 即可。
- `i2c` 字段 / 页面"I2C 总线"卡片 —— `i2c_probe` 组件在 GPIO 8(SDA)/9(SCL) 上扫 0x08–0x77，
  **同一个地址连续两次应答才算数**（空总线会浮出随机的假 ACK，实测过 0x0b 这种幽灵地址）。
  没扫到就每 5 秒重扫，所以外接模块可以随时插上看网页，不用重新烧录、也不用开串口。

页面标记在 `components/monitor_server/web_index.h`，用 C11 原始字符串字面量编译进固件，
不占 SPIFFS 分区；`monitor_server.c` 里只有路由和 JSON，没有 HTML。

## 工程结构

```
main/                 app_main.c 只做接线：link_state_init / status_light_start / i2c_probe_start / monitor_server_start
components/
  link_state/         Wi-Fi 上线、安全模式重试阶梯、外网探测、状态与 IP 发布
  status_light/       WS2812 渲染 + 手动覆盖（依赖 espressif/led_strip）
  i2c_probe/          I2C 总线看门狗，识别外接模块是否应答（用 IDF 自带 esp_driver_i2c）
  monitor_server/     HTTP 页面与 API + mDNS（依赖 espressif/mdns）
partitions.csv        nvs / otadata / phy_init / ota_0 / ota_1 / storage，给 OTA 留好了位
```

组件之间的依赖方向是单向的：`monitor_server` 和 `status_light` 都只读 `link_state`，
`i2c_probe` 谁也不读，`link_state` 不知道上面几位存在。加新功能时按这个方向放，不要出现回指。

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

# 抓 90 秒串口日志（打开端口本身会让板子正常重启，所以连开机日志一起能抓到）
python grab_log.py 90
```

烧完的验收不是"命令返回 0"，而是 `curl http://<板子IP>/api/status` 里出现
`led_ready` 和 `host` 两个字段、`POST /api/led` 回 200。

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
