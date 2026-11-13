# YOUYOU Remote 连接问题分析报告

**分析日期：** 2026-11-03  
**分析对象：** `C:\uu\current\YOUYOU-Remote-ns`  
**源码版本：** 0.3.74（`CMakeLists.txt`）  
**Git 基线：** `ba4d7f9f8f87`，`main` 与 `origin/main` 一致，检查时工作区干净  
**分析性质：** 只读静态源码审查；本次未修改代码、未构建、未连接设备，也没有将某次连接失败日志作为本报告证据。

> 本报告说明当前代码实际做了什么、哪些设计可能导致连接停滞，以及如何用现有日志区分阶段。静态代码中的风险点是待运行数据验证的假设，不等于已确认的故障根因。

## 1. 执行摘要

YOUYOU 的连接由两条逻辑链组成：先通过 UU 信令服务加入远控房间并申请电脑控制权，再建立 WebRTC PeerConnection。PeerConnection 使用 ICE 检查直连或 TURN 中继路径；ICE 建立后还要完成 DTLS/SRTP，并建立 SCTP 数据通道。当前 libdatachannel 在 DTLS Connected 后会启动 SCTP，也会安排音视频 Track 打开；因此 RTP 可能早于 SCTP 完成和界面 connected 到达。首帧显示仍在解码/呈现之后。

从当前代码可得出的结论：

1. **局域网直连有机会发生，但代码没有单独的“检测到同一 Wi-Fi 就直接改走 LAN”分支。** LAN 地址作为 ICE 候选参与检查；是否最终选中它，取决于双方候选、可达性、ICE 检查和选路策略。Switch 侧当前的自定义策略会优先比较 IPv6、同子网 IPv4、其他路径和中继；若信令明确给出 `force_relay=true`，则 PeerConnection 被设成 relay-only，局域网直连会被排除。
2. **VPN/代理导致中继不可达是可能场景，但源码不能证明 VPN 一定会让中继服务器拒绝。** 客户端依照房间信令给出的 TURN 地址、凭据及 UDP/TCP/TLS 类型尝试；中继认证、网络可达性和服务端策略仍须看同一轮日志中的 TURN 与 ICE 事件。
3. **连接超时采用单一 45 秒总期限。** 期限从信令 WebSocket 打开后开始，到 PeerConnection 状态首次进入 `Connected` 才清除。源码没有在收到控制权限、SDP 应答或 ICE Connected 时重置此期限。因此慢 TURN、VPN 路径或其他阶段拖延可能消耗同一总预算；这是可验证的设计风险，不是已确认根因。
4. **PeerConnection 的 `Connected` 会让客户端进入 connected 状态，但代码不等待首个音频帧、视频帧或 Switch 实际显示画面。** 对当前带 application/SCTP 的协商，PeerConnection Connected 要等 SCTP Connected；但 Track 会在 DTLS Connected 时安排打开，所以 RTP 可能早于 SCTP 完成和 UI connected 开始。电脑端“正在控制”、Peer Connected 和 Switch 已显示首帧是不同证据。黑屏问题必须检查媒体到达、解码和呈现，而不能只看 PeerConnection 状态。
5. **诊断文件缺失不能直接说明 ICE 没运行。** `ice_candidates.json` 与 `selected_path.txt` 受 `config.ini` 的 `diag` 开关控制；关闭时不会生成。连接主日志与具体诊断文件的开关不同。
6. **主要连接设计风险是单一 45 秒总期限覆盖多个阶段。** PeerConnection 回调队列达到 2048 项会静默不入队，这是确定存在但目前低优先级、尚无现场证据的风险；应先记录高水位和丢弃计数，不应据此解释普通连接卡住。

当前证据不足以把个别连接失败归因于 VPN、TURN、ICE、DTLS、SCTP 或某一种音视频通道。最关键的下一步是按同一次连接的阶段事件判断“最后成功到哪里”，而不是反复增加泛化诊断。

## 2. 分析范围和证据等级

本报告核对了当前仓库中的连接状态机、Socket.IO 信令处理、ICE/TURN 配置、TURN TCP/TLS 桥接、RTC 事件映射和诊断文件生成路径。

证据分为三类：

- **源码事实：** 当前代码路径、状态变化和文件生成条件，可由本报告引用的行号复核。
- **风险判断：** 根据源码控制流推导出的可能失败方式，必须由对应运行日志确认。
- **未确认事项：** 需要某次失败连接的 `connection.log`、ICE 文件或主机端日志；本报告不据静态代码猜测结果。

本报告不分析或输出配对密钥、认证令牌、设备凭据或原始敏感会话数据。诊断文件可能含局域网/公网 IP 与端口，分享前应遮盖地址。

## 3. 当前连接流程

### 3.1 加入房间和申请控制权

用户发起连接后，客户端先断开旧会话、请求加入目标设备的远控房间、读取房间返回的信令凭据，并打开 UU 信令 WebSocket。WebSocket 打开后启动 45 秒连接期限。房间应答取得电脑端标识后，客户端创建控制 ID，构造画面尺寸、帧率、画质及编解码能力，并发送控制权限请求。

证据：`source/client.cpp` 1098–1117、1415–1452 行。

### 3.2 创建 PeerConnection、收发 SDP 和 ICE 候选

收到控制应答后，`beginPeer()` 使用信令返回的 ICE 服务器、`ice_id` 和 `force_relay` 配置 PeerConnection。客户端创建接收方向的音视频轨道以及若干数据通道，然后生成本地 SDP。电脑端的 SDP 和 ICE 候选通过 `soac` 信令事件处理；候选先于远端 SDP 到达时最多暂存 128 项，之后在应用远端描述时加入 PeerConnection。

证据：`source/client.cpp` 1499–1608、1621–1757、1758–1874 行；`include/rtc_signaling.hpp` 8–61 行。

### 3.3 ICE 路径检查、加密和数据通道

ICE 候选对检查成功后，ICE 状态可能先到 Connected/Completed。客户端此时显示网络路径已就绪并继续等待加密及数据通道。之后由 PeerConnection 的整体状态决定是否进入 connected。超时分类会结合当前是否已应用远端描述以及 ICE 状态，将失败归类为“未取得电脑协商应答”“直连或中继路径未建立”或“加密或数据通道未就绪”。

证据：`source/client.cpp` 637–654、1247–1289 行。

### 3.4 音视频媒体与界面显示

客户端在连接协商期间就创建接收方向音频轨道和视频轨道。当前 libdatachannel 的顺序是 DTLS Connected 后启动 SCTP 并安排打开音视频 Track；SCTP Connected 时才将 PeerConnection 状态改为 Connected。客户端随后清除总期限并显示 connected，但没有等待首个音频包、首个视频帧或原生显示提交。媒体 RTP 因此可能早于 SCTP/UI connected 开始；媒体帧再由轨道回调交给 `Media`，进入解码与显示路径。

证据：`source/client.cpp` 1261–1279、1621–1683 行。

因此应区分：

```text
电脑端显示“正在控制”
    ≠ ICE 路径已成功
    ≠ DTLS/SRTP 已成功
    ≠ 音视频数据已到达 Switch
    ≠ Switch 已解码并显示首帧
```

具体以日志阶段事件为准，不能把上述状态互相替代。

## 4. 局域网直连与 VPN/TURN 的关系

### 4.1 局域网直连如何发生

当前实现没有发现一个独立的 LAN 连接 API，也没有在房间连接失败后另开一条私有 LAN 连接。局域网 host candidate 和其他 ICE 候选共同进入 ICE 检查，ICE 根据连通性及选路策略建立候选对。

Switch 侧的 libjuice 补丁读取 Switch 当前 IPv4 与子网掩码；在启用 `route_policy` 时，候选对优先级分类为 IPv6、同子网 IPv4、其他非中继路径、relay。候选对仍需要真实检查成功。Switch 主动控制角色在 route policy 开启时，对当前临时选中的候选对启动 `NOMINATION_TIMEOUT=2000 ms` 等待窗口；此期间 trickle 候选仍有机会加入/排序，临时工作路径可能已经传媒体，但尚未最终提名。策略只在尚未提名时覆盖 RFC pair priority；一旦 pair 已 nominated，后续排序回到 RFC priority，不再由自定义 rank 把已提名中继改成 LAN。因此中继若在窗口结束时先被提名、而 LAN 候选稍后才到或检查成功，策略不会自动改道到 LAN。这是“同一 LAN 仍选中中继”的具体可能窗口，不代表已解释某次失败。

证据：

- `source/client.cpp` 1514–1517 行记录所用策略；1537–1539 行应用 `force_relay`。
- `scripts/dependency-overrides/deps/libjuice/src/agent.h` 55 行定义 2000 ms 提名等待；`agent.c` 1063–1078 行启动计时，1126–1155 行处理临时工作路径及提名，2564–2598 行计算候选路径偏好。

**结论：** 同一 Wi-Fi 或同一子网只说明存在尝试直连的条件，不保证 LAN 候选一定被收集、交换、检查成功或最终提名。主机防火墙、VPN 虚拟网卡/路由、候选地址范围、NAT 和信令返回的 `force_relay` 都可能改变结果。

### 4.2 `force_relay` 的影响

如果房间控制应答中的 `force_relay` 为 true，客户端设置 `rtc::TransportPolicy::Relay`。这会把连接限制为中继候选策略；即使双方在同一 LAN，也不能据此假设仍会直接走 LAN。

证据：`source/client.cpp` 1513–1517、1537–1539 行；`ice_candidates.json` 初始对象保存 `force_relay`。

### 4.3 TURN UDP、TCP、TLS 选择与桥接

客户端统计房间应答中提供的 TURN UDP/TCP/TLS 类型。Switch 默认按重连次数轮换类型，也可通过 `turn_transport` 请求某一类型；若所请求类型未提供，则回退到可用类型。当前一次 PeerConnection 只保留选定的一种 TURN 类型，不会在同一次 ICE 会话里自动并行尝试 UDP、TCP、TLS 三种 TURN 传输。

libjuice 原生 TURN 支持走 UDP。选择 TCP/TLS 时，Switch 代码启动本地回环 UDP 与远端 TCP/TLS 的桥接，再把 ICE 的 TURN 地址改为本地回环端口。该桥有连接超时、发送/接收和队列上限；错误会写入诊断事件。

源码还公开了 `TurnStreamBridge::failed()`，但当前 `source/client.cpp` 的使用点只创建、保存和最终清理 bridge，没有发现轮询该状态并立即通知 ICE/连接状态机的调用。若桥线程失败，可能先记录桥错误，而上层仍要等 ICE/DTLS/SCTP 状态变化或连接期限触发；这是一项值得用错误时间线验证的观测/故障传播风险。

证据：`source/client.cpp` 1552–1593、2095、2374 行；`source/turn_stream.cpp` 43–160、179–185 行。

**VPN 相关结论：** VPN 可能影响 DNS、路由、UDP 可达性、TURN TCP/TLS 可达性或服务器看到的出口地址，但“启用 VPN 就一定由中继拒绝”不是当前客户端代码能够推出的结论。必须查看 `turn-selected-*`、TURN 分配/认证/错误事件及 ICE 候选对状态，才能分辨服务器拒绝、桥接失败、网络不可达和普通 ICE 超时。

## 5. 源码中最值得关注的故障风险

### 5.1 单一 45 秒期限覆盖多个阶段，且独立于信令心跳

期限在信令 WebSocket 打开后设为 45 秒；仅在 PeerConnection 状态变为 Connected 时清零。控制应答、SDP 应答和 ICE Connected 都不会刷新期限。另有独立信令心跳期限：初始值 45 秒；Socket.IO hello 到达后按 `pingInterval + pingTimeout + 5000 ms` 更新，默认参数下约 50 秒；每个入站 WebSocket 包都会刷新 `lastPacket_`。因此即使信令持续收发、心跳正常，连接总期限仍从 WebSocket 打开时刻单独倒数并可能先触发。超时信息会输出当时的 peer/ICE 状态、SOAC 事件数及拒绝数。

证据：`source/client.cpp` 1113–1117、634–654、1261–1266、1369–1389 行；成员初值见 `include/client.hpp` 111–112 行。

**可能影响：** 在信令慢、候选迟到、TURN 握手/桥接慢或 ICE 需要更多检查时间时，各阶段共享同一预算，后续阶段可能在剩余时间很少时开始。  
**证据边界：** 这是静态可确认的期限设计；没有某次失败日志的阶段耗时，不能说它就是用户遇到的根因。应检查 `client-timeout`、`timeout-*`、最后一个 `soac-*`/`rtc-*` 事件及每个事件的时间戳。

### 5.2 PeerConnection 回调队列满时静默丢弃

创建 PeerConnection 时，`post()` 将异步回调放进客户端事件队列；当队列少于 2048 项才入队，达到上限时没有错误事件、失败状态或可见丢弃计数，只唤醒消费者。

证据：`source/client.cpp` 1599–1607 行。

**可能影响：** 如果回调生产速度持续超过客户端消费速度并填满队列，SDP、本地候选、ICE 状态或 Peer 状态等事件可能丢失，表现为信令/连接状态不前进或超时。  
**证据边界与优先级：** 这是确定存在的静默丢弃路径，但 2048 项门槛很高；没有高水位/拒绝计数时，不应把它作为个别用户连接卡住的首要解释。建议先补高水位与拒绝计数，再按证据决定是否调整策略。

### 5.3 几个不同的有界队列，不能混为一谈

连接建立期间有三条语义不同的信令/连接事件队列：

1. **Peer 尚未创建时的早期 SOAC 队列：** 最多缓存 128 条；超过上限会记录 `soac-early-queue-overflow` 并抛出明确连接错误，不是静默丢弃。
2. **远端 SDP 尚未应用时的 ICE candidate 暂存队列：** 最多 128 项；超过上限会抛出“远端 ICE 候选过多”。这是 `rtc_signaling.hpp` 中独立于早期 SOAC 的另一条队列。
3. **PeerConnection 回调事件队列：** 最多入队 2048 项；满时静默不入队，是上一节的低优先级待证风险。

诊断日志 writer 另有独立队列：`connection.log` 最多排队 512 行，超过会丢弃最旧行并置 dropped 标记。它不是信令/ICE 队列，但日志过量时可能丢掉最早阶段证据；缺少一行不能直接证明对应网络事件没有发生。

证据：`source/client.cpp` 1477–1492、1599–1607 行；`include/rtc_signaling.hpp` 47–51 行；`source/diagnostics.cpp` 35–47、155–166 行。

### 5.4 TURN 桥线程故障没有被上层主动消费

TURN TCP/TLS 桥运行线程会记录异常并置失败标记；`failed()` 可读取该标记。但连接路径没有检查该标记并把桥失败转换为当前 TURN/ICE attempt 的即时失败。

证据：`source/turn_stream.cpp` 68–70、179–185 行；`source/client.cpp` 1573–1593 行及所有 `turnBridges_` 使用点。

**可能影响：** TURN 桥已失败但 ICE 仍等待后续超时，造成 UI 长时间停在建立通道阶段，且错误分类不够直接。  
**证据边界：** 是否影响某次实际失败，须确认当次选了 TCP/TLS、桥错误事件早于超时，并且没有成功的另一条候选路径。

### 5.5 “Peer Connected”与“媒体就绪”未分离

进入 PeerConnection Connected 后，当前逻辑清除连接期限并切到 connected 状态；首帧/首音频并未成为此状态的条件。

证据：`source/client.cpp` 1261–1279、1665–1683 行。

**可能影响：** 信令、ICE、DTLS/SCTP 都已通，但主机没有开始发媒体、媒体轨道/编码协商异常或首帧未解码时，客户端仍可能显示连接成功或呈现黑屏。  
**证据边界：** 当前代码结构支持这种状态差异；需用 `rtc-peer-state`、首个 RTP/帧/解码/呈现事件确认具体失败。

### 5.6 数据通道事件另有较小队列门槛

PeerConnection 总事件队列和数据通道消息处理并非完全同一条容量限制。数据通道回调在待处理事件超过 124 项时提前返回；控制状态、质量 RPC 应答、屏幕信息等都可能受该门槛影响。

证据：`source/client.cpp` 1705–1747 行。

**可能影响：** 队列大量积压时，某些数据通道状态或应答事件不进入客户端处理。  
**证据边界：** 该路径需要先有严重事件积压；没有计数和现场日志时不能作为普通连接失败的首要解释。它与 PeerConnection `post()` 的 2048 项门槛是两种不同队列。

### 5.7 两秒 nomination 窗口的适用范围

2 秒计时从 controlling 端当前 `selected_pair` 发生变化时启动；route policy 开启时，Switch 会等该 pair 的 nomination 时间到，再请求最终提名。自定义 rank 只在 selected pair 尚未 nominated 时参与排序。提名完成后继续使用 RFC pair priority，因此自定义策略不会在之后自动把已提名的 relay 换成较低 RFC priority 的 LAN pair。

这解释了一个具体可能情形：relay pair 先可用，LAN candidate trickle/检查较晚，relay 已在等待窗口结束后获提名；后到的 LAN candidate 不一定触发自定义策略改道。若 LAN candidate 在提名前到达且检查成功，自定义 rank 才有机会影响选路。实际行为仍受候选到达、ICE role、检查结果和标准 nomination 流程影响，需看 pair 的创建/成功/提名/最终选择时间线。

证据：`agent.h` 55 行；`agent.c` 1063–1078、1126–1155、2586–2598 行。

## 6. 补充审阅：状态顺序、计时器与队列语义

### 6.1 RTP 可能早于 SCTP 和界面 connected

在当前 vendored/override libdatachannel 代码中，DTLS Connected 回调会启动 SCTP，并安排 `openTracks()`；若 SDP 有 application，PeerConnection 状态不会在 DTLS callback 里直接变为 Connected。SCTP Connected 回调才设置 PeerConnection Connected，并安排数据通道打开。YOUYOU 主程序收到 PeerConnection Connected 后才清除期限并将 UI 设为 connected。

所以应把时间线理解为：

```text
ICE connected
    → DTLS connected
        ├─ 启动 SCTP → SCTP connected → PeerConnection/UI connected
        └─ 安排音视频 Track 打开 → RTP 可能与 SCTP 建连并行/更早开始
```

这是代码允许的顺序；实际 Track callback、RTP 到达和首帧显示仍需运行日志证明。它修正了报告前版把媒体到达笼统放在所有连接阶段之后的说法，但不改变“Peer/UI connected 不等于首帧已显示”的结论。

证据：`scripts/dependency-overrides/src/impl/peerconnection.cpp` 247–261、349–371、1181–1193 行；`source/client.cpp` 1261–1279、1665–1683 行。

### 6.2 心跳与总连接期限是两个独立计时器

客户端收到 Socket.IO hello 后使用 `pingInterval + pingTimeout + 5000 ms` 作为信令心跳期限；缺省值代入后为 25,000 + 20,000 + 5,000 = 50,000 ms。每收到 WebSocket 包都会更新 `lastPacket_`。这条心跳只判断信令一段时间没有包；它不会续期 `deadline_`。因此信令包持续往返仍不能阻止从 WebSocket 打开后起算的 45 秒连接期限先到。

证据：`source/client.cpp` 634–635、1113–1117、1369–1389 行；`include/client.hpp` 111–112 行。

### 6.3 队列上限和超限行为分别处理

| 队列 | 容量/门槛 | 超限行为 | 连接诊断意义 |
|---|---:|---|---|
| Peer 创建前的 `earlySoac_` | 128 条 | 记 `soac-early-queue-overflow` 后抛连接错误 | 明确失败，不是静默丢弃 |
| 远端描述前的 `pendingCandidates_` | 128 项 | 抛“远端 ICE 候选过多” | 明确失败，不是静默丢弃 |
| Peer 回调 `inbox.events` | 2048 项上限 | `post()` 不入队，无拒绝计数 | 确定的静默丢弃路径；无队列高水位证据前属低优先级假设 |
| 数据通道解析事件 | 回调检查 `events.size()>124` | 直接返回，不投递该消息 | 独立于 2048 门槛，可能漏掉状态/RPC 消息；需严重积压证据 |
| `connection.log` 异步 writer | 512 行 | 删除最旧待写行，设置 dropped 标记 | 可能导致早期日志缺失；不能将缺行当作事件未发生 |

其中前两项是两个不同的 128 上限：一个缓存 Peer 创建前的整个 SOAC 包，另一个仅缓存远端描述前的 ICE candidate。诊断日志队列不是网络协议队列，也不会因日志行被丢弃而直接改变 ICE 状态。

证据：`source/client.cpp` 1477–1492、1599–1607、1724–1747 行；`include/rtc_signaling.hpp` 47–51 行；`source/diagnostics.cpp` 35–47、155–166 行。

## 7. “正在建立音频通道”文字的解释边界

在当前仓库中未找到字面文本“正在建立音频通道”。客户端连接阶段使用的文字包括“正在申请远控权限”“正在等待电脑协商应答”“网络通道已就绪，正在协商加密和数据通道”等。因此，如果该文字来自电脑端 UU 界面或其他版本界面，不能仅凭这句话断定是 Switch 音频轨道初始化失败。

当前 Switch 侧创建音频接收轨道的代码在 `source/client.cpp` 1669–1683 行；连接超时把 ICE 已建立但 Secure/Data channel 未就绪归为另一类，在 637–654 行。若电脑显示“控制中”而 Switch 黑屏，还要区分：

- PeerConnection 是否已 Connected；
- DTLS/SRTP 是否已完成；
- 音频/视频 RTP 是否到达；
- `Media` 是否收到并解码视频；
- 原生显示是否成功提交画面。

“电脑正在控制”本身不能证明 Switch 已收到视频，也不能证明音频轨道正常。

## 8. 现有诊断文件与事件判读

### 8.1 文件生成条件

`connection.log` 位于 Switch `sdmc:/switch/UURemote-ns/connection.log`；电脑构建通常写当前工作目录下的 `connection.log`。ICE 详情写到 `sdmc:/switch/UURemote-ns/diag/`，但 `ice_candidates.json` 和 `selected_path.txt` 只有在配置 `diag` 开启时才会生成。

证据：`source/diagnostics.cpp` 23–27、200 行附近；`source/client.cpp` 337–340、404–437 行。

因此，这两个文件不存在时，先看当次 `config.ini` 是否启用了 `diag`、程序实际使用的 SD/工作目录和文件写入错误。文件缺失不能单独证明没有 ICE candidate，也不能证明没有选择路径。

### 8.2 建议按时间顺序看事件

以下事件名来自当前源代码/RTC 事件映射；具体构建版本若不同，先核对版本标记。

| 阶段 | 重点事件 | 可支持的判断 |
|---|---|---|
| 信令/控制 | `signal-socketio-*`、`socket-ack-room-*`、`socket-ack-control-*` | 是否加入房间并取得控制应答 |
| SDP | `rtc-offer`、`soac-answer-received`、`soac-description-applied` | 是否收到并应用电脑的协商描述 |
| ICE 候选 | `ice-candidate-local/remote-*`、`ice-pair-created-*`、`ice-pair-sent/failed/succeeded-*` | 候选是否生成、交换以及检查成功/失败 |
| 选路/TURN | `ice-selected-*`、`turn-advertised-*`、`turn-selected-*`、`turn-allocated`、`turn-response-error-*`、`turn-stream-*` | 是否使用 TURN、服务器分配/认证或 TCP/TLS 桥是否报错 |
| ICE 状态 | `rtc-ice-state-*` | ICE 是否到 Connected/Completed |
| 加密/SCTP | `rtc-dtls-*`、`rtc-sctp-*` | DTLS/SRTP、SCTP 的启动/成功/失败位置 |
| Peer 状态 | `rtc-peer-state-*` | PeerConnection 整体是否 Connected/Failed/Closed |
| 媒体 | `video-*`、`audio-*`、解码与呈现事件 | 是否开始收包、解码并提交画面；必须看该版本实际支持的事件 |
| 截止期限 | `timeout-peer-state-*`、`timeout-ice-state-*`、`signal-timeout-*` | 45 秒期限到达时卡在哪个已知阶段 |

ICE 事件名称映射见 `source/rtc_diagnostics.cpp` 21–85 行；客户端超时和状态事件见 `source/client.cpp` 637–654、1247–1289 行。

### 8.3 地址隐私

`ice_candidates.json` 和 `selected_path.txt` 可能包含本地/远端候选地址与端口。对外分享前应遮盖公网 IP、局域网地址、端口、设备标识以及其他账号信息。`connection.log` 也应先检查是否含敏感路径或服务器信息。

## 9. 对“局域网为什么没自动连接”的直接回答

ICE 本身会尝试可用候选路径，LAN host candidate 成功时可以直接连接，不需要另行触发一个“切换到 LAN”的按钮。但当前能否走到 LAN 取决于以下条件：

1. 主机和 Switch 的局域网候选确实生成并通过信令交换；
2. 两侧 ICE 检查能够到达对端地址/端口；
3. 没有 `force_relay=true` 把策略限制为中继；
4. VPN、主机防火墙、虚拟网卡和路由规则没有把候选流量导向不可达接口；
5. 当前候选排序/nomination 最终选择了能持续传输的 pair。

因此，“两边有公网 IPv4”不保证一定直连，也不保证一定中继；“同一 LAN”也不保证一定直连。必须以 `ice-selected-*` 的路径及对应候选对成功事件确认。若日志显示选中 relay，接下来应核对 `force_relay`、候选对检查以及 TURN allocation；若显示 LAN/private pair 成功，则连接阶段的问题应转向 DTLS/SCTP/媒体首包，而不是继续归因 TURN。

## 10. 建议的验证和修复顺序

此处是建议，不代表本次已修改代码。

### 第一步：用一次失败日志定位阶段

按 7.2 表找到最后一个确定成功的阶段，并记录从 WebSocket open 到各阶段的耗时。至少要有：

- 构建版本和一次连接的完整时间线；
- 是否 `force_relay`；
- 候选对创建/发送/失败/成功事件及最终选中路径；
- TURN 传输类型、分配/认证/错误和桥接事件；
- ICE、DTLS、SCTP、Peer 状态；
- 首个视频/音频接收、解码、显示事件（若当前构建支持）。

在设备可用时，优先比较一次成功与一次失败；两次都不要同时改网络、VPN、路由器或画质设置。

### 第二步：优先评估两项可观测性缺口

1. 为 45 秒总期限加入阶段耗时/剩余预算记录；依据真实慢阶段数据，再决定是否拆成进度期限或是否允许阶段重置。不要无证据地直接放宽到很长，以免真正断连变成无限等待。
2. 对 PeerConnection 和数据通道事件队列记录高水位、拒绝数量及事件类别；在没有队列满证据前，不改变容量或调度策略。

### 第三步：审查 TURN 桥健康状态传播

对 TCP/TLS TURN 桥记录单调时钟错误时间，并将桥失败和 ICE attempt 关联。再用真实日志确认桥故障是否早于 ICE 超时。如果证明桥错误被上层忽略，再考虑将失败尽快传递到会话状态机，同时避免误报另一个有效候选路径也失败。

### 第四步：区分连接成功和媒体就绪

保留 PeerConnection Connected 语义，另行记录“首个音频包”“首个完整视频帧”“首帧解码完成”“首帧显示提交”。若 Peer Connected 但媒体事件缺失，才能把排查边界收敛到主机发送、RTP/SRTP、轨道或解码呈现路径。

## 11. 当前结论与限制

### 已确认

- 当前项目版本为 0.3.74，代码使用 UU 信令 + WebRTC ICE/DTLS/SCTP 的分阶段连接流程。
- LAN 依赖 ICE 候选与 pair 检查；Switch 有候选路径偏好逻辑，但 `force_relay` 可强制中继。
- TURN TCP/TLS 通过 Switch 本地桥接到 UDP ICE；桥接错误状态没有明显的客户端轮询消费点。
- 连接总期限是从信令 WebSocket 打开后计算的 45 秒，直到 PeerConnection Connected 才清除。
- 独立信令心跳按 hello 参数设置并随每个入站包刷新，不会续期 45 秒连接总期限。
- ICE 策略有 2 秒 nomination 等待；自定义 rank 在 pair nominated 后不再覆盖 RFC priority。
- 早期 SOAC、pending ICE candidate、Peer 回调、数据通道和日志 writer 有不同的容量/超限语义。
- Peer Connected 不等于首帧已显示。
- ICE 两个辅助文件受 `diag` 配置控制。
- 当前仓库没有完全相同的“正在建立音频通道”字面文案。

### 尚未确认

- 某一次失败是否真的走了 TURN，或是否因 VPN 被拒绝；
- 是否生成并交换了 LAN host candidates；
- 是否有候选对成功但后来 DTLS、SCTP 或媒体层失败；
- 45 秒期限是否在失败案例中实际先于进度完成；
- 任一事件队列是否在现实连接中达到上限；
- Switch 无画面时是主机没有发视频、RTP/SRTP 未交付、解码失败还是显示提交失败。

### 总结

现有代码表明“局域网连接会由 ICE 尝试”，但它不是无条件 LAN 优先且保证成功的专用直连流程。VPN/TURN 是可能路径因素之一，不能仅凭电脑处于 VPN 状态认定服务器拒绝。若电脑端显示控制中而 Switch 无画面，代码又允许 PeerConnection Connected 先于首帧状态成立，因此首要判断应是连接时间线最终停在 ICE、DTLS/SCTP 还是媒体/首帧阶段。拿到对应失败日志后，才能把本报告中的风险收敛为实际根因。

## 12. 主要源码索引

| 文件 | 重点范围 | 内容 |
|---|---:|---|
| `source/client.cpp` | 637–654 | 45 秒期限的阶段化失败分类 |
| `source/client.cpp` | 1098–1117 | 加入房间、WebSocket 打开、启动期限 |
| `source/client.cpp` | 1369–1389 | 入站包刷新心跳、Socket.IO hello 设置心跳期限 |
| `source/client.cpp` | 1223–1289 | 本地 SDP/候选、ICE 与 Peer 状态 |
| `source/client.cpp` | 1415–1452 | 房间/控制应答、发送串流参数 |
| `source/client.cpp` | 1499–1608 | ICE/TURN 配置、force relay、桥接、Peer 回调事件队列 |
| `source/client.cpp` | 1621–1757 | 音视频接收轨道、数据通道建立及回调 |
| `source/client.cpp` | 1758–1899 | 远端 SDP/候选应用及诊断 |
| `source/client.cpp` | 337–437 | `diag` 开关及 ICE 详情文件写入 |
| `source/client.cpp` | 1705–1747 | 数据通道事件队列门槛 |
| `source/client.cpp` | 1477–1492 | Peer 创建前 SOAC 队列及明确溢出错误 |
| `source/client.cpp` | 2074–2095 | 超时清理与 TURN bridge 生命周期 |
| `source/client.cpp` | 2362–2395 | 选择路径、RTT 和传输状态更新 |
| `include/rtc_signaling.hpp` | 8–61 | 远端 SDP/candidate 顺序与候选暂存 |
| `source/turn_stream.cpp` | 43–185 | TURN TCP/TLS 本地桥接、错误和 failed 标记 |
| `source/rtc_diagnostics.cpp` | 21–85 | ICE/TURN/DTLS/SCTP 事件映射 |
| `scripts/dependency-overrides/deps/libjuice/src/agent.c` | 134–147、1126–1155、2564–2598 | Switch 路由策略、候选偏好及提名时机 |
| `source/diagnostics.cpp` | 23–27、200–242 | 连接日志位置和诊断写入 |
| `source/diagnostics.cpp` | 35–47、155–166 | 512 行日志队列及丢旧行行为 |
| `scripts/dependency-overrides/src/impl/peerconnection.cpp` | 247–261、349–371、1181–1193 | DTLS/SCTP/Track/Peer 状态推进顺序 |
| `scripts/dependency-overrides/deps/libjuice/src/agent.h` | 55 | 2000 ms nomination 窗口常量 |

