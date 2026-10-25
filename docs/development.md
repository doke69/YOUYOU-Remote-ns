# 开发记录

## 0.3.44 架构概览与阅读方式

本页描述的当前源码基准为 **0.3.44**（2026-10-25）；逐版本故障记录和早期架构说明是历史资料，不应直接当作当前实现。当前功能、验证边界与未完成事项见[项目状态](project-status.md)，具体交付见[0.3.44更新说明](releases/v0.3.44.md)。本次文档维护没有重新构建程序或重做真机测试。

```text
Switch 原生界面 / 手柄 / 触屏
  ├─ HTTPS API：登录、设备、房间
  ├─ TLS WebSocket / Socket.IO：信令
  └─ libdatachannel：ICE / DTLS / SRTP / SCTP
       ├─ 视频：RTP/RTX → 组帧、补包、参考帧恢复
       │         → H.264 / 实验性 H.265 → FFmpeg NVDEC
       │         → 硬件帧 NV12 导入与映射缓存 → Deko3D 显示
       │         （导入/硬件路径不可用时存在 CPU/SDL 回退）
       ├─ 音频：Opus → PCM → 播放
       └─ 控制：键鼠消息 / 虚拟手柄 / 实验性原生触点
```

- `main.cpp` 处理输入隔离和主循环；`live_frontend.inc` 管理当前菜单、偏好、映射选择；`tv_frontend.inc` 提供共用绘制组件。
- `client.cpp` 处理登录、设备、连接、控制与画质请求；`quality_rpc.cpp` 编解码画质 RPC。版本更新时核对请求、应答和实际应用状态；不能凭应答头就认定编码器已应用目标帧率。
- `strict_video_receiver.cpp`、`video_assembler.cpp` 及相关反馈路径处理组帧、补包和参考帧恢复。
- `media.cpp` 与 `native_display.cpp`、`native_nv12.cpp` 分别承担解码、播放调度与原生显示/硬件帧导入。
- 接收 FPS、解码 FPS、显示提交 FPS 和源端游戏 FPS 不同；解码耗时、网络 RTT、播放缓冲也不是端到端操作延迟。分析时保留这些边界。
- GPU NV12 路径已有正向真机反馈，但仍有回退和资源生命周期限制，不能简化为“所有路径绝对零拷贝”。

当前需要继续验证的项目、暂停研究及使用边界集中维护在[功能状态表](project-status.md)。贡献者还可阅读[构建说明](build-v52.md)、[PR1 审核记录](reviews/v52-review.md)。

## 历史开发记录

以下为早期实现与故障处理原记录，结论以对应版本为准。

## 结构

```text
Switch NRO
  ├─ libcurl HTTPS → 网易 UU 正式 API（签名、二维码、设备、加入房间）
  ├─ TLS WebSocket → 正式 Socket.IO 信令（自定义请求头）
  └─ libdatachannel ↔ 原版 UU 电脑端（ICE / DTLS / SRTP / SCTP）
       ├─ H.264 → FFmpeg NVDEC 或软件解码 → RGBA → SDL2
       ├─ Opus → PCM → SDL2 音频
       └─ Joy-Con / 触屏 → 原版 JSON 二进制控制数据
```

`source/protocol.cpp` 负责签名规范化、Protobuf、Socket.IO 数据解析和 WebSocket 帧；`source/transport.cpp` 负责签名 HTTPS 与使用 libcurl CONNECT_ONLY 的 TLS WebSocket。原版请求头、空头的 libcurl 分号语法、连接参数均以参考项目为依据。

`source/client.cpp` 在单个工作线程中处理 API、信令和用户命令。WebRTC 回调只向队列写入带连接代次的事件；断开时使旧代次失效。媒体回调使用独立的媒体代次检查，防止重连后收到旧画面。退出前关闭连接并等待 `rtc::Cleanup()`，然后释放网络服务。

`source/auth.cpp` 校验登录响应和短信参数，复用参考 Web 端的验证码发送与登录 API，成功发送后限制 60 秒内重发。`source/diagnostics.cpp` 将固定连接阶段写入 SD 卡日志，异常时附加类型、数值错误码和最多 16 帧代码偏移；文件超过 128 KiB 后重建，不记录异常消息原文或协议数据。

`source/background.cpp` 为候选地址解析和 RTC 清理保留异步任务的共享 future；完成后回收线程，正常或异常退出时均在关闭网络服务前等待剩余任务。Switch 不使用 `std::thread::detach`。

`source/media.cpp` 在独立解码线程上处理有界视频队列，使用 AVPacket 自有缓冲与 FFmpeg 所需 padding；显示线程只读取最新帧。音频积压超过 200 ms 时清空队列。这个初版没有完整的 RTP 重传策略、音频 jitter buffer 或音画同步；不能推定为稳定的游戏串流客户端。

`source/main.cpp` 提供 1280×720 手柄界面、中文系统字体、二维码和画面黑边坐标映射。要求 hbmenu 完整应用模式。布局不嵌入 WebView。

## 真机检查步骤

1. 以完整应用模式启动，检查系统字体、首页、返回与退出；未登录时不出现假设备。
2. 分别验证手机扫码和短信登录，确认账号和设备列表正确；检查切换登录方式、短信错误提示、60 秒重发、取消与二维码过期后重试。
3. 选择在线原版电脑，确认房间、控制权、ICE、DTLS 与画面；断开再连接至少三次。
4. 检查声音、黑边触控坐标、左右键、WASD、Ctrl/Shift、滚轮、组合键释放。
5. 在按住键鼠时按 Home、休眠或断开，确认电脑端没有残留按键。
6. 检查局域网与不同网络 UDP TURN；验证离线、被其他设备接管、掉线和账号失效提示。
7. 检查 NVDEC 是否实际被选中、720p30 稳态性能、内存与音画漂移；软件 fallback 的性能不能由编译结果保证。

这些步骤需要实际 Switch 和用户账号，本次开发环境无法替代。

## 本次验证结果

- Windows 上以 devkitA64 r30 / GCC 16.1.0、libnx 4.12.0 完成交叉编译与 NRO 打包。
- 本机协议测试通过：参考 Web 固定签名、连接 Protobuf、Socket.IO / ICE，以及 300 组随机 WebSocket 数据流。
- 本机原生 WebRTC 回环通过：连续三轮初始化与清理，覆盖 ICE、DTLS、五个 SCTP 数据通道、二进制键盘消息、SRTP H.264 与 Opus RTP 重组。
- 短信 API 参数、冷却时间、失败重试、前导零验证码和登录响应校验通过模拟请求测试；未发送真实短信。
- 使用实际 qrcodegen 编码器与新版布局生成 477×477 像素的合成登录二维码，经图像解码恢复出完整原始 URL；手机实扫仍需真机复测。
- Horizon 使用的 pthread 随机数分支通过 8 线程并发初始化、状态保留、线程隔离和退出清理测试。
- Horizon 的 usrsctp 内部 C 类型与 libdatachannel 外部 C++ 类型均通过地址长度、地址族偏移与字段宽度的编译期断言；ARM64 实际初始化指令中的地址前缀为 `10 7b`（长度 16、地址族 123）。
- 在 GNU 本机回环中将 `pthread_detach` 强制替换为返回 ENOSYS，先验证 `std::thread::detach` 确实抛错，再验证域名候选解析、ICE / DTLS / SCTP / SRTP 和清理全程不调用 detach。后台任务并发、嵌套、移动参数、异常保留及退出等待测试通过。
- NRO0 与 ASET 资源段检查、ZIP CRC 和包内 NRO 一致性检查通过。
- 已收到用户真机连接阶段崩溃报告并完成符号定位；修复后的正式房间互通、短信实收、NVDEC、实际音视频与键鼠表现仍需真机复测。

## v0.1.0 连接崩溃定位

恢复发布版 ELF 后，其 Build ID `86ad8d2606b95ba1b9a6bbe9844a7acbde840f9a` 与用户报告完全一致。`2168-0002` 的 PC 偏移 `0xdc22c` 对应 `rtc::impl::IceTransport::IceTransport`，上层依次为 `initIceTransport`、`setLocalDescription` 和 `Client::beginPeer`。

故障指令位于 ICE 服务器随机排序前，访问模板函数中的 `thread_local` 随机数状态；该构建生成了错误的 `[thread_pointer + thread_pointer]` 地址。v0.1.1 在 Horizon 上使用 pthread key 保存每线程引擎，在线程退出时释放；其他平台保留原实现。已确认修复后的 ICE 目标文件调用 `pthread_getspecific` / `pthread_setspecific`，不再含原先的 `__aarch64_read_tp` 访问。补丁由 `scripts/patch_deps.py` 重复应用，不改动上游锁定版本。

每版排查应保存对应的 `build-nx/UURemote-ns.elf` 与 `.map`，并核对 Build ID。原始用户崩溃报告不纳入仓库或发布包。

## v0.1.1 连接后异常终止排查

用户截图显示 `2345-0008 (0x1159)`，但 PC 和回溯均为零。连接日志表明已执行 `rtc-offer`，约 5 秒后进入 `runtime-terminate`；因此该截图不足以定位最初异常。

检查发布版二进制发现 `pthread_detach` 固定返回 88（ENOSYS），而上游在解析非数值 ICE 候选地址和清理 RTC 运行时时调用 `std::thread::detach`。分离失败后，仍可 join 的线程在析构时触发 terminate。v0.1.2 用保留共享 future 的异步任务替换这两处，确认最终 Switch ELF 不再含 `pthread_detach` / `std::thread::detach`。

该兼容缺陷已通过禁用 detach 的本机回环覆盖；是否就是用户此次异常的直接触发点，仍需真机复测。终止处理器现在记录异常类型、数值错误码和代码偏移，再直接退出进程，避免 libc 的全局清理与仍在呈现画面的 UI 线程发生竞争。

## v0.1.2 SCTP 堆越界定位

用户报告的 Build ID `5449a2e3c1b942cde71efc81eb5317fd3633f6ea` 与保存的 v0.1.2 ELF 一致。`2168-0002` 的 PC `0x10b7858` 位于 `_malloc_r`；调用栈已通过 DTLS 握手，正在 `SctpTransport` 构造函数末尾插入实例。崩溃前刚调用了 `usrsctp_register_address`。

Horizon 的 `sockaddr` 首两字节是长度和地址族；usrsctp 公共头按已知 BSD 系统宏选择这种布局，但没有识别 `__SWITCH__`，导致 `sockaddr_conn` 使用两个字节的地址族，并将 `HAVE_SCONN_LEN` 缓存为 false。内部却正确启用了 `HAVE_SA_LEN`。因此 `sctp_add_addr_to_vrf` 将 `AF_CONN=123` 读作 `sa_len`，把栈上的 123 字节复制进 32 字节的地址联合体，覆盖后续字段及堆元数据。该复制发生后，下一个 16 字节分配在 malloc 内崩溃。

v0.1.3 将 Horizon 加入长度前缀分支，重新检测已缓存的 `have_sconn_len`，并将 `HAVE_SCONN_LEN` 同时传给 C 库和 C++ 适配层。两处都包含 `switch_compat/sctp_layout.h`，检查长度、地址族的偏移、字段宽度和目标容量。`python tests/switch_sctp_layout_test.py` 用实际 SDK 头文件分别交叉编译 C / C++，验证正确布局通过、旧布局被拒绝。旧版 `usrsctp_register_address` 的实际 ARM64 初始化值为 `0x007b`；新版该函数及 C++ `getSockAddrConn` 均为 `0x7b10`，小端字节序下即长度 16、地址族 123。

本机回环使用宿主 socket 布局，不能覆盖 Horizon ABI；本次额外核对了目标编译和二进制指令。修复后的正式串流仍需真机复测，原始用户报告继续保留在仓库外。

## v0.1.3 协商失败与 v0.1.4 诊断

用户日志中，v0.1.3 在 `rtc-offer` 后约 41 秒记录 `client-exception`、`disconnect`、`error`，未出现 `runtime-terminate`。日志缺少远端描述、候选和各层状态，不能确认这是 ICE 失败、DTLS 失败、SCTP 超时还是其他异常。

与参考 Web 实现对照后，补齐 `include/rtc_signaling.hpp` 中的候选暂存：远端 SDP 尚未设置时保留候选，设置描述并准备好本地回答后按顺序应用，最多 128 条；断开时清空。本机回环使用同一处理函数，故意在 Offer 和 Answer 前发送候选，覆盖扁平 / 嵌套格式以及完整音视频、五个数据通道与连续三轮清理。该顺序缺陷已修复，但旧日志不足以证明它触发了本次真机失败。

`source/rtc_diagnostics.cpp` 将允许的库日志前缀映射成固定事件，只提取有限位数的数值错误码，其余文本全部丢弃。补丁仅检查 SCTP 包第一个 chunk 的类型，记录 INIT、INIT ACK、COOKIE ECHO、COOKIE ACK 和 ABORT 的收发方向，不保存包内容。Mbed TLS 异常增加数值错误码以便诊断；日志脱敏测试覆盖敏感尾部和未知消息被丢弃。

真机排查时先确认 `app-start-0.1.4`，再看 `soac-offer-sent`、`soac-answer-received` 与 `soac-description-applied`；之后依次检查 ICE、DTLS、SCTP。ICE 状态值来自锁定版 libdatachannel：0 New、1 Checking、2 Connected、3 Completed、4 Failed、5 Disconnected、6 Closed。`rtc-dtls-connected` 表示完成 DTLS 握手，`rtc-sctp-connected` 表示数据通道传输已建立；缺少某个事件本身不能证明上一层失败，需结合状态、错误和超时判断。

## v0.1.5 关闭后候选处理

截图显示 `Got a remote candidate without ICE transport`。在锁定版 libdatachannel 中，`processRemoteCandidate` 只有在远端 SDP 存在而 ICE transport 指针为空时抛出该错误。`closeTransports` 先进入 Closed 状态，再清空传输指针，但保留 SDP；因此该消息可能来自最初失败后的迟到候选，不能直接当作失败起因。客户端先批量处理回调事件，再读信令；批量读取过程中发生关闭时，新候选可能早于下一轮状态事件处理而抛错。

远端信令处理现在检查 Failed / Disconnected / Closed 及 ICE Closed，清空待用候选并返回关闭标记。处理期间若遇到 logic_error，仅在连接已关闭时忽略，活动连接的异常继续传播。原先排队的连接状态事件仍由客户端正常处理，不会因这个保护而自动重连或掩盖持续失败。

本机回环在完成五通道音视频并关闭后，确认远端 SDP 仍存在，随后注入候选和无效的迟到 SDP，验证不再调用已关闭的传输或保留旧候选。用户随后复制的最新日志仍以 `app-start-0.1.3` 启动，说明尚未运行诊断版；v0.1.5 首页增加来自同一 CMake 版本定义的版本号，避免仅靠文件名判断更新是否生效。

## v0.1.6 协商期间事件处理延迟

v0.1.5 真机日志已记录 Offer 发出、Answer 收到并应用、远端候选持续应用，但从首次状态之后再无主线程记录的 RTC 状态，54 秒后进入连接超时；DTLS `-29312`（`MBEDTLS_ERR_SSL_CONN_EOF`）出现在 disconnect 之后，符合退出时停止接收队列的情况，不能单独据此判定 DTLS 原始失败。

旧 `Client::run` 在同一循环里读取最多 64 条信令，甚至在检查条数上限前又调用一次 receive，之后先检查超时，到下一轮才处理回调。只要读取持续耗时，连接与收集状态等事件就会滞留。`pumpSignaling` 现在以 16 条 / 8 ms 为预算，并在出现待处理事件、取消或退出时让出；随后调用 `drainEvents`，再检查心跳与连接期限。慢速读取超过 100 ms 和超时时的实际状态只记录数值，不含协议内容。

Switch 最终链接通过 `--wrap=sslConnectionRead` / `--wrap=sslConnectionWrite`，在数据读写时显式使用 `SslIoMode_NonBlocking`；握手本身、HTTPS 证书和主机名验证沿用现有实现。该适配使用公开 libnx API，避免依赖 curl 私有上下文布局。实际 SDK 的 libcurl 目标文件已经含有设置 I/O 模式的调用，不能把“库从未设置非阻塞”当成已确认原因；这里的包装是在数据入口保证 CONNECT_ONLY 所需的非阻塞契约。devkitPro 后端实现可参考[官方补丁](https://github.com/devkitPro/pacman-packages/blob/master/switch/curl/switch-curl.patch)，本地锁定 SDK 二进制也单独核对。

测试覆盖持续来包、单次读取耗尽时间预算、取消时不再读取；TLS 模拟服务验证读写在非阻塞模式下执行，WouldBlock、部分读写和设置模式失败均完整保留。交叉编译使用真实 libnx 头；最终 ELF 的 libnx_recv / libnx_send 分别调用包装函数，包装函数调用真实 SSL 服务入口。真机是否能完成 DTLS / SCTP 仍需更新后验证。

## 参考

- 参考项目 `UURemote-web` 中的 `server/uu-api.mjs`、`public/lib/streamer.js`、`public/lib/protocol.js`、`public/lib/protobuf.js`。
- [libdatachannel](https://github.com/paullouisageneau/libdatachannel)：媒体接收与数据通道 API。
- [Switch SDL2 示例](https://github.com/switchbrew/switch-examples/tree/master/graphics/sdl2/sdl2-simple)：原生应用和依赖链接。
- [devkitPro FFmpeg 包构建](https://github.com/devkitPro/pacman-packages/blob/master/switch/ffmpeg/PKGBUILD)：Horizon/NVTEGRA 支持。
- [NVTEGRA FFmpeg 实现说明](https://ffmpeg.org/pipermail/ffmpeg-devel/2024-May/328549.html)。

## v0.1.7 系统 23.0.1 / 大气层 1.12.1 适配

- 目标环境、机型差异、安装与真机自检见 [固件 23.0.1 / AMS 1.12.1 适配说明](firmware-23.0.1-ams-1.12.1.md)。
- `source/main.cpp` 新增 `set:sys` 初始化与 `setsysGetProductModel()` / `setsysGetFirmwareVersion()`，首页底部显示机型 · 模式 · 系统版本，
  并把 `console-<机型>-<模式>-fw<x.y.z>` 写入 `connection.log`；窗口创建改为 TV 1920×1080 / 掌机 1280×720，并加 `SDL_WINDOW_RESIZABLE`
  让 devkitPro SDL2 驱动在插拔底座时自动切换，渲染逻辑尺寸仍为 1280×720。
- 固件 23.0.x 本身不需要改协议实现：libnx 4.12.0 已包含 23.0.0 的 svc/spl 类型与 21.0.0 之后的 TLS ABI 标记；AMS 1.12.1 修复了
  dns.mitm 的 c-ares 拦截与 fs.mitm 的 BatchRead 支持，hbmenu 3.6.1 / hbl 2.4.5 为其自带版本。
- 构建锁定 SDK 补入了原 `sdk-windows.lock.json` 遗漏的 portlibs：`switch-libopus`、`switch-bzip2`、`switch-libdrm_nouveau`、
  `switch-libfribidi`（否则链接 `-lopus` / `-lbz2` / `-ldrm_nouveau` 失败）。
- Windows 上 devkitA64 binutils 无法打开含非 ASCII 字符的绝对路径，构建需通过纯 ASCII 目录联接（可通过 `mklink /J` 创建纯 ASCII 路径入口）
  并把 `TEMP`/`TMP` 指向 ASCII 目录；详见源码构建文档注意事项。

## v0.1.8 花屏修复

真机反馈：电脑端画面正常，Switch 端花屏（彩色块 / 条带 / 局部错乱）。判断为 RTP/UDP 丢包或解码队列丢帧后
H.264 参考帧丢失，而客户端从不请求关键帧，导致画面长期停留在错误隐藏结果上。

- `source/client.cpp`：`video_->requestKeyframe()` 发送 RTCP PLI。连接建立（peer-state Connected）后立即请求一次；
  主循环中在解码器请求（`media_->takeKeyframeRequest()`）、画面停滞 >1.5 s 或距上次请求 >4 s 时再次请求，最小间隔 700 ms。
  事件写入日志：`rtc-keyframe-initial` / `-media` / `-stall` / `-periodic`。
- `source/media.cpp`：检查 `AVFrame.decode_error_flags`；损坏帧不再覆盖最后一帧好画面（连续 2 s 无干净帧才恢复显示）；
  解码队列上限 8→16，溢出时清空积压、刷新解码器并请求 IDR；NVDEC 解码线程数改为 1（软件解码 3）；
  新增 `config.ini` 的 `decoder=auto|software|hardware` 与状态行（解码方式 / 分辨率 / 解码错误 / 丢帧）。
- 若仍花屏：覆盖层状态行可区分「软件解码 / NVDEC 硬件解码」，`decoder=software` 可排除 NVDEC 驱动因素；
  同时需要确认发送端是否响应 PLI（日志中会持续出现 `rtc-keyframe-*` 事件）。

## v0.1.9 网络信息面板

- `source/stream_stats.cpp`：`uu::RtpTrackStats`（`rtc::MediaHandler`）插入视频/音频 track 的 handler 链尾，
  接收原始 RTP 包，按 16 位序列号差值统计丢包、按 RFC 3550 计算抖动、按 1 秒窗口计算码率。
- `source/media.cpp`：解析 Annex-B 的 SPS（档次/级别）、SEI user_data_unregistered（对端编码器标识）与 IDR；
  统计解码帧率、单帧解码耗时（EMA）、解码错误/丢帧；`Media::stats()` 暴露 `MediaStats`。
- `source/client.cpp`：worker 每 0.5 秒汇总 `PeerConnection::rtt()`（SCTP srtt，与媒体同一 UDP 路径）、
  `getSelectedCandidatePair()`（局域网/P2P/TURN、UDP/TCP）、音视频 RTP 统计与解码统计；
  记录 PLI 请求次数与「请求→IDR」恢复耗时；快照加锁缓存供 UI 线程读取，避免跨线程访问 RTC 对象。
- `source/main.cpp`：R3（`HidNpadButton_StickR`）/ F3 切换半透明信息面板；覆盖层提示加入「R3 网络信息」。
