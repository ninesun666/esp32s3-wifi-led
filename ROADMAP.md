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

## 接力第一步：把新固件烧进去（2026-10-08 00:10 的状态）

代码重构已完成、编译产物是对的，但**板子上跑的仍是重构前的旧固件**。证据（LAN 实测）：

- `GET http://<板子IP>/api/status` 返回的 JSON 里**没有** `led_ready` 和 `host` 两个字段；
- `POST /api/led` 回 `404 Nothing matches the given URI`；
- 而 `wifi_led/build/wifi_led.bin`（909,136 B）里 `strings` 能搜到 3 处 `/api/led` 和 `esp32-monitor`。

原因不是编译，是**烧录根本没执行到这块板子**：

```
powershell -File idf_run.ps1 -Dir wifi_led -p COM7 flash
→ 脚本收到的剩余参数只有 "flash"
```

`-p COM7` 被 PowerShell 自己吞掉了（未知短选项进不了 `ValueFromRemainingArguments`），
idf.py 没拿到端口就去自动探测，探到的是蓝牙虚拟串口（这台机器上只有 COM3/COM4，
全是 `BTHENUM\{00001101-...}`，CH343 根本不在设备树里 —— 板子的 USB 数据线没插，或那根线只供电）。

所以接手的人照这个顺序做：

1. 插好 USB 数据线，`[System.IO.Ports.SerialPort]::GetPortNames()` 里要出现 CH343 那个口。
2. `cmd //c "powershell -NoProfile -ExecutionPolicy Bypass -File idf_run.ps1 -Dir wifi_led -Port COM7 flash"`
   —— 端口必须走 `-Port`（内部转成 `ESPPORT` 环境变量，见 idf.py `serial_ext.py`），**不要写 `-p COM7`**。
3. 验收：`/api/status` 出现 `led_ready` 与 `host`，`POST /api/led` 回 200 且灯真的变色，
   串口日志里有 `mDNS: http://esp32-monitor.local:80/`。
4. 烧完再复查一次 OTA：`idf.py partition-table` 应列出 `otadata` + `ota_0` + `ota_1`。

`-Port` 参数已经加进 `idf_run.ps1`；旧代码是 `Set-Location $Dir; idf.py @IdfArgs`。

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

## 结构约定（本次改造立的，后续照这个长）

```
wifi_led/
  main/              只做接线：app_main.c 20 行 + Kconfig.projbuild
  components/
    link_state/      Wi-Fi 上线、安全模式重试阶梯、外网探测、状态发布
    status_light/    WS2812 渲染 + 手动覆盖接口
    monitor_server/  HTTP 页面与 API、mDNS
  partitions.csv     otadata + 双 app 槽，给 OTA 留位
  sdkconfig.defaults 硬件事实（提交）
  sdkconfig.local    凭据（不提交）
```

依赖不自己造：`espressif/led_strip` 2.5.5、`espressif/mdns` 1.14.0、IDF 自带 `json`/`esp_http_server`。
