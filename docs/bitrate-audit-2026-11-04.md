# YOUYOU Remote NS 码率链路审查报告

> 版本：0.3.75（canonical 工作树，含未提交 diagnostic 修改）  
> 审查日期：2026-11-04  
> 审查对象：`C:/uu/current/YOUYOU-Remote-ns`  
> 约束：只读审查；未修改源码；PC 端不可修改。  
> 相关日志：`%USERPROFILE%/Downloads/rr/connection.log*`

---

## 1. 执行摘要

当前码率问题的核心不是“字段 8 不够大”，也不是“PC 绝对只能 8 Mbps”。

证据表明：

1. 官方手机 APK 在同一 PC、同一高运动场景可以达到 **50 Mbps**；
2. 当前 Switch 固定 50 Mbps REMB 实测只有中位约 **11 Mbps**、最高约 **15 Mbps**；
3. 当前 Switch 没有 transport-cc 发送路径；
4. 当前固定 REMB 的 RTCP sender SSRC 使用了远端视频 SSRC，可能被 PC 当作无效/自身反馈；
5. 当前 PC `streamer.dll` 有 transport-cc/REMB/BWE；官方 APK 也有；
6. 因此当前客户端很可能缺少“有效的接收端带宽反馈”，而不是缺少字段 8。

**优先级最高的问题：**

```text
P0：没有 transport-cc SDP 与发送路径
P0：REMB sender SSRC 复用远端 SSRC，疑似无效
```

在这两个问题解决前，继续调 `max_custom_bitrate`、改成自适应 REMB、或把全部档位改成 Q6，都不能确定提高实际码率。

---

## 2. 代码路径总览

### 2.1 字段 8：自定义上限

`source/client.cpp` / `source/protocol.cpp`：

```text
quality == 6
  → capture field 8 = max_custom_bitrate
  → bps = Mbps * 1,000,000
```

连接请求和在线 capture setting 都携带该字段。

**结论：字段 8 是上限，不是目标码率。**

### 2.2 当前 Q6 REMB

当前代码有两处发送 Q6 REMB：

#### `commitQuality()`

```cpp
if(q.quality==6 && q.cap>0 && video_ && video_->isOpen() && rembAvailable_) {
    video_->requestBitrate(target);
    bitrateFeedbackSent_ = true;
    rembTargetBps_ = target;
}
```

#### `applyBitrateCap()`

```cpp
if(streamQuality_==6 && bitrateFeedbackSent_ && rembTargetBps_==target) return;
...
video_->requestBitrate(target);
```

这说明：

- Q6 不再是“完全不发 REMB”；
- 但仍然只有固定 REMB；
- 没有 transport-cc；
- 两处路径没有统一的节流设计，存在重复发送可能。

### 2.3 当前没有 transport-cc

视频 SDP 构造只调用：

```cpp
v.addH264Codec(...)
```

或 H.265 对应 codec。

没有：

```text
a=rtcp-fb:96 transport-cc
a=extmap:N http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
```

现有 `transport_feedback` 代码虽然存在，但：

```text
UU_TWCC_FOUNDATION=OFF
UU_TWCC_OBSERVE_CLIENT=OFF
UU_TWCC_SEND_EXPERIMENT=OFF
```

`Client::refreshFeedbackObservation()` 只创建 observation-only：

```text
twcc-observation-attached-send-disabled
```

没有：

```text
startSending()
poll()
```

---

## 3. REMB SSRC 问题（重点）

### 3.1 相关代码

`external/libdatachannel/src/rtcpreceivingsession.cpp`：

```cpp
case Message::Binary:
    ...
    mSsrc = rtp->ssrc();

case Message::Control:
    if (SR/RR)
        mSsrc = rr->senderSSRC();
```

`requestBitrate()`：

```cpp
bool RtcpReceivingSession::requestBitrate(unsigned int bitrate,
                                           const message_callback &send) {
    mRequestedBitrate.store(bitrate);
    pushREMB(send, bitrate);
    return true;
}
```

`pushREMB()`：

```cpp
remb->preparePacket(mSsrc, 1, bitrate);
remb->setSsrc(0, mSsrc);
```

### 3.2 问题

`mSsrc` 是收到的 RTP/RTCP 中的 **远端 PC SSRC**。

但 `pushREMB` 同时把它作为：

```text
RTCP sender SSRC
REMB media SSRC
```

正确语义应是：

```text
REMB sender SSRC = Switch 接收端自己的 SSRC
REMB media SSRC  = PC 视频 SSRC
```

当前实际发出：

```text
sender SSRC = PC 视频 SSRC
media SSRC  = PC 视频 SSRC
```

### 3.3 可能后果

- PC 可能不把该 REMB 当作来自接收端的有效反馈；
- PC 可能忽略或丢弃；
- Switch 日志仍会显示 `bitrate-cap-feedback-custom-bps-...`，因为本地 `send` 成功；
- 实际码率不响应。

### 3.4 证据边界

这是**高置信度代码缺陷**，但尚未用 RTCP 抓包确认 PC 是否因此忽略。

需要抓：

```text
REMB sender SSRC
REMB media SSRC
```

如果两者都等于 PC 视频 SSRC，基本可确认语义不符合 RFC。

---

## 4. transport-cc 缺口

### 4.1 当前状态

`transport_feedback` 代码已有：

```text
ArrivalWindow
FeedbackCoordinator
FeedbackSender
FeedbackAttachment
NegotiationCapabilities
```

但默认未启用，且当前 Client 只观察。

### 4.2 官方 APK / PC 静态证据

官方手机 APK `libstreamer.so`：

```text
transport-cc
remote_estimator_proxy
receive_side_congestion_controller
goog-remb
Incoming REMB
```

PC `streamer.dll`：

```text
transport-cc
remote_estimator_proxy
receive_side_congestion_controller
Incoming REMB
max_bitrate_limit
SetBitRate
```

官方 APK 能到 50M，说明 PC 具备并可能使用 transport-cc/BWE。

### 4.3 结论

当前 Switch 没有复制官方控制端的 transport-cc 路径。  
固定 REMB 即使 SSRC 正确，也可能不如 transport-cc；如果 SSRC 还错，则固定 REMB 基本无效。

---

## 5. 2026-11-04 日志实测

日志目录：

```text
%USERPROFILE%/Downloads/rr/connection.log
%USERPROFILE%/Downloads/rr/connection.log.1
%USERPROFILE%/Downloads/rr/connection.log.2
```

### 5.1 目标 50 Mbps 阶段

只提取可解析的 `stream-rate`：

```text
target 50 Mbps
n = 75
median ≈ 10.9 Mbps
max ≈ 14.9 Mbps
>= 20 Mbps = 0
```

特征：

```text
loss=0.00
nack=0
rtx=0
host-known=0
host-mbps=0
ack=0
applied=0
```

### 5.2 REMB 日志

日志中可见：

```text
bitrate-cap-feedback-custom-bps-50000000
```

说明客户端确实调用了 `requestBitrate(50,000,000)` 并成功进入发送路径。

但：

- 没有 PC 实际接受/SetBitRate 日志；
- 没有 transport feedback 发送；
- 无法从客户端日志判断 PC 是否接受 REMB。

### 5.3 解释

```text
50M REMB 已发
实际 RTP 仍 10–15M
无丢包/NACK/RTX
```

这更支持：

- PC 没有接受/使用该 REMB；
- 或 REMB SSRC 错误；
- 或 PC 需要 transport-cc 才会提高 BWE；
- 或编码/内容限制，但官方同场景 50M 降低了该可能。

---

## 6. 发现清单与严重度

### P0：REMB sender SSRC 错误

- 位置：`external/libdatachannel/src/rtcpreceivingsession.cpp`
- `mSsrc` 来自远端 RTP/RTCP；
- `pushREMB` 将其作为本地 sender SSRC；
- 可能使 REMB 被 PC 忽略；
- 影响：固定 REMB 可能完全无效。

### P0：缺少 transport-cc

- 本地 SDP 没有 transport-cc/extmap；
- `transport_feedback` 默认 OFF；
- Client 没有 `startSending/poll`；
- 影响：无法复制官方 APK 的接收端 BWE。

### P1：Q6 REMB 双路径

- `commitQuality()` 与 `applyBitrateCap()` 都可发送；
- `commitQuality()` 未检查 `rembTargetBps_`；
- 修改非码率设置也可能触发重复 REMB；
- 影响：不一定是码率低主因，但反馈不干净。

### P1：自定义上限 100M / UI 50M

- `maxBitrateMbps_ = configInt(...,0,100)`；
- `quality-settings` 校验 `maxBitrate > 100` 拒绝；
- 当前 UI 自定义档通常 8–50M；
- 官方手机可设置 500M；
- 对本次 50M 不是直接瓶颈，但产品能力不一致。

### P1：PC 默认 8100 无客户端覆盖

- PC `streamer.dll` 有 `PcFactoryDefaultBitrates max:8100`；
- 但官方同 PC 可达 50M，说明不是绝对硬上限；
- 无有效 transport-cc/REMB 时可能回落；
- 不能把 8100 单独当归因。

### P2：原生预设没有每档 Mbps 上限

- Q1–Q4 不发字段 8、不发 REMB；
- PC 用原生 quality 映射；
- 不能通过原生预设定义 5/12/20/30 Mbps；
- 要每档明确上限，只能 Q6+field8（或 PC 侧表，PC 不可改）。

### P2：日志/可观测性不足

- `host-known=0`、`applied=0`；
- 没有 PC 实际 SetBitRate 反馈；
- 不能区分“PC 拒绝 REMB”与“编码器按内容输出”。

---

## 7. 根因排序

结合代码、官方 APK 50M、Switch 50M REMB 10–15M：

1. **REMB sender SSRC 错误**（高置信度，可能直接导致 REMB 无效）；
2. **没有 transport-cc**（官方路径差异）；
3. **Q6 只有固定 REMB，没有接收端 BWE**；
4. **PC 默认 8100 在无有效反馈时回落**；
5. **编码/内容/场景**（但官方同场景 50M，降低权重）；
6. **字段 8 不够大**（50M 已远超实际，低权重）。

---

## 8. 建议的修复顺序

### 阶段 0：抓包确认 REMB SSRC

在 Switch 发出 50M REMB 时抓 RTCP：

```text
REMB sender SSRC
REMB media SSRC
```

判定：

- `sender SSRC == PC video SSRC` → 确认当前代码问题；
- `sender SSRC` 是独立本地 SSRC → 再看 PC 是否接受。

### 阶段 1：修正 REMB SSRC（最小实验）

不要直接把 `mSsrc` 全局改成另一个值，因为 RR 使用它作为 report block。

正确方向：

```text
local RTCP sender SSRC  = Switch 自己的非零 SSRC
remote media SSRC       = PC 视频 SSRC
```

REMB：

```text
sender SSRC = local
media SSRC  = remote video
```

RR：

```text
report block SSRC = remote video
```

建议单独实现 REMB 发送，或让 `PrimaryVideoRtcpSession` 维护 local/remote 两个 SSRC，而不是复用基类 `mSsrc`。

验证：

- RTCP 抓包 SSRC 正确；
- PC 实际码率是否上升；
- 若无变化，进入 transport-cc。

### 阶段 2：接入 transport-cc

如果官方 APK 抓包确认 transport-cc，按以下顺序：

1. 本地 SDP：
   ```text
   a=rtcp-fb:96 transport-cc
   a=extmap:N http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
   ```
2. 实验构建：
   ```text
   UU_RECEIVE_TIMESTAMPS=ON
   UU_TWCC_FOUNDATION=ON
   UU_TWCC_OBSERVE_CLIENT=ON
   UU_TWCC_SEND_EXPERIMENT=ON
   ```
3. 先用 observation-only 验证：
   ```text
   twcc-observation-accepted-...
   missing-extension=0
   ```
4. 接 `FeedbackAttachment::startSending()`：
   - local sender SSRC；
   - remote media SSRC；
   - `FeedbackSendPolicy`；
   - `SharedRtcpBudget`。
5. 在 `refreshStats()` / worker tick 调用 `poll()`。
6. `disconnect()` 调用 `stop()` 并清 handler。
7. 与 fixed REMB 做 A/B。

详细 transport-cc 方案见：

```text
C:/uu/archive/snapshots/youyou-v0372/audit-20261027/TRANSPORT-CC-IMPLEMENTATION-PLAN.md
```

### 阶段 3：清理 Q6 REMB 双路径

建议：

- `commitQuality()` 只更新目标/保存配置；
- `applyBitrateCap()` 统一负责 REMB 发送；
- 或保留 commit 立即发送，但增加 `rembTargetBps_` 检查，避免重复。

### 阶段 4：档位/上限

如果希望每档明确 Mbps：

- 全部 Q6 + field8；或
- PC 端 quality→SetBitRate 表（但 PC 不可改，无法采用）。

如果希望支持 500M：

- 放宽 `maxBitrateMbps_` 和 `quality-settings` 上限；
- 但这不会自动提高实际码率，仍需反馈/PC 上限。

---

## 9. 测试与验收

### 9.1 抓包验收

官方 APK 与 Switch 对比：

```text
SDP:
  transport-cc
  extmap ID
  goog-remb
  b=AS / x-google

RTCP:
  REMB sender/media SSRC
  transport feedback 包数
  RR/NACK/RTX

RTP:
  中位/峰值 Mbps
  丢包/抖动/RTT
```

### 9.2 代码测试

- REMB SSRC 单元测试：
  - sender SSRC 不等于 media SSRC；
  - RR report block 仍使用 remote media SSRC。
- transport feedback：
  - SDP 协商 Ready；
  - `ArrivalWindow` / `FeedbackCoordinator` / `FeedbackSender` 测试；
  - 发送失败不影响媒体。
- 回归：
  - 720p/1080p；
  - H.264/H.265；
  - 无 REMB 时 fallback；
  - 断线重连清状态。

### 9.3 真机验收

同 PC、同 720p60 H265、高运动窗口晃动、60 秒以上：

| 组 | 控制端 | 期望 |
|---|---|---|
| A | 官方 APK 50M | 50M 基线 |
| B | Switch fixed 50M REMB | 当前 10–15M |
| C | Switch + 正确 REMB | 是否上升 |
| D | Switch + transport-cc | 接近官方 |

只有 D 显著优于 B，且无明显丢包/NACK/RTX 增加，才可认为 transport-cc 路线有效。

---

## 10. 风险

- PC 未必真的使用 transport-cc；
- 现有 transport feedback 是实验代码，未真机验证；
- SSRC/CNAME 配置错误可能被 PC 忽略；
- 与 RR/NACK 共享 RTCP 预算，可能产生竞争/重复；
- `UU_RECEIVE_TIMESTAMPS` 改变 Message ABI，依赖需一致；
- 高码率下 transport feedback 有 CPU 成本；
- 若 PC 有内部硬上限，客户端无法保证 50M；
- 不能把 50M 上限当成稳定输出。

---

## 11. 结论

当前代码的主要问题不是字段 8，而是：

```text
1. REMB sender SSRC 使用了远端 SSRC，可能无效
2. 没有 transport-cc SDP/发送路径
```

这两点与以下事实一致：

```text
官方 APK 50M
Switch fixed 50M REMB 10–15M
```

建议先抓包确认 REMB SSRC，再修正本地/远端 SSRC；  
如果官方确认走 transport-cc，则启用并接入现有 transport_feedback 发送器。  
在 PC 不可修改的前提下，这是最可能恢复官方高码率控制端行为的路线。

本次仅审查和生成报告，未修改源码。
