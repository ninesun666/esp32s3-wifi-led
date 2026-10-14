# 公网隧道（演示用，只读）

把板子的监控页临时暴露到公网给人看，两个进程：

```
访客 → https://xxx.trycloudflare.com → cloudflared → 127.0.0.1:8788 (monitor_proxy.py) → 板子
```

`monitor_proxy.py` 不是多余的中间层：板子的 HTTP 接口没有任何鉴权，
`POST /api/ota` 可以直接刷进备用分区、`POST /api/wifi` 可以把板子指到别的 AP。
直连隧道等于把这两件事交给互联网上任何人，所以代理只放行
`/`、`/api/status`、`/api/led` 的 GET，其余 403/404。页面上的刷屏、表情、
OTA 按钮在隧道里会报失败，这是设计如此。

## 用法（Windows）

```powershell
# cloudflared 放用户目录，不动系统包
curl.exe -fsSL -o $env:USERPROFILE\.local\bin\cloudflared.exe `
  https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-windows-amd64.exe

powershell -ExecutionPolicy Bypass -File tools\tunnel\start_proxy.ps1   # 打印 proxy_pid
powershell -ExecutionPolicy Bypass -File tools\tunnel\start_tunnel.ps1  # 打印 URL，写 tunnel.url
powershell -ExecutionPolicy Bypass -File tools\tunnel\stop_tunnel.ps1   # 按 pid 文件收尾
```

`--board 192.168.1.6` 改板子地址；`--allow-write` 才会把写接口透传给板子
（确认访客可信再用）。Quick Tunnel 的域名随机、进程一停即失效、无 SLA，
固定域名要 named tunnel + 账号授权。

## 未验证 / 已知遗留

- `GET /api/status` 会把 SSID、MAC、局域网 IP 一起暴露出去（不含口令）。
  要在公网隐藏这些信息，就在这个隧道之外做，或只发截图。
- 凭据不经这条链路；`sdkconfig.local` 保持不进仓库。
- 隧道进程由本机用户会话拉起，机器重启或进程被杀即断，没有守护。
