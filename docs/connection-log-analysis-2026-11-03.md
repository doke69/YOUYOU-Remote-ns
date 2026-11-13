# 2026-11-03 连接失败日志分析

分析输入：`%USERPROFILE%/Downloads/uu全部文件/connection.log`，321 行、14489 字节。
SHA-256：`bd8157dca651bab8b6e9547dcfb21852613b4653e13efa54c5d3828b01d0355f`。
用户标识为 0.3.74 测试日志；文件头及两次启动事件均标识 `0.3.73-beta`。没有配套 NRO，因此不能据标签确定二进制源码版本，也不能用它验证本次新增诊断或修复。

## 结论

可确认的失败阶段是：已取得控制应答并创建本地 Peer，发送本地 Offer/候选后，未观察到协商完成及可用 ICE 路径。首要线索是远端 SDP 应答的生成、传递、接收和应用链路。

日志不能区分电脑没生成 Answer、信令没转发到客户端、旧客户端丢弃/未正确处理消息。也不能证明 VPN、TURN 认证拒绝、音频解码或 GPU 绘制导致失败。

## 时间线与证据

| 尝试 | 日志行 | 已观察到的结果 |
|---|---|---|
| 首次 | 54–86 | joining/signaling/negotiating；Offer 发出、候选收集结束；约 30 秒后出现 loading/idle，后续被新尝试替代，没有完整超时结尾 |
| 第二次 | 98–148 | 1793672930 signaling；2931 Offer/候选；2935 收集结束；2975 Peer=1、ICE=1、连接超时并安排重连，约 45 秒 |
| 第三次 | 161–215 | 1793672977 Offer；同秒两个 TURN TCP connected；2982 收集结束；3022 Peer=1、ICE=1、连接超时并安排重连，约 45 秒 |
| 第四次 | 228–260 | 新一次协商和收集；日志未记录完整结束，随后出现应用再次启动 |
| 再次启动后 | 296–321 | 再次发出 Offer 和候选，日志随后结束，不能判断最后结果 |

当前依赖的枚举将 Peer=1 映射为 Connecting、ICE=1 映射为 Checking、Signaling=1 映射为 HaveLocalOffer；文件中没有对应的 Signaling=0（Stable）或 Peer/ICE Connected 事件。该枚举解释依据当前依赖，不能替代对未提供二进制的版本核验。

第二、三次失败前均收到 `forward-setting-summary`（123、187 行），说明该时段至少部分服务器/电脑向客户端的信令信息能够到达；不能概括成整条信令完全不通。但这不是 SDP Answer，也不证明 SDP 通路正常。

## 对关键日志的解释

- `soac-offer-sent`：客户端走到了本地 Offer 的信令发送代码，不是电脑确认收到并接受 Offer 的 ACK。
- `rtc-gathering-state-2`：本地候选收集结束，不是双方候选对检查成功。
- `turn-stream-tcp-connected`：桥的 TCP 流建立，不是 TURN 分配/认证成功，不是 ICE Connected，也不是 SRTP/音视频通道建立成功。
- `rtc-audio` / `rtc-channels`：创建接收轨道/数据通道的初始化标记，不是音频故障证据。
- 文件没有解码帧/呈现统计，结合一直未完成连接的状态，不能把本轮归因为码率过高、2K 解码或呈现性能。

## 与当前源码的对应

当前 `source/client.cpp` 的 `dispatch()` 接收远端 `soac`；`handleSoac()` 经 `inspectSoac()` 核对 envelope、消息类型和 ICE 会话，再由 `applyRemoteSignal()` 应用 SDP/候选。当前这些路径会记录 received/rejected/applied 事件；本输入没有这些事件，也缺少当前源码无条件记录的 `ice-address-format-numeric-v1`。因此不能直接借当前新增日志的缺失断言旧包完全没有收到远端消息。

最应核对的是 `soac` 入站是否发生、会话匹配是否通过、Answer 是否成功应用；只有远端描述已应用而 ICE 仍 Checking，才进一步聚焦双方候选、TURN 和路径检查。

## 当前修复的覆盖边界

本次增加的 TURN 恢复要求 Peer 已 Connected、所有 TCP/TLS 桥已失败、选中的本地候选是 Relayed。这份日志的完整失败一直是 Peer=Connecting，且没有桥失败事件，所以不满足恢复条件。不能声称这项修复解决了本日志故障。

单纯增加 45 秒总预算、修改解码或自动更换画质，都没有本日志提供的因果依据。

## 按日志继续核对当前代码（追加）

### 1. 已确认的状态覆盖缺陷，与首轮日志相符

`source/client.cpp` 1126–1130 的 refresh 命令不检查是否正在协商或已有会话，直接 setState(loading)；`refresh()` 更新设备后又 setState(idle)。此过程没有同步关闭 Peer/socket/连接期限。`source/live_frontend.inc` 300 行的刷新动作直接投递该命令。

因此，建连过程中刷新设备可以把尚在协商的界面改成 idle，但底层仍继续连接。日志 85–86 行在 connecting 后、真正 disconnect 前出现 loading/idle，与这条路径相符；日志没有用户按键记录，不能断定当时一定按了刷新。它解释首轮状态显示异常的可能来源，不解释后两轮完整 45 秒失败。

### 2. 已确认的兼容缺口：压缩 SDP 与二进制附件没有完整处理

- `source/client.cpp` 1419–1423：二进制消息只递减 remainingAttachments_，收齐后直接 dispatch(pending_)，没有保存 message.data，也没有将 `_placeholder` 替换为附件数据。
- `include/signaling_envelope.hpp` 22–25：offer/answer/restart_ice 只接受非空字符串 `sdp`。
- `include/rtc_signaling.hpp` 26–28：只从 `data.at("sdp")` 创建远端 Description；当前源码没有 gzip_sdp 解压分支。

本地 Linux 参考仓（HEAD `ab5d0a6`，所读四个 viewer 文件无工作区修改）有另一条路径：`src/viewer/signal.rs` 82–128 重建二进制附件，`src/viewer/rtc.rs` 298–307 同时接受明文 SDP 和 base64/gzip_sdp。该参考代码的注释声称观察到主机发送此格式，但本轮没有独立复核它的现场证据或手机 APK，因此不能把注释提升为本次主机的实测事实。

使用当前生产 `inspectSoac()` 的独立合成探针结果：

```text
plain-answer-accepted=1 gzip-answer-rejected=1 binary-gzip-placeholder-rejected=1 foreign-session-rejected=1
```

探针源码和结果位于 `C:/uu/repo/.tools/tmp/connection-verify-20261103/soac_compat_probe.cpp`、`soac-compat-probe.log`。它验证当前代码不支持这两种输入形态；没有伪造实际主机应答或连接外部服务。

重要反证：当前代码收到上述压缩应答并进入 handleSoac 后，会明确报“电脑协商消息格式不兼容”，而非必然静默等待 45 秒。本日志没有对应错误和原始消息形态，因此该兼容缺口不能认定为本轮根因，也不能单凭它断言补解压就能解决。

### 3. 高价值的发送结构差异，尚不能确认为协议错误

当前 `source/client.cpp` 208–228 的 clientCapabilityJson() 返回直接包含 remote_display_info、local_display_info、frame_quality_capability 的 JSON；1475–1494 的 control 事件外层带 app_control_id，但 streamer_data 内没有 control_id/device_capability 包装。

本地 Linux 参考的 `src/viewer/wire.rs` 108–116 构造的 streamer_data 则带 control_id 和 device_capability 包装，内部包含 display_info、ice_id、video_codec_capability。该差异与“电脑是否为当前 Offer 创建并正确关联会话”有关，值得按实际 TV/PC Control 解析代码核实。仅凭一个参考实现不能认定当前格式无效，也不应盲目替换能力字段而影响已经可连接的主机。

参考 `src/viewer/session.rs` 37–40 还提示主机连接表尚未建好时可能丢弃 Offer，但其延迟默认值是 0，且本日志时间戳精度只有秒，不能证明竞态、不能据此直接加固定 sleep 或重发 Offer。

### 4. 本轮没有找到证据支持放宽会话过滤或改解码

当前 inspectSoac 对字符串/整数 ICE ID 做文本匹配，错会话和未知类型才忽略；格式错误及 H.265 参数拒绝会产生明确异常。明文正确会话 SDP 和候选具备正常应用路径，先到候选也有独立 pending 队列。日志不足以证明这些校验误拒绝了合法应答，因此应保留会话隔离，不能去掉 ICE ID 校验来尝试连通。

本轮仅追加代码排查和离线探针，未新增生产代码修复。现有 TURN 恢复和诊断修改保留。
