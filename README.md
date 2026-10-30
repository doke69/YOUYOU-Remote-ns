# YOUYOU远程 · Nintendo Switch

**在 Nintendo Switch 上远程操作自己的 Windows 电脑。**

基于 C++17 / libnx 的非官方自制客户端，使用 UU 远程账号连接电脑，接收画面和声音，并通过手柄、键鼠映射或触屏进行操作。项目基于 [Antman2023/UURemote-ns](https://github.com/Antman2023/UURemote-ns)，与网易、任天堂无隶属或背书关系。

![Platform](https://img.shields.io/badge/platform-Nintendo%20Switch-428580)
![Version](https://img.shields.io/badge/version-0.3.74%20Beta-d99a36)
[![License](https://img.shields.io/badge/license-GPL--3.0--or--later-blue)](LICENSE)

[安装使用](#安装使用) · [按键操作](#按键操作) · [当前限制](#当前限制) · [源码构建](#源码构建) · [文档导航](docs/README.md) · [更新记录](CHANGELOG.md)

> **当前为 0.3.74 Beta。** 安全页增加“屏幕保护”子页，整理“隐私保护”与“关闭显示屏”。已有 Switch 真机使用和测试反馈；新页面布局、音频、弱网、断连恢复、各机型及长时间运行仍需进一步验证。源码仓库不包含 APK、独立签名文件或用户会话；测试版 NRO 与 SHA256SUMS 可从 Releases 下载。

## 功能与验证状态

当前源码包含：

- **账号与设备**：扫码或短信登录、本地会话恢复、手动刷新电脑列表，并记住上次成功连接的电脑。
- **画面与声音**：H.264 / H.265 接收、Opus 音频、NVDEC 硬解和 Deko3D NV12 显示路径；不可用时存在回退路径。
- **画质与网络**：省流、清晰、高清、原画主机预设，以及 30 / 50 / 75 / 100 Mbps 自定义码率上限；网络页可查看连接通道、实际接收码率和诊断信息。
- **操作方式**：两套键鼠映射、实验性 Xbox 手柄直连、手机式鼠标触屏与平板多点触控。
- **桌面操作**：Windows 快捷操作、剪贴板确认粘贴及部分安全设置。
- **屏幕保护**：安全页增加“屏幕保护”子页，集中放置“隐私保护”和“关闭显示屏”；两种模式互斥，并区分电脑已确认、等待和未确认。

| 验证项目 | 当前结果 |
| --- | --- |
| 0.3.74 Beta Switch 编译与 NRO 打包 | 维护者完成本地 Switch 构建；Release 提供测试 NRO 与 SHA256SUMS |
| 主机侧测试 | 23/23 通过；不能替代 Switch 真机回归 |
| 720p / H.265 串流 | 已有真机使用反馈；实际表现随设备、场景和网络变化 |
| 自定义码率与 REMB | 可请求上限并发送反馈；尚无同场景反馈开关对照，不能保证主机采用或达到上限 |
| 音频缓冲与缺包补偿 | 已实现；音画同步、弱网和长时间表现待专项验证 |
| TURN TCP/TLS、原生 IPv6、完整无重连调节 | 未完成或尚无充分互通验证 |

默认配置为 **720p / 60 FPS、H.265、高清主机预设、垂直同步开启、8 ms 播放缓冲、键鼠方案一**。自定义码率是上限而非固定输出；程序不会自动超频。详见[项目状态](docs/project-status.md)与[本版说明](docs/releases/v0.3.74-beta.md)。

## 安装使用

准备可运行自制程序的 Switch、安装官方 UU远程并登录的电脑，以及用于扫码的手机。只连接你有权控制的电脑。

1. 从 [Releases](https://github.com/doke69/YOUYOU-Remote-ns/releases) 下载测试 NRO，或按[源码构建说明](docs/build.md)自行构建，放入 SD 卡的 `switch/UURemote-ns/` 目录。更新时只替换程序文件，保留配置与账号数据。
2. 在电脑上打开官方 UU远程，登录并保持在线。
3. 在 Switch 使用 **完整应用模式**进入 sphaira / hbmenu，例如按住 **R** 启动游戏，再打开 YOUYOU远程；不要以相册小程序模式比较性能。
4. 使用手机官方 App 扫码确认，或选择短信登录。已有有效会话时程序会尝试恢复登录。
5. 刷新并选择在线电脑，进入桌面。设备身份可能显示为 TV，设备名为 Switch 主控端；本项目并非官方 Switch 客户端。

```text
SD 卡根目录/
└── switch/
    └── UURemote-ns/
        └── UURemote-ns.nro
```

完整流程、配置和故障排查见[使用手册](docs/user-guide.md)。

## 按键操作

| 场景 | 按键 | 行为 |
| --- | --- | --- |
| 首页 | 长按 **＋** | 退出应用；从加载器启动时通常返回加载器 |
| 串流 | 长按 **−** | 切换键鼠映射与手柄直连 |
| 串流 · 键鼠模式 | 按一下 **＋** | 打开或收起本地菜单 |
| 串流 · 手柄模式 | 按一下 **＋** | 向电脑发送 Xbox Start / Menu |
| 串流 · 手柄模式 | 长按 **＋** | 打开本地菜单 |
| 本地菜单 | **A / B / 方向键** | 确认 / 返回 / 选择；也可触屏 |

键鼠方案、Xbox 布局和触屏模式可在菜单中设置；详细映射见[按键与输入模式](docs/user-guide.md#按键与输入模式)。

## 当前限制

- 原生 IPv6 尚未交付；TURN TCP/TLS 及不同网络环境的完整互通仍需验证。显示 P2P 或 TURN 以本次实际连接路径为准。
- 切换部分画质或编码设置可能需要重连；电脑端不一定回报实际采用的码率。
- 0.3.73 Beta 自定义档会在协商支持时发送固定 REMB；它不是动态带宽估计，也不能保证达到所选上限。
- 音频暂存和补偿不等于完整音画同步；自动解锁尚未实现，安全设置的实际执行仍需按电脑端验证。
- 解码耗时包含处理与等待，不等于纯硬件时间或端到端操作延迟；50–100 Mbps 稳定串流尚无保证。

连接异常时，可先查看[诊断说明](docs/live-diagnostics.md)；公开分享日志或崩溃报告前请检查并去除账号、网络地址及其他敏感信息。完整边界见[项目状态](docs/project-status.md)。

## 源码构建

```sh
git clone https://github.com/doke69/YOUYOU-Remote-ns.git
cd YOUYOU-Remote-ns
```

需要 CMake、Ninja、Python、devkitPro Switch 工具链及项目所需依赖。构建需自行准备兼容的本地认证材料；仓库不包含该材料或 UU APK。完整依赖、资源模式、命令和分发边界见[构建说明](docs/build.md)及[分发说明](docs/distribution.md)。

使用完整应用模式运行生成的 NRO。公开源码不会自动构成可分发的完整安装包；构建及使用参考材料前，请自行确认相应授权。

## 技术结构

```mermaid
flowchart LR
    Switch[Switch 原生客户端] -->|登录、设备与信令| UU[UU 远程服务]
    Switch <-->|WebRTC 音视频与输入| PC[UU远程 Windows 电脑端]
```

libcurl 处理 HTTPS / TLS WebSocket；libdatachannel 及其依赖处理 ICE、DTLS、SRTP 和 SCTP；FFmpeg / Opus 负责解码，SDL2 与 deko3d 用于界面、音频和显示。项目目录、研究记录及诊断入口见[文档导航](docs/README.md)。

## 许可证与致谢

项目代码使用 [GPL-3.0-or-later](LICENSE)，依赖和字体分别遵循其许可，详见[第三方许可](THIRD_PARTY.md)及[来源与致谢](docs/credits.md)。感谢上游 UURemote-ns 作者、相关开源项目维护者以及提供真机反馈的测试者。

项目是非官方适配，与网易、任天堂无隶属或背书关系；UU 品牌和相关材料归各自权利人所有。仓库不包含 UU APK、签名材料或用户会话。若对来源或许可有疑问，请通过仓库 Issues 联系维护者。
