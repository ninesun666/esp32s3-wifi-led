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
