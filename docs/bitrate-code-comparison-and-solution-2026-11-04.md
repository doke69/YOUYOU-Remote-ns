# YOUYOU Remote NS：码率链路代码对比与解决方案

> 审查日期：2026-11-04  
> 当前源码：`C:/uu/current/YOUYOU-Remote-ns`  
> 版本：0.3.75（canonical 工作树，含未提交 diagnostic 修改）  
> 约束：PC 端不可修改；只读审查；未修改源码。  
> 样本：`uuyc_4.44.0.apk`、`uuyc_4.44.0 (1).exe`

---

## 1. 结论摘要

当前码率上不去的主要原因不是字段 8 不够大，而是当前 Switch 客户端与官方控制端使用的**反馈链路不同**。

官方手机 APK：

```text
field8 自定义上限
+ 完整 WebRTC 接收端
+ transport-cc / REMB / BWE
+ PC SetBitRate
→ 同 PC 可达 50 Mbps
```

当前 Switch：

```text
field8 自定义上限
+ 固定 REMB
+ 没有 transport-cc
+ 没有接收端 BWE
→ 固定 50M REMB 实测 10–15 Mbps
```

此外当前 REMB 实现还有两个代码级问题：

1. **RTCP sender SSRC 使用远端 PC SSRC**；
2. **REMB 以单包发送，不是 compound RR + SDES + REMB**，且没有确认 reduced-size RTCP 协商。

因此优先级是：

```text
P0：修正 REMB SSRC 与 compound RTCP
P0：接入 transport-cc SDP 与发送
P1：清理 Q6 REMB 双入口
P1：确认本地上限/档位语义
```

---

## 2. 样本身份

```text
uuyc_4.44.0.apk
SHA-256: 87b2abb3765300d59b108d22fdaa421a8e25b5e37c0cb40daa854cb17b118918

uuyc_4.44.0 (1).exe
SHA-256: 4f1b86d768478ddb923ca13c19c9880976549edc0d26b3c8067c87f7fb4c5379

uuyc_4.44.0.exe
SHA-256: 4f1b86d768478ddb923ca13c19c9880976549edc0d26b3c8067c87f7fb4c5379
```

`(1).exe` 与旧 exe 完全相同，不是新版本；APK 也与上次分析相同。

---

## 3. 官方 APK/PC 的证据

### 3.1 官方 APK

DEX：

```text
QualityBitrate(fastBitrate, generalBitrate, hdBitrate, blurayBitrate)
max_custom_bitrate=
custom_bitrate
custom_bitrate_limit
FF_CAPTURE_SETTING_CUSTOM_BITRATE
```

native `libstreamer.so`：

```text
transport-cc
goog-remb
remote_estimator_proxy
receive_side_congestion_controller
Incoming REMB
```

TV/PC native 还可见：

```text
GetRecommendBitRate
GetQualityBitRateDiscrete
GetDefaultBitRate
GetMinBitrate
```

### 3.2 PC `streamer.dll`

```text
max_bitrate_limit applied, limit={} bps
SetBitRate ... min_bps ... max_bps ... start_bps ... max_probe_bps ... adaptive_bitrate
Incoming REMB
transport-cc
remote_estimator_proxy
receive_side_congestion_controller
```

PC Protobuf：

```text
CaptureSettingRequest.max_custom_bitrate field 8
CaptureSettingSync.max_custom_bitrate field 4
```

### 3.3 官方行为总结

```text
APK 控制端
  ├── field8 / quality
  ├── 接收 RTP
  ├── 读取 transport-wide sequence
  ├── 生成 transport-cc / REMB
  └── 接收端 BWE
          ↓
PC streamer.dll
  ├── max_bitrate_limit
  ├── transport-cc / Incoming REMB
  ├── SetBitRate(min,start,max,probe,adaptive)
  └── 编码器
```

关键点：

- 官方不是“只把字段 8 调大”；
- 官方控制端持续提供带宽反馈；
- PC 的 `SetBitRate` 根据 BWE 调整，而不只是读字段 8。

---

## 4. 当前 Switch 源码路径

### 4.1 字段 8

`source/client.cpp` / `source/protocol.cpp`：

```cpp
// connectOptions
if (quality == 6)
    integer(capture, 8, customMbps * 1000000u);
```

在线请求同样写字段 8。

**结论：字段 8 语义正确，是上限而不是目标码率。**

### 4.2 Q6 固定 REMB

`commitQuality()`：

```cpp
if(q.quality==6 && q.cap>0 && video_ && video_->isOpen() && rembAvailable_) {
    video_->requestBitrate(target);
    bitrateFeedbackSent_ = true;
    rembTargetBps_ = target;
}
```

`applyBitrateCap()`：

```cpp
if(streamQuality_==6 && bitrateFeedbackSent_ && rembTargetBps_==target) return;
...
video_->requestBitrate(target);
```

### 4.3 `requestBitrate` 实现

`external/libdatachannel/src/rtcpreceivingsession.cpp`：

```cpp
bool RtcpReceivingSession::requestBitrate(unsigned int bitrate,
                                           const message_callback &send) {
    mRequestedBitrate.store(bitrate);
    pushREMB(send, bitrate);
    return true;
}
```

```cpp
void RtcpReceivingSession::pushREMB(...) {
    remb->preparePacket(mSsrc, 1, bitrate);
    remb->setSsrc(0, mSsrc);
}
```

`mSsrc` 来源：

```cpp
mSsrc = rtp->ssrc();
```

即 PC 视频 SSRC。

### 4.4 本地 SDP

当前视频 SDP 只走：

```cpp
v.addH264Codec(...)
v.addH265Codec(...)
```

没有：

```text
a=rtcp-fb:96 transport-cc
a=extmap:N http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
```

### 4.5 已有 transport feedback 代码

源码存在：

```text
ArrivalWindow
FeedbackCoordinator
FeedbackSender
FeedbackAttachment
NegotiationCapabilities
```

但：

```text
UU_TWCC_FOUNDATION=OFF
UU_TWCC_OBSERVE_CLIENT=OFF
UU_TWCC_SEND_EXPERIMENT=OFF
```

`refreshFeedbackObservation()` 只：

```text
twcc-observation-attached-send-disabled
```

没有：

```text
startSending()
poll()
```

---

## 5. 官方 vs 当前：逐项代码对比

| 环节 | 官方 APK/PC | 当前 Switch | 影响 |
|---|---|---|---|
| 字段 8 | `max_custom_bitrate` | Q6 写字段 8 | 语义正确 |
| 编码器控制 | `SetBitRate(min,start,max,probe,adaptive)` | 无 | 当前不能控制 start/probe |
| 带宽反馈 | transport-cc + REMB + BWE | 固定 REMB | 官方持续估计，当前固定值 |
| SDP | transport-cc/extmap | 无 | PC 不走 transport-cc |
| REMB sender SSRC | 本地 RTCP SSRC | `mSsrc` = PC SSRC | 可能被 PC 忽略 |
| REMB media SSRC | PC 视频 SSRC | PC 视频 SSRC | 这一项相同 |
| REMB 封装 | compound RR+SDES+feedback | 单 REMB | 可能不符合 compound 规则 |
| REMB 时机 | 持续 BWE | 连接/上限变化一次 | 不能跟随网络 |
| 原生档 | QualityBitrate/码率表 | 不发字段 8 | 每档不可控 |
| 上限 | 可 500M | 代码 100M | 产品能力不一致 |

---

## 6. 关键代码问题

### P0-1：REMB sender SSRC 错误

当前：

```text
sender SSRC = mSsrc = PC 视频 SSRC
media SSRC  = mSsrc = PC 视频 SSRC
```

正确：

```text
sender SSRC = Switch 本地 RTCP SSRC
media SSRC  = PC 视频 SSRC
```

风险：

- PC 可能把 REMB 当作自身包或无效反馈；
- 本地日志仍显示发送成功；
- 实际码率不响应。

### P0-2：REMB 不是 compound RTCP

`pushREMB` 只创建一个 `RtcpRemb` 消息：

```text
[REMB]
```

没有：

```text
RR
SDES(CNAME)
REMB
```

RFC 3550 要求 compound RTCP；只有协商 reduced-size RTCP 才可单独发送。  
当前 SDP 没有看到 `a=rtcp-rsize` 的明确协商。

更直接的证据是，当前 libdatachannel 明确不支持 Reduced-Size RTCP：

```cpp
// external/libdatachannel/src/description.cpp
// Remove rtcp-rsize attribute as Reduced-Size RTCP is not supported (see RFC 5506)
reciprocated.removeAttribute("rtcp-rsize");
```

所以当前代码一边移除 `a=rtcp-rsize`，一边发送单包 REMB，二者是矛盾的。  
风险：

- PC 可能直接丢弃；
- 与官方完整 WebRTC 的 compound feedback 不同。

### P0-3：mSsrc 可能为 0

`commitQuality()` 的 Q6 路径只检查：

```cpp
video_ && video_->isOpen()
```

没有检查是否收到过 PC RTP。  
若首包 RTP 尚未到：

```text
mSsrc = 0
REMB sender = 0
REMB media  = 0
```

之后 `bitrateFeedbackSent_ = true`，`applyBitrateCap()` 看到同一 target 不再重发，可能永久保留错误 REMB。

### P1-1：Q6 REMB 双入口

`commitQuality()` 与 `applyBitrateCap()` 都可发送 Q6 REMB。  
`commitQuality()` 未检查 `rembTargetBps_`，改其它设置时可能重复发。

### P1-2：没有 transport-cc

当前本地 SDP 不声明，PC 不会发送 transport-wide extension；Switch 也没有发送 transport feedback。

### P1-3：本地上限 100M

```cpp
maxBitrateMbps_ = configInt(...,0,100);
```

官方可 500M。  
对本次 50M 不是直接原因，但产品能力不一致。

---

## 7. 日志证据

`%USERPROFILE%/Downloads/rr/connection.log*`：

```text
target 50 Mbps
n = 75
median ≈ 10.9 Mbps
max ≈ 14.9 Mbps
>=20 Mbps = 0

loss=0
nack=0
rtx=0
host-known=0
host-mbps=0
ack=0
applied=0
```

同时存在：

```text
bitrate-cap-feedback-custom-bps-50000000
```

解释：

```text
本地 requestBitrate(50M) 成功
但 PC 实际没有升到 50M
```

可能原因：

1. REMB SSRC 错误；
2. REMB 不是 compound；
3. 没有 transport-cc；
4. PC 没有接受；
5. 编码器/内容限制，但官方同场景 50M 降低了该可能。

---

## 8. 解决方案总览

分两个阶段，先做最小修复，再做 transport-cc。

```text
阶段 A：修 REMB SSRC + compound
阶段 B：接入 transport-cc
阶段 C：清理 Q6 双入口
阶段 D：档位/上限
```

### 为什么先修 REMB

- 改动范围小；
- 可以快速验证“PC 是否接受 Switch 的反馈”；
- 如果修好后 50M 生效，可能不需要立即上 transport-cc；
- 如果修好后仍不生效，再上 transport-cc，因果更清楚。

---

## 9. 阶段 A：修正 REMB

### 9.1 维护两个 SSRC

在 `PrimaryVideoRtcpSession` 中区分：

```text
localRtcpSsrc_   // Switch 自己的 RTCP sender SSRC
remoteVideoSsrc_ // PC 视频 media SSRC
```

来源：

- `localRtcpSsrc_`：每会话生成一个非零稳定值；
- `remoteVideoSsrc_`：从第一个主视频 RTP 的 SSRC 读取。

不要复用 `RtcpReceivingSession::mSsrc` 作为 REMB sender。

### 9.2 不破坏 RR

现有 `RtcpReceivingSession` 的 `mSsrc` 还用于 RR report block。  
不要直接把 `mSsrc` 改成 local；否则报告块会指向错误 SSRC。

建议：

- 保留基类 `mSsrc` 作为 remote video SSRC；
- 单独构造 REMB，sender 使用 `localRtcpSsrc_`；
- 或 override `requestBitrate`，不调用基类 `pushREMB`。

### 9.3 构造 compound REMB

建议发送：

```text
RR(sender=local, report block=remote)
SDES(CNAME=local)
REMB(sender=local, media=remote)
```

至少：

```text
RR + REMB
```

如果有 CNAME，优先 compound。

### 9.4 等待 remote SSRC

发送 REMB 前检查：

```text
remoteVideoSsrc_ != 0
localRtcpSsrc_ != 0
video_ open
rembAvailable_
```

如果 remote SSRC 未建立：

- 不发送；
- 不要设置 `bitrateFeedbackSent_`；
- 等第一个 RTP 后再发。

### 9.5 验证

抓 RTCP 或加日志：

```text
remb-sender-ssrc
remb-media-ssrc
```

期望：

```text
sender != media
sender = 本地
media  = PC
```

---

## 10. 阶段 B：接入 transport-cc

### 10.1 SDP

本地视频 SDP 增加：

```text
a=rtcp-fb:96 transport-cc
a=extmap:<id> http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
```

要求：

- extmap ID 不与现有冲突；
- 视频方向允许接收；
- 保留 nack/pli/goog-remb；
- 不使用 `transport-wide-cc-02`。

### 10.2 构建开关

实验构建：

```text
UU_RECEIVE_TIMESTAMPS=ON
UU_TWCC_FOUNDATION=ON
UU_TWCC_OBSERVE_CLIENT=ON
UU_TWCC_SEND_EXPERIMENT=ON
```

### 10.3 观察阶段

先只观察：

```text
twcc-observation-accepted-...
missing-extension=0
missing-time=0
```

如果 `missingExtension` 高，说明 SDP/extmap 没对上，不要进入发送。

### 10.4 发送阶段

在 `FeedbackAttachment` Ready 后：

```text
startSending(
    budget,
    policy,
    localRtcpSsrc,
    remoteVideoSsrc
)
```

参数建议：

```text
FeedbackTiming:
  intervalUs = 100000
  reorderHoldUs = 0
  lookbackPackets = 2048

FeedbackSendPolicy:
  firstRetryUs = 50000
  maxRetryUs = 500000
  maxPendingAgeUs = 1000000
  maxConsecutiveFailures = 5
  rtcpCname = 非秘密会话 CNAME
```

### 10.5 Poll

在 `refreshStats()` 或 worker tick：

```text
feedbackAttachment->poll(monotonicUs());
```

建议 50–100ms 一次，不能阻塞 UI。

### 10.6 Stop

`disconnect()`：

```text
feedbackAttachment->stop();
peer_->setMediaHandler(nullptr);
feedbackAttachment.reset();
```

必须早于 peer 销毁。

### 10.7 预算

使用 `SharedRtcpBudget`：

```text
bytesPerSecond ≈ 10–20 KB/s
burstBytes ≈ 4–8 KB
criticalReserve ≈ RR/NACK 保留
```

避免 transport feedback 挤掉 RR/NACK。

---

## 11. 阶段 C：清理 Q6 双入口

当前两条：

```text
commitQuality() -> requestBitrate
applyBitrateCap() -> requestBitrate
```

建议：

- `commitQuality()` 只更新目标/保存配置；
- `applyBitrateCap()` 统一发送；
- 或 commit 立即发送前检查 `rembTargetBps_`。

验收：

- 改 vsync/缓冲不会重复 REMB；
- 修改上限只发一次新值；
- 重连清空状态。

---

## 12. 阶段 D：档位与上限

### 12.1 全 Q6 档位

如果 UI 要每档明确 Mbps：

| UI | quality | field8 |
|---|---:|---:|
| 省流 | 6 | 5M |
| 清晰 | 6 | 12M |
| 高清 | 6 | 20M |
| 原画 | 6 | 30M |

代价：

- 丢失原生 quality 语义；
- PC 可能走 Custom 编码路径；
- 原画不再是原生原画。

### 12.2 保留原生 Q1–Q4

如果保留原生：

- 原生档不发字段 8；
- 不能保证每档具体 Mbps；
- 只有 Q6 有明确上限。

### 12.3 上限 100 vs 500

当前：

```text
maxBitrateMbps <= 100
```

官方可 500。  
如要测试 500M：

- 放宽配置校验；
- 同步 UI；
- 但不要把它当成提高实际码率的主要手段。

---

## 13. 验证方案

### 13.1 官方 APK 基线

同 PC、720p60 H265、高运动窗口晃动、60 秒：

```text
官方 APK：自定义 500M
实际：50M
```

### 13.2 Switch 三组

| 组 | 配置 | 目的 |
|---|---|---|
| A | 当前 fixed 50M REMB | 基线 10–15M |
| B | 修正 SSRC + compound REMB | 验证 REMB 是否有效 |
| C | transport-cc | 验证官方路径 |

### 13.3 记录

```text
RTP 中位/峰值
REMB target
REMB sender/media SSRC
transport feedback 包数
loss/nack/rtx
PC Incoming REMB / SetBitRate
```

### 13.4 通过标准

- B 或 C 实际码率显著高于 A；
- 无持续丢包/重传；
- REMB sender 是本地 SSRC；
- transport-cc 协商 Ready；
- 不影响播放/GPU/解码；
- 失败可回滚。

---

## 14. 风险

1. 官方静态支持 transport-cc，不等于当前 PC 会话一定使用；需抓包/日志确认。
2. 现有 transport feedback 是实验代码，未真机验证。
3. REMB 修正可能影响 RR report block，必须分离 SSRC。
4. compound RTCP 可能与现有 RR/NACK 重复，需要预算。
5. `UU_RECEIVE_TIMESTAMPS` 改变 Message ABI，依赖必须一致。
6. 高码率下 transport feedback 有 CPU 成本。
7. PC 若仍有内部硬上限，客户端无法保证 50M。

---

## 15. 回滚

- 保持 TWCC 默认 OFF；
- 实验构建单独目录；
- REMB 修正保留旧路径 fallback；
- 如果 transport-cc 无效：

```text
关闭 UU_TWCC_SEND_EXPERIMENT
回到 observation-only 或完全 OFF
保留 fixed REMB
```

- 不删除既有 RR/NACK/PLI/RTX。

---

## 16. 最终建议

### 最小下一步

1. 抓 RTCP，确认 REMB sender/media SSRC；
2. 修正 SSRC 与 compound；
3. 用 50M 测试；
4. 若仍不升，接入 transport-cc。

### 最可能成功路径

```text
修正 REMB SSRC/compound（快速排除）
    ↓
若无效
接入 transport-cc（官方同款反馈）
    ↓
PC BWE → SetBitRate → 实际 50M
```

### 不应做的事

- 不要只把字段 8 改成 500；
- 不要继续盲调自适应 REMB；
- 不要把全部档位改 Q6 当成反馈修复；
- 不要在没有抓包/日志的情况下断言 PC 硬上限。

---

## 17. 参考文件

当前源码：

```text
C:/uu/current/YOUYOU-Remote-ns/source/client.cpp
C:/uu/current/YOUYOU-Remote-ns/source/protocol.cpp
C:/uu/current/YOUYOU-Remote-ns/source/quality_rpc.cpp
C:/uu/current/YOUYOU-Remote-ns/include/quality_presets.hpp
C:/uu/current/YOUYOU-Remote-ns/external/libdatachannel/src/rtcpreceivingsession.cpp
C:/uu/current/YOUYOU-Remote-ns/include/transport_feedback_attachment.hpp
C:/uu/current/YOUYOU-Remote-ns/source/transport_feedback_attachment.cpp
```

官方样本：

```text
%USERPROFILE%/Downloads/uuyc_4.44.0.apk
%USERPROFILE%/Downloads/uuyc_4.44.0 (1).exe
```

日志：

```text
%USERPROFILE%/Downloads/rr/connection.log
%USERPROFILE%/Downloads/rr/connection.log.1
%USERPROFILE%/Downloads/rr/connection.log.2
```

既有 transport-cc 详细方案：

```text
C:/uu/archive/snapshots/youyou-v0372/audit-20261027/TRANSPORT-CC-IMPLEMENTATION-PLAN.md
```

---

## 18. 结论

当前代码与官方控制端的差异可以概括为：

```text
官方：field8 + transport-cc/REMB + WebRTC BWE + SetBitRate
当前：field8 + fixed REMB
```

当前固定 REMB 还叠加了：

```text
sender SSRC 使用 PC SSRC
单包 REMB 而非 compound
可能以 SSRC 0 发出
```

所以：

- 50M 字段 8 不是瓶颈；
- 固定 REMB 不可信；
- 应先修 REMB SSRC/compound，再上 transport-cc；
- PC 不可修改的前提下，transport-cc 是最接近官方的方案。

本次只生成报告和分析，未修改源码。
