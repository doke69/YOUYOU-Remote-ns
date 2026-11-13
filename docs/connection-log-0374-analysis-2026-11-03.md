# 0.3.74 新日志独立分析

输入：`%USERPROFILE%/Downloads/uu全部文件/3.74/connection.log`。
大小：76126 字节。SHA-256：`6988f4cf39e295851118a899ede5aae7088318b9b148ef988c0fbc267df95c16`。
文件头和 7 次启动标记均为 `0.3.74-beta`；无配套 NRO，不能确认二进制具体源码哈希。仅分析连接日志，未读取同目录登录/配对文件，未修改产品代码。

## 核心结论

本日志直接支持原连接分析报告的“未取得有效远端协商应答”分支。10 次完整超时中，全部是 SOAC 事件计数 0、拒绝计数 0，remote description 尚未应用，没有已选候选对。

因此重点是 **Offer 发送后，电脑是否生成并经房间路由送回 SOAC Answer，以及客户端在 Socket.IO 分发之前是否完整识别该消息**。尚不能确定是 PC、服务转发还是客户端分帧/事件识别造成；不能把 root cause 写成已证实的服务器故障。

## 全文件统计

| 事件 | 数量 | 意义 |
|---|---:|---|
| 应用启动 | 7 | 多次重启后仍有相似失败 |
| joining | 18 | 18 次加入房间尝试 |
| soac-offer-sent | 17 | 17 次走到本地 Offer 发送完成；不是 PC 收到的 ACK |
| forward-setting-summary | 17 | 每次发 Offer 的尝试均看到电脑/服务传来的能力摘要；部分下行信令可到达 |
| turn-allocated | 34 | 17 次 Peer 尝试中每次两个 TURN allocation 成功；UDP/TCP 均有成功样例 |
| turn-auth-challenge-401 | 34 | 随后有分配成功，不能当作最终认证拒绝 |
| 完整连接超时 | 10 | 均在 WebSocket 打开约 45–46 秒后触发 |
| signal-timeout-soac-events-0-rejected-0 | 10 | 完整超时均未 dispatch 到 SOAC，非 inspectSoac 会话匹配拒绝 |
| signal-timeout-waiting-description | 10 | 未应用远端 SDP |
| Socket.IO disconnect | 2 | 服务端协议断开通知，日志没有提供具体原因 |
| ICE Failed | 1 | 约 22 秒时 ICE/Peer 失败，无已选候选对 |
| 加房间阶段异常 | 1 | 尚未打开信令即出现 runtime error，缺少细分原因，不能混同为 SDP 超时 |
| 其他未完整结束的尝试 | 4 | 随后重启/日志结束，没有完整结果，不能都计算成 45 秒超时 |

全文件没有 SOAC 入站/Answer 收到/描述应用成功/会话拒绝记录，没有 Signaling Stable、Peer Connected、DTLS/SCTP Connected 或已选路径成功记录。没有 TURN bridge failed 或 authentication-failed 记录。

## 代表性时间线

### UDP：第 53–111 行

- 1793675995：joining/signaling；
- 1793675996：force_relay=0、选择 UDP、发送 Offer 和候选；
- 同秒：收到 forward-setting，两个 TURN allocation 成功，候选收集结束；
- 1793676040：Peer=Connecting、ICE=Checking；SOAC=0、rejected=0；waiting-description；没有 selected pair。

这不是“TURN 无法分配”；在远端描述尚未应用时，不能把 ICE Checking 单独解释成双方候选网络不可达。

### TCP：第 137–204 行

- 1793676042：signaling；
- 1793676043：选择 TCP，两个 TCP stream connected，随后两个 TURN allocation 成功；
- 1793676088：同样 SOAC=0、waiting-description 超时，没有 selected pair。

UDP/TCP 更换没有改变失败阶段，进一步降低“只修 TURN 传输类型即可解决”这一假设的优先级。只证明 Switch 一侧分配成功，不证明电脑侧中继、双向路径、凭据或所有网络环节都正常。

### 两类额外失败

- 第 459、771 行是 `signal-socketio-disconnect`，后续统一映射为 `failure-signal-rejected`。这个分类不能被解释为认证拒绝或 VPN 被封；Socket.IO 主动断开与 connect-error 是不同原始事件，日志此处已区分。
- 第 873–879 行是 ICE/Peer 从 Checking/Connecting 到 Failed，随后关闭；仍没有 selected pair，且此前无远端 SDP 应用成功证据。这一次不是 45 秒期限先到。

## 与原报告逐项对应

| 原报告观点 | 本日志支持程度 |
|---|---|
| 45 秒总期限覆盖多个阶段 | 直接支持，10 次约 45–46 秒超时 |
| 应先区分未收到描述与 ICE/DTLS/SCTP 失败 | 直接支持，10 次明确为 waiting-description |
| VPN 会导致 TURN 拒绝 | 不支持；本机 UDP/TCP allocation 均成功，也没有 VPN 状态证据 |
| TURN 桥故障未传播造成久等 | 没有支持；无桥失败，且当前补丁的 Peer Connected 条件未满足 |
| force_relay 排除 LAN | 本次均 force_relay=0，不是强制 relay 的案例 |
| 两秒提名导致 LAN 选路问题 | 未到有已选候选对的阶段，没有证据支持 |
| 队列满丢掉状态/应答 | 没有高水位/拒绝证据；本日志缺少本轮新增队列计数，不能确认或完全排除 |
| 首帧解码/显示未就绪 | 尚未完成远端协商，不能以这份日志认定解码或显示错误 |

## 对上一轮代码线索的修正

当前 `dispatch()` 在调用 inspectSoac/handleSoac 之前就增加 soacEvents_。本日志 10 次完整失败均为 0，因此不能把这些失败归为该校验器的 ICE ID 不匹配、H.265 参数拒绝或 gzip_sdp 进入 handleSoac 后遭拒绝。

但 0 只代表**没有分发到完整的 SOAC 事件**，并不严格等于 TLS/WebSocket 从未收到任何相关字节。若二进制附件没收齐、事件名不被识别或其他分发前兼容问题，同样可能保持 0。当前 `packet()` 不重建附件和 `gzip_sdp` 未支持仍是独立代码缺口，但没有本次应答形态，不能认定其已命中。

控制请求 streamer_data 结构与本地 Linux 参考存在差异、电脑连接记录可能尚未正确建立，仍属于值得核实的上游发送侧线索；forward-setting 到达不等于主机已将 Offer 绑定到当前 ICE 会话。不能只改字段或增加固定 sleep 就宣布解决。

## 当前源码修改是否已被这份日志验证

没有。文件中存在先前的 ICE/SOAC 阶段诊断，但没有这轮新增的 connect-stage/event-queue/media-first 标记；同一个 0.3.74-beta 标签不能唯一标识本轮二进制。本日志能够验证原报告的阶段判断，不能用于验收刚新增的桥失效恢复逻辑。
