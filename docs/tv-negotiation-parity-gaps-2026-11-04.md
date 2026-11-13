# TV 与 YOUYOU 协商/码率链路一致性差距（2026-11-04）

> 状态：只读核对，暂停应用侧观察器修复、协商能力新增和 NRO 准备。  
> 范围：TV 3.5.0 + 手机 4.44.0 既有离线证据；YOUYOU 当前工作树。  
> 边界：不运行 APK、不联网、不接设备、不读取凭据；不把“库里有 builder”当成“官方启用”，也不以我们自加 offer 后的 PC answer 反推 TV。

## 1. 这次纠偏的核心

目标不是“100M 目标必须输出 100M”，而是：

```text
YOUYOU 采用与官方 TV APK 一致的
初始连接请求、在线 quality/capture 请求、
本地 Offer 构造/最终生成、
以及 REMB/TWCC 的实际选择规则
```

当前 YOUYOU 的固定 REMB、`UU_TWCC_ADVERTISE` 观察开关、任何高目标吞吐实验，都不能单独作为“官方一致性”的证据。

## 2. TV 已证明的部分

### 2.1 初始连接请求

证据：[TV-MOBILE-BITRATE-CODE-2026-11-04.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/TV-MOBILE-BITRATE-CODE-2026-11-04.md>) §8。

已证明链路：

```text
bc/h.c 或 k
→ dc/k.a() 构造 ConnectOptions
→ kf/v.d
→ kf/e.d（补 DecoderCap）
→ IControllerService.createConnection
→ ControllerService.createConnection
→ StreamerControllerUtil.createConnection
→ StreamerController.Connect
```

已在 `dc/k.a` 看到：

- code-unit 99：fps；
- code-unit 113：videoQuality；
- code-unit 123：cursorCapture；
- code-unit 174/251：尺寸相关；
- code-unit 267：CaptureParams 写入 ConnectOptions；
- 该常规初始构造路径**没有** `max_custom_bitrate` setter，字段 8 保持 Protobuf 默认 0。

### 2.2 在线 quality/capture 请求

证据：同报告 §1、§8；`TV-DEX-CTOR-CORRECTION-2026-11-04-rev9.md`（rev7/rev8 已标注更正）。

已证明：

```text
Ltc/l;::j
→ CaptureSetting.<init>(fps, quality, int, Point)
→ unit 10: j = 第三个 int 参数
→ unit 14: l = 0
→ 序列化 lambda 读取 l
→ RpcMessage$CaptureSettingRequest.h
→ setMaxCustomBitrate(0)
→ RPC 发送
```

- `CaptureSetting.<init>` 的常规路径把 `l` 初始为 0，不是把 custom bitrate 写入 `l`；
- `j` 接收第三个 int 参数；原报告 §1 称 j=屏幕ID；
- 常规预设路径没有把每个档位改成 Q6，也没有直接赋 5/12/20/30M；
- 全 DEX 只看到 `CaptureSetting.l` 的构造器写 0 与 `j7/e.g` 读取，没有发现非零写入者；
- 因此在线自定义非零 field8 的来源仍未证明。

`g7/t.j0` 已证明 UI 枚举到协议 `VideoQuality` 的映射：

| UI ordinal | 协议 VideoQuality |
|---:|---|
| 0 | Auto=5 |
| 1 | Fast=1 |
| 2 | General=2 |
| 3 | HD=3 |
| 4 | Bluray=4 |
| 5 | Custom=6 |

### 2.3 field8 来源/单位

已证明：

- `CaptureSetting.<init>` 把第三个 int 参数写入 `j`，把 `l` 写为 0；
- `CaptureSettingRequest` 的 setter 存在，但该构造路径读到的 `l` 是 0；
- 手机 Java 最终 protobuf adapter 直接写整数，没有在最终编码阶段乘 `1,000,000`。

未证明：

- `CaptureSetting.l` 的真实非零写入者/其他构造入口；
- `Ltc/l` 的 q/r 与 custom 的语义（不要再把 q/r 当 custom 来源盲追）；
- `RemoteConfigStore.customBitrate` 到 `Ltc/l` 的完整来源；
- TV 值到底是 Mbps 还是 bps；
- TV 常规/自定义分支最终单位换算点。

### 2.4 真实本地 Offer 构造与最终生成

已证明的只有解析/安装侧：

- `d6bd54/d76a68 → 9644f0 → 968670 → 96cc90` 是 description/attribute 处理链；
- `96e030` 处理 `a=rtcp-fb`，`96e7b0` 处理 `a=extmap`，`96c51c` 解析并 `vtable+0xf8` 安装；
- `766394` 通过 `7565a0` 插入 `goog-remb`/`transport-cc` feedback 能力；
- `97b278` 处理 TWCC-01/TWCC-02 URI。

未证明：

- TV 最终本地 Offer 的具体构造 caller；
- Offer 是否真的写入 TWCC extmap、哪个 ID、direction/BUNDLE；
- `766394` 的上游是否最终进入真实 Peer 的本地 Offer；
- TV 最终 SDP 生成后是否还有 app 层改写。

证据位置：`TV-MOBILE-BITRATE-CODE-2026-11-04.md` §3、§4；`TV-PORT-CLOSURE-2026-11-04-rev8.md` §2；`rev8-twcc-uri-xrefs.txt`。

### 2.5 REMB/TWCC 的实际选择规则

已证明的静态形态：

- TV 收包路径存在 proxy/TWCC 与传统 RBE 的分流：header `+0x98` 为真走 proxy，为假走传统估计器；
- TWCC 包构造存在 `78ada8 → 78a734 → 789fc0 → 78c024`；
- REMB 包构造存在 `789ffc → 0x10000 → 7891d8 → 78e4d0`（PT206/FMT15）；
- 两者都有实际编码/发送条件，不能仅凭 builder/字符串判断本次会话使用哪一个。

未证明：

- 官方 TV 某次会话的最终 SDP 是否协商 TWCC；
- 实际发出的 RTCP 是 TWCC（PT205/FMT15）、REMB（PT206/FMT15），还是两者；
- PC 是否消费并应用；
- 选择规则与 extmap/方向/BUNDLE 的运行时关系。

证据：`TV-MOBILE-BITRATE-CODE-2026-11-04.md` §12、§13、§20、§21；`rev8` §2/§3。

## 3. YOUYOU 当前实现

### 3.1 初始连接

- `source/client.cpp:1495` 调用 `connectOptions(..., streamQuality_==6?maxBitrateMbps_:0, hevc_)`；
- `source/protocol.cpp:140` 的 `connectOptions` 写 CaptureParams 字段 1/2/3/4/5/6/10/11；
- `quality==6` 时写字段 8：`customMbps * 1,000,000`。

与 TV 的明确差异：

- TV 常规初始路径字段 8 为 0；
- YOUYOU 在 q6 且已有自定义码率时，初始 ConnectOptions 就带非零字段 8；
- 该差异是**已证行为差异**，不自动等于“错误”，但尚未证明 TV 其他初始入口也这样做。

### 3.2 在线 quality/capture

- `source/client.cpp:896/899`：q6 时把 `maxBitrate` 传给 `qualityUpdateRpc`/`captureSettingsRpc`；
- `source/quality_rpc.cpp:165+`：字段 8 写 `customMbps * 1,000,000`；
- `source/client.cpp:1969`：answer 只扫描精确的 `a=rtcp-fb:96 goog-remb` / `a=rtcp-fb:* goog-remb`。

与 TV 的差异/风险：

- TV 最终 adapter 没有在最后编码阶段乘 `1e6`；YOUYOU 在 RPC 构造时乘 `1e6`。TV 单位未证明，不能断言谁对；
- TV 有更完整的 rtcp-fb/extmap 解析；YOUYOU 的 REMB 可用性只看 goog-remb 字符串。

### 3.3 本地 Offer

- `source/client.cpp:1730` 起构造 H264/H265 `RecvOnly` 视频；
- 默认不广告 TWCC；
- 仅在实验宏 `UU_TWCC_ADVERTISE` 下增加 `transport-cc` + `extmap:5`（当前工作树实验改动，默认 OFF）；
- `peer_->setLocalDescription()` 在 `source/client.cpp:1894`。

边界：

- 这是 YOUYOU 自己的 Offer；
- 不能用它的 PC answer 反推官方 TV 的 Offer/answer；
- 它只证明 PC 对自加能力的响应，不证明官方一致性。

### 3.4 REMB/TWCC

- `source/client.cpp:2495+`：q6 且有 `maxBitrateMbps_` 时，以固定目标 `maxBitrateMbps_*1e6` 发 REMB；
- `rembAvailable_` 只来自 answer 的 goog-remb；
- 没有传统 RBE estimator；
- 没有 TWCC 发送；
- 现有 `transport_feedback` 是 observation-only/实验框架，不是官方选择规则。

## 4. 已证差异清单

| 项 | TV 已证 | YOUYOU 当前 | 差异性质 |
|---|---|---|---|
| 初始 ConnectOptions field8 | 常规路径 0 | q6 时非零 | 已证行为差异，未证 TV 其他入口 |
| 在线 field8 单位 | 最终 adapter 未见 `*1e6`；单位未定 | `*1e6` | 潜在差异，待 TV 单位证据 |
| 本地 Offer TWCC | 有 feedback builder；最终 Offer/extmap 未证 | 默认无；实验宏才加 | 不能对照，官方未证 |
| Answer 解析 | 解析 rtcp-fb/extmap | 只看 goog-remb | 能力覆盖差异 |
| REMB 产生 | 传统 RBE 形态存在；实际选择未证 | 固定用户上限 REMB | 实现不同，是否官方一致未证 |
| TWCC 发送 | 包构造存在；实际选择未证 | 未发送（仅实验观察） | 不能以自加 offer 证明官方 |
| field8 到 RBE/REMB | 无直接 native 边 | REMB 直接用 field8 上限 | 语义可能混淆 |

## 5. 尚无法确定项

1. TV 最终本地 Offer 的真实构造路径和最终 SDP；
2. TV 是否在 Offer 中广告 TWCC extmap；ID、direction、BUNDLE；
3. TV 的 field8 值单位（Mbps/bps）和 UI/config 来源；
4. TV 本次会话选 TWCC 还是传统 RBE/REMB；
5. PC 是否应用 REMB/TWCC；
6. TV 初始连接是否在其他入口也会写非零 field8。

这些项不能靠继续重复扫描字符串或相邻函数解决。

## 6. 最小需要哪份官方证据

### 证据 A（最高优先）

官方 TV/手机与 PC 的最终 Offer/Answer SDP，至少包含：

```text
a=group:BUNDLE ...
a=rtcp-mux
m=video ...
a=mid:...
a=sendrecv / recvonly / sendonly
a=rtpmap:96 ...
a=rtcp-fb:96 goog-remb
a=rtcp-fb:96 transport-cc（若有）
a=extmap:<id> ...transport-wide-cc...
```

要求是**官方会话**，不是 YOUYOU 自加 `UU_TWCC_ADVERTISE` 后的 answer。

### 证据 B

官方会话的 RTCP 分类/抓包：

- PT205/FMT15（TWCC）是否存在；
- PT206/FMT15（REMB）是否存在；
- 周期、目标 SSRC、目标码率。

### 证据 C

官方自定义 quality 的一次在线 CaptureSetting 请求字节/日志：

- quality=6；
- field8 原值；
- 对应目标 Mbps；
- 用来确定单位。

### 证据 D（可选但关键）

PC 侧 `Incoming REMB` / `SetBitRate` 日志，证明反馈是否真的被应用。

## 7. 当前结论

- 可以确认：TV 有完整的初始连接/在线 quality 请求形态；`CaptureSetting` 构造器把第三个 int 写入 `j`、把 `l` 写 0，非零 field8 来源未证明；不能把 `l` 当 custom 来源；
- 不能确认：TV 实际本地 Offer 是否广告 TWCC，TV 实际反馈选择，PC 是否应用；
- 当前 YOUYOU 的固定 REMB、实验 TWCC extmap 广告都不是官方一致性证明；
- 在证据 A/B/C 之前，不继续实现“官方协商对齐”，也不以高目标值对应吞吐作为验收。

## 8. 与当前代码改动的关系

本报告不新增应用能力。当前工作树中已有的实验改动（`UU_TWCC_ADVERTISE`、观察分支）保留为待审 diff，不作为官方一致性实现；在 Codex 审核和官方证据到位前不再扩展。
