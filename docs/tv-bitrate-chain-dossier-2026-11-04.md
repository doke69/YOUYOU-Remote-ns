# YOUYOU Remote 与官方 TV：码率连接链路证据总报告

日期：2026-11-04。汇总至 rev16，作为后续适配的事实底稿。实施方案见 [适配方案](./tv-bitrate-adaptation-plan-2026-11-04.md)。

## 1. 阅读规则和结论

本文将信息分成：**静态已证实、运行观察、推断、未闭合**。函数地址指固定样本 ELF 虚拟地址，不是源码行号或任意版本通用地址。C++ 内部对象偏移不能直接照搬到 YOUYOU/libdatachannel。

总判断：官方具有初始请求、在线 RPC、WebRTC 协商、接收扩展安装、到包反馈/传统估计、RTCP 路由等分离层次。YOUYOU 已有可用的请求和 RTCP 基础，但仍主要使用用户 cap 作为固定 REMB；TWCC 助手尚未形成默认可用的应用发送闭环。**足以制定有界适配，不足以宣称完整复刻或低码率根因已唯一确定。**

本轮用户边界：编解码、组帧/恢复、播放及原生 GPU 路径不改。本报告不触发代码改动、构建、部署或采集。

## 2. 样本与当前项目身份

| 对象 | 身份/位置 |
|---|---|
| 当前源码 | `C:/uu/current/YOUYOU-Remote-ns` |
| Git HEAD | `ba4d7f9f8f87f4a47e4d872db3dd0ca526718f8b`；工作树含既有修改，不能仅凭 HEAD 复原当前源码 |
| 当前系列 | 0.3.75 诊断/开发工作；实际 NRO 要以 manifest/hash/构建开关区分 |
| 离线 case | `%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440` |
| TV APK | TV 3.5.0；SHA-256 `d2905fca0441adb30a1ce6c308c35cce07abed7d352f838f60ea9e95bf4c74a6` |
| TV arm64 libstreamer.so | `evidence/tv-350-native/libstreamer.so`；SHA-256 `e4c2445d822bd7b83024f7175dd51c320a6663070362e684f1d3b6bb99ad4814` |
| 手机 APK | 4.44.0；SHA-256 `87b2abb3765300d59b108d22fdaa421a8e25b5e37c0cb40daa854cb17b118918` |

原始分析样本/二进制留在本地；本文不包含密钥、token、配对配置或完整 SDP 凭据。证据目录是本机取证材料，不代表可公开分发全部内容。

## 3. 全链路结构与未闭合边

```text
TV UI/配置
  ├─ 初始 ConnectOptions / CaptureParams
  └─ 在线 CaptureSetting RPC
             ↓
ControllerSession / Peer 工厂
             ↓
Peer 代理 → WebRTC Offer/Answer 引擎
             ↓
codec 匹配 + feedback 名称/参数交集
             ↓
扩展集合/方向/ID 选择
             ↓
本地/远端描述应用 → 工作线程媒体通道更新
             ↓
接收配置：反馈能力布尔 + 独立扩展映射
             ↓
RTP 解析：实际 transport 序号是否存在
  ├─ transport 到达窗口 → 周期/请求反馈 → PT205/FMT15
  └─ 传统 RBE → 估计值限幅/节流 → REMB PT206/FMT15
             ↓
RTCP module → 发送器 → 流 Transport 适配器
             ⋮ 外部 Transport 到具体 SRTCP 端点仍缺完整对象链
PC 接收反馈 / BWE / 编码器策略
             ↓
实际视频码率（不等于请求上限）
```

还不能把初始 factory 默认支持集合、最终官方会话实际 SDP/反馈、PC 应用反馈全部画成已闭合实线。下面逐段说明证据。

## 4. TV 请求、档位与 field8

### 4.1 初始连接

已识别 DEX 链：

```text
bc/h.c 或 k → dc/k.a（ConnectOptions）
→ kf/v.d → kf/e.d（补 DecoderCap）
→ IControllerService.createConnection
→ ControllerService / StreamerControllerUtil
→ StreamerController.Connect
```

普通构造路径写 fps、videoQuality、cursorCapture、尺寸及 CaptureParams，未调用非零 max_custom_bitrate setter；field8 保持 protobuf 默认0。

### 4.2 在线请求与 rev9 更正

```text
Ltc/l::j
→ CaptureSetting(fps,quality,int,Point)
→ j 接收第三个 int（此前屏幕ID语义）
→ l 在构造器写0
→ j7/e.g 读取 l
→ CaptureSettingRequest 字段8
```

原始 DEX：unit7 `const/4 v1,0`；unit10 `iput v3,...j`；unit14 `iput v1,...l`。因此 rev7/rev8 的“第三整数直接写 custom bitrate”错误，必须以 rev9 更正为准。

TV UI ordinal→协议 quality：Auto5、Fast1、General2、HD3、Bluray4、Custom6。普通预设不是全部 Q6，也不能据本地显示的5/12/20/30M反推 TV 的每档固定请求值。

未找到该构造路径对 l 的非零写入者，不证明反射/native/其他未检查路径绝不存在。手机最终 protobuf adapter 写整数不再次乘一百万，不能据此省略上游单位分析。

### 4.3 YOUYOU 当前请求

`include/quality_presets.hpp` 定义生产预设 quality1–4；其中 legacyMbps 只识别历史自定义配置。`source/protocol.cpp` 与 `source/quality_rpc.cpp` 仅 Q6 写 field8，数值为 customMbps×1,000,000。

field8 是远端编码上限请求，不是固定编码输出，也不是 Switch 本地解码负载指标。普通 TV 预设应与本项目预设比较，不与非零 Q6 请求混为一谈。

## 5. Peer 创建、Offer 与本地描述设置

### 5.1 工厂与代理

控制端初始化：`ccb3d0 → d1ef00 → df0d74`。被控端还有独立入口 `d217a8 → d472f8 → df0d74`；不能只凭 CaptureType0 判断角色。

工厂创建：

```text
df1180 → 66591c → 86a500
→ factory 实现 vtable f2c1d0
→ factory 代理 vtable f2c0a8
```

应用 CreateConnection 的工厂 +28 经代理到 `86abf0`，创建 Peer 实现 `8c685c → 8c6e20 → 8d0858`（vtable f2ff20），返回 Peer 代理（vtable f2c268）。不能将代理与实现虚表混用。

### 5.2 Offer 实入口

```text
应用 Offer coroutine ce7c44：Peer +108
→ 86ef80 线程代理
→ 8729d4 成员虚方法转发
→ 8cb10c → 9822d4
→ task 992934
→ 989314 DoCreateOffer
→ 989950 媒体选项 → 998c60 → 998fb4 → 975c20
```

任务/构造器/RELA/成员描述 `(0x108,1)` 相互验证；不是只有一个 CreateOffer 字符串。

observer 包装 cefba4→deca1c，vtable回调 decd60；成功后应用序列化/重建描述，调用 Peer +118 设置本地描述，再返回 offer 类型结果。+118 经代理到 9825dc。

**未闭合：** coroutine 所保存连接视图 +180 与 ControllerPeerConnection 字段 +178 的完整包装布局，以及结果到最终 SOAC socket 发送的逐字段桥。不得悄悄抹去这两个对象边界。

## 6. codec feedback 与扩展协商

### 6.1 feedback 能力构造

TV `766394` 向 codec 添加 goog-remb / transport-cc，直接调用点 `759a78 / 759e60`；手机对应 `6fecc8`。这证明能力存在，不证明特定会话最终保留。

初始能力构造到当前 Peer/media factory 实际集合的完整桥仍待补。

### 6.2 真正的交集

```text
Answer 媒体生成 977c78 / 978238
→ codec 匹配 97b898 / 97bbb4
→ 7565ac（两 codec +48 feedback 集合）
→ 755e98
→ 755d44 / 758048 / 755c94 比较名称和参数
→ 删除另一侧没有的反馈项
```

未发现这里做“TWCC存在就专门删除REMB”；共同支持列表可能同时保留多项，实际发送还受接收状态和到包条件控制。

`935a6c` 将 goog-remb/transport-cc 转内部类型3/4，只是类型转换，不是二者优先级算法。goog-lntf、nack、rrtr 检查函数也不能误认 REMB。

### 6.3 扩展选择：Offer 与 Answer 分开

Offer：`975c20 → 976798 → 97b010`，将入参扩展与 factory 列表匹配过滤，写媒体描述 +f0。之前的媒体会话扩展由 `9761d8` 合并并分配/协调ID。

Answer：`999448 → 97721c → 978238 → 97b278`。先查 TWCC-02，存在时跳过01，最后把02加入结果；还存在方向/加密等条件。

该02优先策略不能写成“TV本地Offer必定包含02”。最终ID来自协商，不硬编码为内部type8/9，也不能仅因实验包选了extmap5就认定官方如此。

## 7. 描述应用、接收配置及扩展安装

远端应用已连到：

```text
98a1c4 DoSetRemoteDescription
→ 9853d0 ApplyRemoteDescription
→ 985a50 → 98495c
→ 98b600 PushdownMediaDescription
→ 工作线程回调997484
→ 按方向选通道 +50 / +58
→ 本地932a90 / 远端932fc0
```

### 7.1 描述方向

- 本地 `VideoChannel::SetLocalContent_w` 取媒体扩展，经932edc构造接收参数，再调用媒体通道+c8→75d354。
- 远端 `SetRemoteContent_w` 主要下发发送参数（+c0→75b760），并在codec相关变化时条件回写接收参数；不是无条件把Answer扩展覆盖接收映射。

### 7.2 接收扩展

```text
75d354 → 75d060 验证/差异
→ 75d50c
→ 接收包装配置 +e8（配置基址+b8，即扩展+30）
→ 当前接收流 +0 → 6915c8
→ 6a07ac → 6f7410 清旧映射/遍历新项
→ 6f72b0 按URI查内部type
→ 6f7494 注册实际ID/type
```

创建/重建则：`762290 → 7623fc → Call+38 → 66dc34 CreateVideoReceiveStream`，传相同配置。扩展变化不必然重建整个视频流。

### 7.3 feedback 开关

```text
远端发送参数75b760 → 75b2a8 → 75bb48保存codec
→ 75753c 检查transport-cc
→ 75c560 写接收配置+28
→ 当前流+18 → 691648 写接收对象+78
```

新流配置由75e72c同时设置：反馈能力布尔+28，以及独立扩展列表+30。+78全部消费者的精确行为仍待补；不能把这个布尔当实际发送成功。

## 8. 实际到包、TWCC模式与传统估计

### 8.1 URI/type/ID

TV静态注册表：TWCC-01→type8（f246c0），TWCC-02→type9（f246d8）；手机对应记录已核对。内部type不同于SDP扩展ID。

包解析 `6fa54c` 优先尝试02的6fa70c，再尝试01的6fa75c；后者用6f7a14解析2字节网络序序号。成功写header+98存在位、+9a序号；02还含请求参数。

### 8.2 分流

`66f354 → 7f8afc` 先把包信息交proxy7b2200。若header+98为真则不再把该包交传统WrappingEstimator；否则经7f87d8选估计器。

传统分支有abs-send-time时切对应RBE；连续30个包缺该扩展后转toffset估计器。30是包计数，不是30ms或永久丢包率。

### 8.3 TWCC模式

CreateVideoReceiveStream `66dc34` 检查接收配置的TWCC-02 URI；存在则通过7f8b9c→7b290c关闭proxy+228周期标记。周期处理7b2644读该标记；收包7b2334同时保留按报文请求调用7b2548的分支。

01相关周期链：`6713f4 → 7f8bb4 → 7b2644 → 7b26d8 → 7b2988`。默认周期配置wind500ms/min50ms/max250ms/def100ms、frac0.05，允许field trial覆盖。frac是预算参与量，不能解释成5%丢包阈值。

到包窗口经799af8等构造transport feedback，序列化79a838写PT205/FMT15。内容是序号/到达时间，不是用户指定的Mbps。

### 8.4 传统RBE到REMB

observer7fc4d4对估计输出取min(new,cap)，较明显下降可提前发送，其他变化受200000us节流（默认Clock已核对为微秒）。不能把它反写成“增长3%主动探测”。cap来源未闭合，不能等同field8。

REMB输出经具体模块：`7fc63c → 670f64/670f8c → 7e6e84 → module+18 → 79c6dc → 789ffc`。后者保存码率和媒体SSRC列表，请求0x10000分发项，经7891d8构造并由78e4d0序列化PT206/FMT15 REMB。

## 9. RTCP到传输的证据边界

TWCC回调是 `7b2e00 → 670d6c/670d84 → 7e6f10 → module+10 → 79ca44 → 78ada8`，与传统REMB回调不同。早期两者身份曾连反，后已独立更正。

RTCP发送器经已登记sink、模式检查、调度与字节输出回调，进入流内Transport适配器694870。该适配器关闭时返回false；发送器返回/队列接受不等于PC收到或应用。

外部Transport动态对象到具体SRTCP端点尚未完整闭合。9150b4已识别SRTP保护/发送函数，但不能把已定位端点擅自拼成直接调用边。

## 10. YOUYOU 当前源码对照

| 位置 | 当前事实 |
|---|---|
| source/client.cpp applyBitrateCap | 用户maxBitrateMbps×1e6作为requestBitrate值；门控首RTP、协商、重复和失败；不是接收端BWE |
| external/libdatachannel/src/rtcpreceivingsession.cpp | RR+SDES及可选REMB；保存已请求目标，后续SR重复；已修正本地身份/媒体SSRC语义 |
| source/client.cpp REMB能力检查 | 远端视频SDP的PT96或通配goog-remb文本；不等于官方完整codec交集 |
| CMakeLists.txt | REMB提交默认ON；TWCC FOUNDATION/ADVERTISE/OBSERVE/SEND默认OFF，实际包以manifest为准 |
| transport_feedback_negotiation.cpp | 实验支持01，拒绝02；校验共同ID、方向、feedback和单传输等条件 |
| Client refreshFeedbackObservation | 观察附件，不调用startSending；stale-capabilities拒绝原因仍需修正/验证而非删保护 |
| UU_TWCC_ADVERTISE | 受控观察广告01/extmap5；不是官方TV默认能力的证明 |

当前没有证据表明“把普通预设改成全部Q6”是TV行为。旧自拟500ms/+15%/-20%方案也不是已恢复的官方估计算法。

## 11. A/B运行证据与局限

本地对照报告：`C:/uu/releases/diagnostics/0.3.75-remb-controlled-ab/results/COMPARISON.md`。

H.265/720p60、自定义100M，A关闭主动REMB，B开启：RTP采样中位约7.495→12.134Mbps，均值7.253→12.659，峰值13.098→17.724；显示FPS中位60.00/59.90，解码墙钟中位2.19/2.31ms。

支持“该兼容反馈路径影响PC输出”，但两轮场景复杂度不可由日志证明相同；短窗口间歇采样不是全程吞吐；未见PC Incoming REMB/SetBitRate证据；没有实际50/100M稳定吞吐。用户反馈B明显更卡也必须纳入验收，不能只看码率提高。

TWCC观察包只广告并记录到stale-capabilities，未证明observer挂载成功，不能用该次结果判定PC支持或不支持。

## 12. 必须撤回/限定的旧结论

1. rev7/rev8的CaptureSetting第三整数→custom bitrate错误，以rev9原始DEX更正为准。
2. 6736e8/673ab8属音频发送配置，不能证明视频接收注册；视频发送/FlexFEC引用也不能混用。
3. TWCC/REMB回调签名曾交叉误认，以独立review及基础总报告§17/§23为准。
4. CreateOffer字符串不能独自证明应用路径，rev11/12补了代理/任务/虚表证据。
5. Answer的02优先策略不等于Offer默认广告02。
6. PC/TV field-trial出现8100不能证明PC存在不可突破8.1M硬上限，也不能把TV控制端常量当PC实时编码配置。
7. REMB提交成功不是PC应用；cap不是实际吞吐；loss=0不代表没有初始缺口/RTX；静止无新帧不等于卡死。
8. 旧默认库/不同宏阶段产物不能混装；源码有helper不等于NRO包含或实际使用。

## 13. 未闭合清单与实施影响

| 缺口 | 影响 |
|---|---|
| 初始factory能力到实际Peer集合 | 默认广告集合还不能照搬；先补证或明确受控探测 |
| SOAC结果/Answer到引擎输入的逐字段桥 | 当前已验证WebRTC内部段，不能称所有信令字段已复原 |
| +78等状态消费者及完整反馈启停条件 | 启用前需核对，不把写布尔当闭环 |
| RBE observer cap来源 | 不把field8直接充当本地估计器cap |
| 外部Transport到SRTCP端点 | 官方完整网络发送链尚缺；本项目应复用现有认证SRTCP，不能明文旁路 |
| 官方真实会话SDP/RTP/反馈与PC消费 | 静态能力不能代替互通/性能证据 |

适配方案允许先实现能力状态和观察；以上涉及功能启用的缺口必须作为阶段门，不通过就保留默认旧路径。

## 14. 具体报告与证据索引

以下清单由本轮按实际本地文件生成；每份报告分别保留历史，不删除错误版本，但已标出应采用的修订。rev11–16报告内附各自原始asm/json证据文件名，根目录统一是 `%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/evidence/bitrate-official/`。

<!-- GENERATED-REPORT-INDEX -->

### 14.1 case逐份报告（全部实际文件）

| 报告完整路径（链接） | 阅读状态 |
|---|---|
| [CODEX-THREE-LINKS-REVIEW-2026-11-04.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/CODEX-THREE-LINKS-REVIEW-2026-11-04.md>) | 阶段报告：结合后续修订阅读 |
| [CODEX-TV-ANSWER-PUSHDOWN-2026-11-04-rev15.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/CODEX-TV-ANSWER-PUSHDOWN-2026-11-04-rev15.md>) | 采用：独立静态核验，运行边界见正文 |
| [CODEX-TV-EXTENSION-POLICY-2026-11-04-rev13.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/CODEX-TV-EXTENSION-POLICY-2026-11-04-rev13.md>) | 采用：独立静态核验，运行边界见正文 |
| [CODEX-TV-FEEDBACK-INTERSECTION-2026-11-04-rev16.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/CODEX-TV-FEEDBACK-INTERSECTION-2026-11-04-rev16.md>) | 采用：独立静态核验，运行边界见正文 |
| [CODEX-TV-OFFER-CHAIN-2026-11-04-rev11.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/CODEX-TV-OFFER-CHAIN-2026-11-04-rev11.md>) | 采用：独立静态核验，运行边界见正文 |
| [CODEX-TV-PEER-OFFER-ENGINE-2026-11-04-rev12.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/CODEX-TV-PEER-OFFER-ENGINE-2026-11-04-rev12.md>) | 采用：独立静态核验，运行边界见正文 |
| [CODEX-TV-RECEIVE-CONFIG-2026-11-04-rev14.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/CODEX-TV-RECEIVE-CONFIG-2026-11-04-rev14.md>) | 采用：独立静态核验，运行边界见正文 |
| [CONTROL-0375-CHECK.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/CONTROL-0375-CHECK.md>) | 阶段报告：结合后续修订阅读 |
| [CURSOR-CAPTURE-MOBILE-4440-20261031.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/CURSOR-CAPTURE-MOBILE-4440-20261031.md>) | 相邻功能调查：保留路径，非码率主证据 |
| [DSH-THREE-LINKS-2026-11-04-rev2.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/DSH-THREE-LINKS-2026-11-04-rev2.md>) | 历史交付/任务：不能单独作为当前最终结论 |
| [DSH-THREE-LINKS-2026-11-04-rev3.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/DSH-THREE-LINKS-2026-11-04-rev3.md>) | 历史交付/任务：不能单独作为当前最终结论 |
| [DSH-THREE-LINKS-2026-11-04-rev4.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/DSH-THREE-LINKS-2026-11-04-rev4.md>) | 历史交付/任务：不能单独作为当前最终结论 |
| [DSH-THREE-LINKS-2026-11-04-rev5.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/DSH-THREE-LINKS-2026-11-04-rev5.md>) | 历史：后续rev6撤回部分强推断 |
| [DSH-THREE-LINKS-2026-11-04-rev6.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/DSH-THREE-LINKS-2026-11-04-rev6.md>) | 阶段交付：结合Codex独立review和后续修订 |
| [DSH-THREE-LINKS-2026-11-04.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/DSH-THREE-LINKS-2026-11-04.md>) | 历史交付/任务：不能单独作为当前最终结论 |
| [DSH-THREE-LINKS-TASK-2026-11-04.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/DSH-THREE-LINKS-TASK-2026-11-04.md>) | 历史交付/任务：不能单独作为当前最终结论 |
| [FINDINGS.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/FINDINGS.md>) | 背景：PC离线差分/早期总报告，非当前会话证明 |
| [MOBILE-CONNECTION-COMPARISON-2026-11-03.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/MOBILE-CONNECTION-COMPARISON-2026-11-03.md>) | 阶段报告：结合后续修订阅读 |
| [PC-443-444-FUNCTION-REVIEW.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/PC-443-444-FUNCTION-REVIEW.md>) | 背景：PC离线差分/早期总报告，非当前会话证明 |
| [PC-443-444-PRELIMINARY.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/PC-443-444-PRELIMINARY.md>) | 背景：PC离线差分/早期总报告，非当前会话证明 |
| [SCREEN-OFF-MODE-4440-20261031.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/SCREEN-OFF-MODE-4440-20261031.md>) | 相邻功能调查：保留路径，非码率主证据 |
| [TV-DEX-CTOR-CORRECTION-2026-11-04-rev9.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/TV-DEX-CTOR-CORRECTION-2026-11-04-rev9.md>) | 采用：原始DEX构造器纠错 |
| [TV-MOBILE-BITRATE-CODE-2026-11-04.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/TV-MOBILE-BITRATE-CODE-2026-11-04.md>) | 阶段报告：结合后续修订阅读 |
| [TV-OFFER-PATH-2026-11-04-rev10.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/TV-OFFER-PATH-2026-11-04-rev10.md>) | 阶段报告：结合后续修订阅读 |
| [TV-PORT-CLOSURE-2026-11-04-rev7.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/TV-PORT-CLOSURE-2026-11-04-rev7.md>) | 历史：field8结论须以rev9更正；其他结论需独立复核 |
| [TV-PORT-CLOSURE-2026-11-04-rev8.md](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/TV-PORT-CLOSURE-2026-11-04-rev8.md>) | 历史：field8结论须以rev9更正；其他结论需独立复核 |

### 14.2 当前项目审查、修复和A/B记录

- [C:/uu/current/YOUYOU-Remote-ns/docs/bitrate-audit-2026-11-04.md](<C:/uu/current/YOUYOU-Remote-ns/docs/bitrate-audit-2026-11-04.md>)
- [C:/uu/current/YOUYOU-Remote-ns/docs/bitrate-code-comparison-and-solution-2026-11-04.md](<C:/uu/current/YOUYOU-Remote-ns/docs/bitrate-code-comparison-and-solution-2026-11-04.md>)
- [C:/uu/current/YOUYOU-Remote-ns/docs/remb-stage-a-2026-11-04.md](<C:/uu/current/YOUYOU-Remote-ns/docs/remb-stage-a-2026-11-04.md>)
- [C:/uu/current/YOUYOU-Remote-ns/docs/remb-ab-2026-11-04.md](<C:/uu/current/YOUYOU-Remote-ns/docs/remb-ab-2026-11-04.md>)
- [C:/uu/releases/diagnostics/0.3.75-remb-controlled-ab/results/COMPARISON.md](<C:/uu/releases/diagnostics/0.3.75-remb-controlled-ab/results/COMPARISON.md>)
- [C:/uu/current/YOUYOU-Remote-ns/docs/twcc-observation-build-2026-11-04.md](<C:/uu/current/YOUYOU-Remote-ns/docs/twcc-observation-build-2026-11-04.md>)
- [C:/uu/current/YOUYOU-Remote-ns/docs/transport-feedback-foundation.md](<C:/uu/current/YOUYOU-Remote-ns/docs/transport-feedback-foundation.md>)
- [C:/uu/current/YOUYOU-Remote-ns/docs/tv-negotiation-parity-gaps-2026-11-04.md](<C:/uu/current/YOUYOU-Remote-ns/docs/tv-negotiation-parity-gaps-2026-11-04.md>)
- [C:/uu/current/YOUYOU-Remote-ns/docs/tv-chain-evidence-review-2026-11-04.md](<C:/uu/current/YOUYOU-Remote-ns/docs/tv-chain-evidence-review-2026-11-04.md>)
- [C:/uu/current/YOUYOU-Remote-ns/docs/bitrate-connection-status-2026-11-04.md](<C:/uu/current/YOUYOU-Remote-ns/docs/bitrate-connection-status-2026-11-04.md>)
- [C:/uu/current/YOUYOU-Remote-ns/docs/tv-bitrate-adaptation-plan-2026-11-04.md](<C:/uu/current/YOUYOU-Remote-ns/docs/tv-bitrate-adaptation-plan-2026-11-04.md>)

### 14.3 原始证据目录

- `%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/evidence/bitrate-official`
- `%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/evidence/bitrate-official/dsh-three-links`
- `%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/notes`
- `C:/uu/releases/diagnostics/0.3.75-remb-controlled-ab/results`
- `C:/uu/releases/diagnostics/0.3.75-twcc-observe`

逐次rev报告中的证据文件名应在对应目录解析；不把文件存在当作结论被确认。本文未新建或复制原始日志/SDP/样本。


## 2026-11-05 适配实施入口

[首轮S0/S1实施与验证](./tv-adaptation-s1-2026-11-05.md)：已改源码并完成定向测试/默认Switch构建，未部署；保护文件哈希一致；S2观察闭环和S3/S4反馈尚未完成。

[S2观察闭环与离线验证](./tv-adaptation-s2-2026-11-05.md)：后续已完成原始协商与SDK安装视图的区别处理、纯内存流水线回归和完整观察构建；真实会话未验收，S3生产发送与S4传统估计仍未完成。

### rev17：TV初始视频扩展能力表

后续PC 4.46专题：[media SSRC过滤与适配修正](./tv-adaptation-s3-ssrc-2026-11-05.md)，对应离线case报告`report/PC-446-TWCC-MEDIA-SSRC-2026-11-05.md`。它修正此前默认认为0适合作为本PC反馈锚点的假设；未声称PC运行时回调已被证明。

后续适配状态见[Client发送候选](./tv-adaptation-s3-client-2026-11-05.md)：s3obs真机已取得01/ID5与有效时间戳证据，受控发送调用点已实现并离线构建；真实发送和PC码率响应尚未验收。该阶段没有补成官方02请求反馈或传统RBE的完整实现。

新增本地静态报告：`%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/TV-INITIAL-VIDEO-CAPABILITIES-REV17.md`。

已定位视频引擎能力构造器759fac，通过f25490表连续追加11项能力（初始ID4–14），其中TWCC-01初始ID10。它是支持能力表，不是最终SDP的extmap；尚不能据此更改项目的协商ID、认定默认会启用TWCC或证明码率根因。
