# xiaozhi-web — 小智服务端（P0 暂存区）

这里是 [`../ROADMAP.md`](../ROADMAP.md) 里 P0 那一格的服务端启动文件，先跟着固件仓库一起放，
等 P1（智控台 + MySQL + Redis）落地时再拆成独立仓库。

```bash
docker compose up -d            # 单容器最简化模式，8000=ws，8003=OTA/vision
```

**跑不起来是预期的**：`data/.config.yaml` 需要一份云端 LLM/ASR 的 key（阿里百炼 / 硅基流动 /
任何 OpenAI 兼容端点），凭据不进仓库，所以这个文件还没生成。镜像里自带的
`data/.config.yaml` 缺失时容器会拒绝启动，这是设计好的。

浏览器语音客户端在源码仓库 `main/digital-human/`，起在 `http://127.0.0.1:8006/index.html`，
它本身就是"没有硬件时的假设备"，P4 之前先用它验证整条链路。
