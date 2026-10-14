# 公网隧道（演示用，只读，带看门狗）

```
访客 → https://xxx.trycloudflare.com → cloudflared → 127.0.0.1:8788 (monitor_proxy.py) → 板子
```

`monitor_proxy.py` 不是多余的中间层：板子的 HTTP 接口没有任何鉴权，
`POST /api/ota` 可以把 .bin 直接刷进备用分区、`POST /api/wifi` 能把板子指到别的 AP。
直连隧道等于把这两件事交给互联网上任何人，所以代理只放行
`/`、`/api/status`、`/api/led` 的 GET，其余 403/404。页面上的刷屏、表情、
OTA 按钮在隧道里会报失败，这是设计如此。

## 用法（Windows）

```powershell
# cloudflared 放用户目录，不动系统包
curl.exe -fsSL -o $env:USERPROFILE\.local\bin\cloudflared.exe `
  https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-windows-amd64.exe

powershell -ExecutionPolicy Bypass -File tools\tunnel\register_task.ps1   # 注册每分钟看门狗
powershell -ExecutionPolicy Bypass -File tools\tunnel\watchdog.ps1        # 手动跑一遍
notepad tools\tunnel\tunnel.url                                          # 当前 URL
powershell -ExecutionPolicy Bypass -File tools\tunnel\stop_tunnel.ps1     # 注销任务并停进程
```

`watchdog.ps1` 每次跑一遍就退出：代理没应答就重启，cloudflared 进程没了或公网 URL
取不到 200 就重启，并把结论写 `watchdog.log`。**Quick Tunnel 每次重启换域名**，
所以 `tunnel.url` 是唯一可信的当前地址，`tunnel_history.log` 留历史。

`--board 192.168.1.6` 改板子地址；`--allow-write` 才会把写接口透传给板子
（确认访客可信再用）。

## 为什么需要看门狗

代理和隧道是普通用户进程：agent 会话结束、进程被回收、机器重启都会带走它们，
而 Quick Tunnel 没有守护。计划任务 `Esp32MonitorTunnel` 每分钟检查一次，
实测杀掉两个进程后 15s 内自愈并换发新域名。

## 未验证 / 已知遗留

- `GET /api/status` 会把 SSID、MAC、局域网 IP 一起暴露给公网访客（不含口令，
  口令是 write-only，不进任何响应）。要公网隐藏这些信息得另做，别只靠截图。
- 计划任务的触发器是 `Once + 每分钟重复`。带 `-AtLogOn` 触发器 + `-RestartCount` 的
  注册在这台机器上报 Access Denied（`0x80070005`），没定位到具体是哪一个，
  所以现在没有登录触发器，重启后是否继续跑也没有实测；不放心就手动跑一次
  `watchdog.ps1`。
- 任务注册在 `tools/tunnel/` 的绝对路径上，这个目录删掉任务就失效。
- 访客侧浏览器渲染、跨境链路上 2s 轮询的稳定性没有实测，只有 curl 的证据。
- 固定域名（挂 ninesun.site 子域）需要 named tunnel + Cloudflare 账号授权，
  和这套临时方案是两条路。
