# Wi-Fi 配网（动态配置网络，取消硬编码凭据）

板子的目标网络不再只有"重编译重烧录"这一条路：手机 App（BLE）和板载网页
（`/api/wifi`）都能在读到周边网络之后，把新的 SSID/密码写进板子并立刻让它重新关联。

一句话结论：**凭据存在 flash 的 NVS 里，由固件自己读写；App 和网页都不保存密码。**

---

## 为什么是 BLE，不是热点门户

| 方案 | 结论 |
| --- | --- |
| 板子开 SoftAP + 门户页让人连上去改 | 现有事实：`esp_wifi` 的 AP 不 NAT 到 STA 上行，Windows 一连就把板子提成默认网关，接入方直接断网（见 ROADMAP"已知的坑"1）。所以 `WIFI_LED_AP_ENABLE` 默认关闭，门户页在"板子连不上现有网络"这个最需要它的场景里恰好也最难用。 |
| ESP-IDF 官方 `wifi_provisioning` 组件 | 协议是 security1/2（SRP + 自定义 protobuf），要和现有 GATT profile、现有 App 的 Nordic BleManager 重新对接一遍，收益主要是"配网通道自带加密"。这一条单独用 Kconfig 开关补上（见"安全"一节），不整体替换。 |
| **沿用现有 GATT profile 加一条特征值** | BLE 与 Wi-Fi 无关，板子连不上网也能配；App 已经有连接、读写、MTU 512 的一整套管线，加一条特征值即可。HTTP 侧把同一份逻辑再暴露一次，网页和脚本也能用。 |

选了第三条。凭据交换走的命令与 `/api/led` 同构：一个 JSON 一条命令。

## 凭据来源与优先级

单一来源是 `wifi_led/components/wifi_creds/`，其他组件都通过它取值：

1. **NVS**（namespace `wifi`，键 `ssid` / `pass`，落在 `partitions.csv` 的 16 KiB `nvs` 分区里）——配网写入的就是这里，优先级最高。
2. **编译期 Kconfig**（`CONFIG_WIFI_LED_WIFI_SSID` / `_PASSWORD`，实际值放 `sdkconfig.local`）——只有 NVS 里没有网络时才用，`CHANGE_ME` 和空串视为未填。
3. **都没有**：连接阶梯每 5 s 报一次 `no network configured`，灯一直是"连接中"的蓝色呼吸，BLE 照常广播，等配网。

保留第 2 层是有意的：板子被指到一个再也连不上、LAN 地址也进不去的网络时，
`sdkconfig.local` + 重烧是唯一不依赖任何通道的恢复路径。它同时也是一块垫子——
`forget` 之后板子回到编译期网络，不会出现"没有网络也没有凭据"的死局。

esp_wifi 自己仍是 `WIFI_STORAGE_RAM`：阶梯每一档都会重写配置（认证模式阈值不同），
不让驱动缓存参与；配网凭据在独立的 `wifi` namespace 里，驱动不会去读它。

## 协议

一份实现同时服务两个通道：`wifi_led/components/net_config/`
（BLE 特征值 `5d400008-…` 和 HTTP `/api/wifi` 都只是它的适配器）。

### 写命令

| 命令 | 含义 | 失败 |
| --- | --- | --- |
| `{"cmd":"scan"}` | 让板子后台做一次全信道扫描，结果从"读"里取 | 已有扫描在跑 → 拒绝 |
| `{"cmd":"set","ssid":"...","password":"..."}` | 存进 NVS 并立刻断开、按新网络重新走阶梯 | SSID/密码不合法、NVS 写失败 → 拒绝 |
| `{"cmd":"forget"}` | 删掉 NVS 里的网络，回到编译期配置 | NVS 操作失败 → 拒绝 |

校验规则（`wifi_creds_validate()`）：SSID 1–32 **字节**（中文 SSID 按 UTF-8 计数）；
密码要么空（开放网络），要么 8–64 字节。
`{"ssid":"...","password":"..."}` 不带 `cmd` 也接受，等价于 `cmd:"set"`（给手写 curl 和表单用的）。

### 读回来

```json
{"proto":1,"source":"stored","ssid":"MyRouter","configured":true,"has_password":true,
 "state":"online","scan":"done",
 "networks":[{"ssid":"MyRouter","rssi":-47,"auth":"WPA2_PSK","open":false}]}
```

| 字段 | 说明 |
| --- | --- |
| `proto` | 协议版本，目前 1。旧固件没有这条特征值 / 这个端点，App 据此隐藏配网卡。 |
| `source` | `stored` / `build` / `none`，即上面三档优先级里当前生效的那一档。 |
| `ssid` | 阶梯**正在连**的目标网络（不是"实际连上了哪个"——那个看 `/api/status` 的 `ssid`）。 |
| `state` | `connecting` / `linked` / `online`，下发之后就是靠它看有没有连上。 |
| `scan` | `idle` / `running` / `done` / `error`；`error` 时带 `scan_error`。 |
| `networks` | 只在 `scan:"done"` 时出现，按信号从强到弱排序，同名 AP（mesh/中继）合并成一条，最多 8 条。 |

**密码永远不出现在任何响应里**，读侧只有 `has_password` 这个布尔。

### 通道

- **BLE**：特征值 `5d400008-7a9b-4c8e-b2f4-38a1c6e0d951`，READ + WRITE，写响应模式。
  非法 JSON / 被拒绝的命令 → 应用错误 `0x81`。MTU 512 足够一条命令；读回带扫描结果时
  可能超过单包，NimBLE 走长读，客户端库自己拼。
- **HTTP**：`GET /api/wifi` 同上 JSON；`POST /api/wifi` 收命令，成功时直接回新的状态 JSON，
  失败时 `{"ok":false,"error":"..."}`（HTTP 400 走 `read_json_body` 的体积/JSON 校验）。
  请求体上限仍是 `POST_BODY_MAX` = 192 字节：32 字节 SSID + 64 字节密码的命令约 134 字节，够用。

### 时序：扫描是异步的

`{"cmd":"scan"}` 只是发起。扫描任务里 `esp_wifi_scan_start(sync=true)` 会阻塞几秒，
所以它跑在独立任务里，并且和连接阶梯自己的扫描共用一把 `s_scan_lock`——
**同一时刻只有一个扫描在跑**，撞上了就拒（不排队，客户端重试更简单）。

客户端的正确姿势：写 `scan` → 每 1.5 s 读一次 → `scan` 落到 `done`/`error` 就停。
App 侧上限 15 s（`WIFI_SCAN_TIMEOUT_MS`）。

`set` 之后板子会主动断开重连：正在等 IP 的分支被 `BIT_REJOIN` 唤醒，
正在"连上了"的分支同样被唤醒并 `esp_wifi_disconnect()`，下一轮循环重新加载凭据、
重新扫描、从阶梯第 0 档（WPA2 任意 BSS）开始。所以下发完不要立刻判定失败，
盯 `state` 和 `/api/status`，密码错会照旧打出 reason 204 那套诊断。

## 安全边界（要说清楚的几件事）

1. **BLE 这条通道默认是明文的**，和这个 profile 里其他特征值一样。射频范围内的设备可以
   读当前 SSID、可以写一个新网络进去。开启
   `CONFIG_WIFI_LED_BLE_PROV_ENCRYPT`（`main/Kconfig.projbuild`）后，写 0008 要求链路加密，
   手机必须先与板子配对；读仍然开放（只暴露 SSID）。默认关是因为现版本 App 没有主动发起
   配对的流程，直接打开会让没配对过的手机写失败——这是**已知取舍，不是已完成**。
2. **HTTP 无鉴权**，这是整个 monitor_server 的既有设计（LAN 内诊断设备）。`/api/wifi`
   把"能改网络"这件事也放进了这个前提里，比改灯严重，所以路由器的隔离就是它的边界；
   需要只留 BLE 的话，把 monitor_server 的这两条 URI 摘掉即可。
   要把这套接口经公网给人用（手机遥控），鉴权不在固件里，由 `tools/tunnel/monitor_proxy.py`
   的门禁代理承担：没解锁 key 时板子的任何路径都返回 401，`/api/wifi` 与 `/api/ota`
   只在 `--mode control` 且已解锁时透传。**这条边界是代理给的，不是板子给的**——
   直接暴露板子地址的做法依然等于把改网络和刷固件交给任何能连到它的人。
3. 密码不进日志：`wifi_creds` 只打印长度，`net_config` 只打印 SSID，
   `ESP_LOGI(TAG, "network set to \"%s\", re-associating", ssid)` 是有意不含凭据的。
4. App 不保存密码：不写 SharedPreferences、不进文件；下发成功即清空输入框
   （转屏时系统会保存 EditText 内容，清空是让它少活一轮）。

## 验证方法

**本机已验证**：ESP-IDF v5.3.5 全量编译（`-Wall -Werror`）通过，明文与
`WIFI_LED_BLE_PROV_ENCRYPT=y` 两条分支都编过；`board_api.py` / `ble_gatt_test.py` 语法检查通过；
App 侧只做静态核对（布局 id、字符串资源、ViewBinding 引用都对得上）——**这台机器没有 Android SDK，
App 没编译过也没装过**。

**未验证，需要你在硬件上做**：

1. PC 端先看协议通不通（板子已联网时）：
   ```bash
   python board_api.py wifi                       # 读配置与最近一次扫描
   python board_api.py wifi scan                  # 发起扫描
   python board_api.py wifi scan                  # 1–2 秒后再读，看 networks
   ```
   命令行传密码会留在 shell 历史里，只用于一次性验证；日常配网用 App。
2. BLE 端不依赖手机：`tools/ble_gatt_test.py` 从 7 项涨到 11 项，新增的都是这条特征值的
   （读回能解析、扫描会收敛、响应里不含密码、非法命令被 0x81 拒——用的假 SSID
   `ble-test`，校验在写 NVS 之前就失败，不会碰到你的真网络）。板子在广播时用仓库 venv 跑它。
3. 手机 App：连接 → 配网卡显示"当前：…" → 扫描网络 → 选一条 → 填密码 → 下发到板子
   → 十几秒内"板子状态"应变成 linked/online，路由器上能看到新关联。
4. **换网络的失败路径也要测**：故意填错密码，确认灯回到蓝色呼吸、串口打出
   `4-way handshake timeout - wrong password`，然后重新下发正确密码——
   这条是配网真正 risky 的地方（改完连不上还能不能救回来）。
5. 断电重启，确认 `source` 仍是 `stored` 且自动连上——NVS 持久化的验收点。
6. 兜底路径：下发 `{"cmd":"forget"}`，确认板子回到编译期网络（`source` 变 `build`）。
   板子已经在 `source:"none"` 状态下时，它只会持续报 `no network configured`，
   这时用 App 或 `board_api.py wifi set` 配一条即可，不需要重烧。
