# ESP32-S3 沙箱

个人练手仓库：一块 ESP32-S3-WROOM-1-N16R8 开发板 + 已经跑通的 Wi-Fi 状态灯 / 网页监控项目。
代码按 ESP-IDF 工程组织，可以直接拿去做新项目的底座。

| 目录 | 说明 |
| --- | --- |
| [`wifi_led/`](wifi_led/README.md) | 主项目：Wi-Fi 状态灯 + HTTP 监控页 |
| `idf_run.ps1` | Windows + Git Bash 下唯一能跑通 `idf.py` 的入口 |
| `grab_log.py` | 抓 COM7 串口日志（含开机日志） |
| [`docs/`](docs/README.md) | 乐鑫官方手册（芯片规格书 v2.2、WROOM-1 模组手册、TRM，中文 PDF） |

## 硬件事实

- 模块 **ESP32-S3-WROOM-1-N16R8**：16 MiB flash + 8 MiB **Octal（八线）** PSRAM，双核 LX7 @240 MHz。
- PSRAM 是八线的，固件必须 `CONFIG_SPIRAM_MODE_OCT=y`；按四线配会开机但 PSRAM 初始化失败。
- 八线 PSRAM 吃掉 **GPIO 26–37**，但排针照样引出来了 —— 接线避开这一段。
- 可用引脚：0–21、38–48；另避开 19/20（原生 USB）、43/44（UART）、0/3（strapping）。
- 板载 WS2812 在 **GPIO 48**（已物理确认）。串口桥是 CH343，Windows 上枚举为 COM7。
- ESP32-S3 **只有 BLE，没有经典蓝牙**，手机系统蓝牙设置里搜不到它，要用 nRF Connect。

## 换台电脑怎么跑起来

1. 装 ESP-IDF **v5.3.5**（离线安装包 `esp-idf-tools-setup-offline-5.3.5.exe`，
   下载地址 <https://dl.espressif.com/dl/idf-installer/>；注意不是 `/dl/esp-idf/`，那个 404）。
2. 克隆本仓库，进入 `wifi_led/`。
3. 造一份本地凭据文件（这个文件被 `.gitignore` 掉了，仓库里不含任何真实密码）：

   ```bash
   cp sdkconfig.local.example sdkconfig.local
   # 编辑 sdkconfig.local，填你自己路由器的 SSID 和密码
   ```

4. 激活环境后编译烧录：

   ```bash
   . $IDF_PATH/export.sh          # 或 Windows 上运行起 ESP-IDF 的终端
   idf.py set-target esp32s3
   idf.py build
   idf.py -p COM7 flash           # 端口按实际改
   idf.py -p COM7 monitor
   ```

5. 刷机后看串口里的 `got IP: 192.168.x.x -> http://192.168.x.x/`，浏览器直接打开就是这个页面。

> `sdkconfig` 是编译时真正生效的那份，`sdkconfig.defaults` + `sdkconfig.local` 只在
> `sdkconfig` 不存在时被读取。改完这两个文件要 `rm sdkconfig` 再 build，否则不生效。

## 不要提交的东西

`build/`、`sdkconfig`、`sdkconfig.local`、`managed_components/`、`ap_profile.xml` 全在
`.gitignore` 里。路由器密码只存在于本地 `sdkconfig.local`，别写进日志、提交信息或文档。
