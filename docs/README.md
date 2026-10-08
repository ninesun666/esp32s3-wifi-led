# 参考文档

## 乐鑫官方手册（中文 PDF）

从 espressif.com 官方文档页下载，供离线查阅。选型原则见根目录 CLAUDE.md——查问题先翻官方手册。

| 文件 | 内容 | 什么时候查 |
| --- | --- | --- |
| `esp32-s3_datasheet_cn.pdf` | 芯片技术规格书 v2.2（83 页）：第 2 章管脚（IO MUX/RTC/模拟功能、电源、Flash/PSRAM 占用）、第 3 章 Strapping、电气特性 | 引脚分配、上下电、启动模式 |
| `esp32-s3-wroom-1_wroom-1u_datasheet_cn.pdf` | ESP32-S3-WROOM-1/-1U 模组规格书：模组管脚、射频指标、外形封装 | 本仓库的 WROOM-1-N16R8 模组相关 |
| `esp32-s3_technical_reference_manual_cn.pdf` | 技术参考手册（完整版）：全部外设的寄存器级描述，「IO MUX 和 GPIO 交换矩阵」章节在此 | 深挖某个外设行为时 |

来源（2026-10-08 下载）：
<https://www.espressif.com/documentation/esp32-s3_datasheet_cn.pdf>
<https://www.espressif.com/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_cn.pdf>
<https://www.espressif.com/documentation/esp32-s3_technical_reference_manual_cn.pdf>

芯片规格书里引脚问题的速查表：表 2-4（IO MUX 功能）、表 2-6（RTC 功能）、表 2-8（模拟功能）、表 2-14（Flash/PSRAM 占用）、表 3-3/3-4/3-5（Strapping）。

## Meta Muse Gadget SDK · esp32/AGENTS.md

`reference/muse-gadget-sdk-esp32-AGENTS.md`——Meta 开源 Muse 硬件配件 SDK 的 AI 编码指南原样副本（Apache 2.0）。自家项目写 AGENTS.md/CLAUDE.md 时值得借鉴的点：

- **按"构建→烧录→监控"分节**，每步给出可直接执行的命令和失败处置（如进不了 bootloader 的按键序列）
- **"Identify the board first"**：要求 Agent 先通过 USB 描述符/esptool 探测硬件再动手，不要问用户也不要猜
- **不阻塞原则**：命令处理函数里禁慢操作，异步任务要拷贝参数、错误也要回结果
- **配置陷阱清单**：生成的 sdkconfig 覆盖 defaults 的坑、分区偏移不许动、不要提交生成的 sdkconfig
- **收尾验收清单**：编译过 + size check + host 测试过 + 启动日志无 panic 才算完成

原文以 ESP-IDF v6.0.1 为准，与本仓库 v5.3.5 不同，勿照抄命令。
