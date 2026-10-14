# 手机遥控 / 公网隧道（带看门狗）

```
手机浏览器 → https://<域名> → cloudflared → 127.0.0.1:8788 (monitor_proxy.py) → 板子 192.168.1.6
```

## 为什么中间必须有代理

板子的 HTTP 接口没有任何鉴权：`POST /api/ota` 能把 .bin 直接刷进备用分区，
`POST /api/wifi` 能把板子指到别的 AP，`GET /api/status` 还会吐 SSID、MAC、局域网 IP。
公网直连板子等于把这些交给互联网上任何人，所以 `monitor_proxy.py` 做两件事：

1. **解锁门禁**：除 `/remote`、`/unlock` 外的所有路径都要求 cookie，没解锁一律 401
   （浏览器直接访问会拿到解锁表单）。密钥在 `control.key`，第一次启动自动生成，
   不进仓库、不进 issue。
2. **模式**：`--mode readonly` 只放行 GET（灯带/刷屏/OTA 按钮全部 403）；
   `--mode control` 解锁后放行 `/api/led`、`/api/screen`、`/api/wifi`、`/api/ota` 的 POST。

`/remote` 是给手机用的遥控页：大号按钮、灯带效果与颜色亮度、15 个表情、整屏纯色，
调用的是板子 `web_index.h` 里同一套 JSON 协议，不新增任何字段。

## 用法（Windows）

```powershell
curl.exe -fsSL -o $env:USERPROFILE\.local\bin\cloudflared.exe `
  https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-windows-amd64.exe

# 1) 注册每分钟看门狗（只起代理+隧道，域名要另做，见下）
powershell -ExecutionPolicy Bypass -File tools\tunnel\register_task.ps1

# 2) 当前可访问地址
type tools\tunnel\tunnel.url        # 手机打开 <URL>/unlock 输入 tools\tunnel\control.key 里的密钥

# 3) 关掉：注销任务 + 停两个进程
powershell -ExecutionPolicy Bypass -File tools\tunnel\stop_tunnel.ps1
```

`proxy.mode` 文件内容 `readonly` / `control` 决定看门狗拉起代理的模式，缺省 `control`。
`watchdog.ps1` 每次跑一遍就退出：代理没应答就重启，cloudflared 进程没了或公网 URL
的 `/remote` 取不到 200 就重启，结论写 `watchdog.log`。**Quick Tunnel 每次重启换域名**，
所以 `tunnel.url` 才是唯一可信的当前地址，`tunnel_history.log` 留历史。

## 固定域名（ninesun.site）

临时域名的毛病是每次自愈都换名字，手机收藏不了。要 `esp32.ninesun.site` 这种固定入口，
需要 Cloudflare 账号侧的动作，`config.named.yml` 是本仓库这一侧的接线（ingress 只放行该
域名到 127.0.0.1:8788）：

1. Zero Trust → Networks → Tunnels 建 tunnel，`cloudflared tunnel route dns` 或面板里
   加 DNS 记录 `esp32.ninesun.site` → 该 tunnel。
2. 把 tunnel token 交给**在本机跑 cloudflared 的人**，写进本地文件，不写进仓库和 issue；
   看门狗换成 `cloudflared tunnel run --config config.named.yml` 或 `--token`。
3. 推荐再在 Cloudflare Access 上给这条域名加一层邮箱/策略鉴权：这样即使 `control.key`
   泄露，公网请求也过不了 Access。密钥门禁和 Access 是两层，不是重复。

这台机器上没有 Cloudflare 凭证（无 `~/.cloudflared`、无相关环境变量），所以上面 1、2
不在本仓库能完成的范围里。

## 已实测

- `GET /remote` 200；未解锁 `GET /` 401、`POST /api/led` 401、`POST /api/ota` 401。
- 错误密钥 `POST /unlock` 401；正确密钥 302 并下发 cookie。
- 解锁后：`POST /api/led` 回显 `{"manual":true,"fx":"breathe","rgb":[38,198,218],"brightness":30}`，
  `POST /api/screen {"emote":…}` 返回 `{"ok":true}`，实测灯带与屏幕变化，测后已恢复
  `manual:false` + `idle`。
- 杀掉代理后看门狗 15~60s 内自愈（计划任务 `Esp32MonitorTunnel`，退出码 0，无实例堆积）。
- `POST /` 等不在白名单的写方法 405；`/api/led` 之外的路径 404。

## 未验证 / 遗留

- **OTA 只验证了「未解锁会被 401 挡住」，没有真的推固件**（拿真 .bin 走公网刷备用分区
  风险太大，需要你在板子旁边、且明确要升级时再做）。
- 手机浏览器上的 `/remote` 实际触控与渲染效果没测（我只跑了 curl 与协议级验证）。
- cookie 里放的是密钥本身（`HttpOnly; SameSite=Lax; Secure`，30 天）。想换更短的会话或
  每次输入，改成服务端存 session id 是几行代码。
- 没有 `-AtLogOn` 触发器：带它的注册在这台机器上报 `0x80070005` Access Denied，
  所以机器重启后是否继续跑没有实测，不放心就手动 `watchdog.ps1` 一遍。
- 任务注册在 `tools/tunnel/` 的绝对路径上，目录删掉任务就失效。
- 局域网内、手机连同一个 WiFi 时不需要这一整套：`http://192.168.1.6` 直接就是完整功能。
