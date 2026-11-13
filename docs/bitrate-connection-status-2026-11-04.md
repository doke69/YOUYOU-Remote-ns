# YOUYOU Remote 码率连接全链路现状报告

**整理日期：** 2026-11-04  
**项目源码：** `C:/uu/current/YOUYOU-Remote-ns`  
**工作区版本：** 0.3.75  
**报告用途：** 汇总当前 Switch 侧码率控制实现、已有构建与 A/B 运行证据、TV/手机/PC 静态分析线索及仍需验证的问题，可交给 Grok 做独立复核。

## 1. 结论摘要

1. 当前码率链是“控制请求上限 + 可选 REMB 提示 + PC 自己的拥塞控制/编码器决定实际输出”。`max_custom_bitrate`（protobuf field 8）是上限请求，不是 Switch 能强制达到的码率。
2. 当前工作区 0.3.75 的四个产品预设分别发送 quality 1–4，不发送 field 8；自定义模式 quality 6 才发送 field 8，单位从 Mbps 转换为 bps。UI 档位表里的 5/12/20/30 是旧自定义配置迁移识别值，不是承诺的实际输出速率。
3. 当前源码已包含 2026-11-04 的 REMB 阶段 A 修复：区分本地 RTCP sender SSRC 与远端视频 SSRC，首个视频 RTP 到达后才允许发 REMB，并使用 RR + SDES(CNAME) + REMB 复合 RTCP。**此前“sender SSRC 错、裸 REMB 单包”的审查结论已过时，不能再当作当前源码问题。**
4. 最新可见的真机 A/B 结果支持主动 REMB 对实际接收码率有帮助：同为 H.265、720p60、自定义 100 Mbps，A 关闭 REMB 的 RTP 采样中位数 7.495 Mbps；B 开启 REMB 的中位数 12.1345 Mbps，峰值 17.724 Mbps。两轮非同时、内容/场景无法证明完全一致，且 PC 没有 Incoming REMB/SetBitRate 证据；这只是相关性和有限因果支持，不证明 PC 接受了目标或已解决低码率。
5. 当前仍没有已启用的 transport-cc 发送/BWE 链。TWCC 的若干辅助模块存在于源码，但构建默认关闭，且没有挂入 SDP 广告及反馈发送路径。
6. 本地有 A/B NRO 和对应日志，但本报告无法从这些本地文件确认 Switch 此刻安装的 NRO 文件哈希。最近可见运行日志属于 REMB ON 的 B 组；“最近测试过 B”不等同于“当前设备仍安装 B”。

## 2. 源码、构建与设备部署状态

### 2.1 当前源码身份

- 项目：`C:/uu/current/YOUYOU-Remote-ns`
- Git HEAD：`ba4d7f9`
- `CMakeLists.txt` 项目版本：`0.3.75`
- 工作树有多项未提交源码、依赖覆盖、测试和文档改动；因此 HEAD 本身不能代表完整的当前源码状态。
- REMB A/B 的构建验证记录称 A/B 仅 `UU_REMB_FEEDBACK` 和 build label 不同，源码、资源、依赖覆盖相同。

### 2.2 可核对的本地 A/B NRO

| 组 | 构建差异 | 本地 NRO | SHA-256 |
|---|---|---|---|
| A | `UU_REMB_FEEDBACK=OFF` | `C:/uu/releases/diagnostics/0.3.75-remb-controlled-ab/A/UURemote-ns.nro` | `0e3f4d4543610afddd17bea6692083d57cb52a6df6f9904d5c06e86990bcc105` |
| B | `UU_REMB_FEEDBACK=ON` | `C:/uu/releases/diagnostics/0.3.75-remb-controlled-ab/B/UURemote-ns.nro` | `58fd9a9c693204e9929d913f630f2343db985f57175a2b4cfd5cc931ef50981b` |

构建清单核对结果：A/B 的 source、resources、dependency overrides 相同；唯一功能开关差异是 REMB OFF/ON，另有对应 build label。文件存在及哈希只证明本机有这两个产物，不能证明哪一个当前装在 Switch 上。

### 2.3 运行/安装证据边界

- A 组和 B 组运行日志都已保存在 `C:/uu/releases/diagnostics/0.3.75-remb-controlled-ab/results/`。
- B 日志包含 `remb-experiment-enabled`；B 组运行证据明确。
- A 的保留日志显示 `feedback=0`、`remb-target-bps=0`，与 REMB OFF 一致，但保留日志中没有找到显式 `remb-experiment-disabled` 标记；所以 A 组运行身份证据弱于 B 组的标记证据。
- 没有当前 Switch SD 卡的 NRO 回读哈希/安装清单，故不能给出“当前设备版本/当前安装组”的确定结论。最近一次 B 组测试不能替代当前安装核验。

## 3. 从 UI 到 PC 编码输出的实际链路

```text
YOUYOU 画质选择 / 本地配置
  ├─ 预设 quality=1..4：不带 field 8
  └─ 自定义 quality=6：带 max_custom_bitrate（field 8，Mbps×1,000,000）
       ↓
初始 CaptureSettings 或在线 Quality/CaptureSettings RPC
       ↓
信令与 WebRTC PeerConnection 建立、接收远端 SDP/RTP
       ↓
若 answer 视频行声明 goog-remb，Client 才将 REMB 标记为可用
       ↓
首个主视频 RTP 到达后，REMB gate 才允许提交当前自定义上限
       ↓
libdatachannel 通过 SRTCP 发送 RR + SDES + REMB
       ↓
PC 是否接收/纳入 BWE、如何调用 SetBitRate：当前 Switch 日志没有直接证据
       ↓
PC 编码器实际产生 RTP；Switch 从接收 RTP 字节统计 actual-mbps
```

关键含义：field 8、`remb-target-bps`、`feedback=1` 都不是 PC 实际编码码率。`actual-mbps` 是接收侧 RTP 统计窗口的输出速率估计，才是现有 Switch 日志里最接近实际流量的指标；它也不是 PC 的编码器控制面回执。

## 4. 当前档位及 field 8 语义

源码 `include/quality_presets.hpp` 当前定义：

| UI 档位 | quality | field 8 | 说明 |
|---|---:|---|---|
| 省流 | 1 | 不发送 | 由 PC 原生质量/BWE策略决定实际输出 |
| 清晰 | 2 | 不发送 | 同上 |
| 高清 | 3 | 不发送 | 同上 |
| 原画 | 4 | 不发送 | 同上 |
| 自定义 | 6 | 发送用户上限 | 例如 50 Mbps → 50,000,000 bps；不是固定输出目标 |

预设结构中的 `legacyMbps` 5/12/20/30 仅用于识别和迁移旧配置。它们没有作为 field 8 发给 PC，也不代表 PC 当前各质量档的目标/最大码率。此前日志中用户观察到省流约 1 Mbps、清晰约 4 Mbps、高清约 8 Mbps、原画约 14–17 Mbps，是特定场景的实际观察值，不是代码保证的档位映射。

## 5. 当前 Switch REMB 实现

### 5.1 阶段 A 已修正的旧问题

当前记录 `docs/remb-stage-a-2026-11-04.md` 说明：

- 本地 RTCP sender SSRC 与远端 PC 视频 RTP SSRC 分开保存；本地 SSRC/CNAME 在单次连接内稳定。
- RR/SDES 使用本地身份；RR report block 仍指向远端主视频 SSRC。
- REMB 反馈头的 media-source SSRC 为 0；REMB FCI 目标列表使用已收到的主视频 SSRC。
- 尚无主视频 RTP 时不发送、不保存 REMB 目标，也不将其记为本地成功；首包到达后才可提交。
- 主动 REMB 和 SR 后续重复都用 RR + SDES(CNAME) + REMB 复合包，经既有 SRTCP 路径发送。
- REMB gate 负责首包门控、相同 target/media 去重、target/media 变化、失败重试与连接重置。
- 连接阶段从 `commitQuality()` 的重复直接提交改为由 `applyBitrateCap()` 统一发送。

这些是源码结构和本地测试能证明的修正，不等于已抓到线上 RTCP 包，也不等于 PC 已应用。

### 5.2 当前 REMB 是固定上限反馈，不是自适应接收端 BWE

当前主动路径提交的是自定义上限对应的 REMB 值；它没有根据接收端的传输到达时间、拥塞窗口或实时吞吐运行完整 transport-wide congestion control。包级 gate/去重也不是 BWE 控制律。当前代码不能据此保证“网络好就升到上限、网络拥塞就按估计平滑下降”。

### 5.3 REMB 日志含义

- `remb-experiment-enabled/disabled`：标识构建门控。
- `remb-target-bps`：本地准备/当前保存的反馈目标。
- `feedback=1` / `remb-local-accepted`：本地 RTC/SRTCP 处理链接受了请求；不是对端确认。
- `remb-deferred`：尚未具备首包/有效身份等发送条件而延迟。
- `remb-sender-ssrc` / `remb-media-ssrc`：本地记录的 sender/media 身份值；它们不是抓包证据。
- `actual-mbps`：Switch 收到的 RTP 窗口速率估算。
- `host-known=0`, `host-mbps=0`, `ack=0`, `applied=0`：当前客户端没有得到 PC 的实际编码器应用确认；0 不代表实际码率为 0。

## 6. 2026-11-04 REMB A/B 结果

来源：`C:/uu/releases/diagnostics/0.3.75-remb-controlled-ab/results/COMPARISON.md` 和 `comparison.json`。报告记载两组均为 H.265、1280×720、60 FPS、自定义 100 Mbps。

| 指标 | A：REMB OFF | B：REMB ON |
|---|---:|---:|
| RTP 速率样本数 | 23 | 22 |
| 实际速率中位数 | 7.495 Mbps | 12.1345 Mbps |
| 实际速率均值 | 7.253 Mbps | 12.659 Mbps |
| 峰值 | 13.098 Mbps | 17.724 Mbps |
| 显示 FPS 中位数 | 60.00 | 59.90 |
| 解码墙钟中位数 | 2.19 ms | 2.31 ms |
| 队列等待中位数 | 28.465 ms | 27.70 ms |

**可支持的判断：** 这组短时 A/B 结果与“REMB ON 有助于提高当前实际码率”一致。两组中位数差约 61.9%。B 日志能看到 REMB ON 标记，且后续 `feedback=1`、本地 target 100,000,000 bps。

**不能支持的判断：**

- 不能说 PC 收到了、接受了或应用了 100 Mbps REMB；B 组 `host-known=0`，无 PC Incoming REMB/SetBitRate 证据。
- 不能把 61.9% 当成稳定增益或保证值；两轮不是同时测试，日志不能证明画面复杂度/时间段完全一致。
- 不能说 100 Mbps 实际吞吐已达成：B 组峰值为 17.724 Mbps，样本是间歇窗口，不是连续全程吞吐。
- 不能据 `loss=0` 推断无传输恢复；两组有 NACK/RTX 计数。
- 解码墙钟略有不同不足以归因于 REMB，也没有显示本次改动触及解码路径。

此前另一组记录曾报告 Q6/50 Mbps 实际中位约 10.9、峰值 14.9 Mbps；后续 A/B 的设置是 100 Mbps，不能把不同 run 的数值拼成同一条连续因果曲线。

## 7. TV APK、手机 APK 与 PC 端逆向线索

### 7.1 已有样本

- TV：`UURemote-tvProduction-3.5.0-30500-2b0f97d4-TV_gwqd.apk`。更正：此前列出的 SHA-256 `e4c2445d822bd7b83024f7175dd51c320a6663070362e684f1d3b6bb99ad4814` 是所提取 `libstreamer.so` 的哈希，不是 APK 容器哈希；rev8 报告给出的 APK 哈希前缀为 `d2905fca`，完整容器哈希需查样本清单。
- 手机：`uuyc_4.44.0.apk`，已记录 SHA-256 `87b2abb3765300d59b108d22fdaa421a8e25b5e37c0cb40daa854cb17b118918`。
- PC：UU 4.44 样本分析记录了 `streamer.dll` 中 BWE、Incoming REMB、transport-cc、`SetBitRate(min/start/max/probe/adaptive)`、`max_bitrate_limit` 等相关符号/字符串线索。

### 7.2 手机/TV APK 能支持什么结论

- 静态样本中能看到 `goog-remb`、`transport-cc`、接收侧 estimator/congestion-controller 等相关能力线索；TV 与手机部分函数/结构有同源迹象。
- 这说明官方客户端代码库/构建包含这些能力，不自动证明某个具体会话的最终 Offer/Answer 协商了它们，也不证明该反馈在运行时实际发送。
- TV APK 逆向的 rev6 报告明确撤回了先前把若干独立 Transport 间接调用链串成完整一条的结论；SDP parser 可以识别 `rtcp-fb`/`extmap`，但能力交集、BUNDLE 最终映射和单次 PeerConnection 实际选用项未静态闭合。
- rev6 没找到 field 8 到 native cap/REMB 的已证明静态边。field 8 是控制端发送给 PC 的上限请求这一协议语义有其他证据支持；不能用 rev6 证明它如何直接驱动 APK native BWE。
- 本次逆向是离线静态分析，没有 APK 运行时、SDP 抓取或 PC RTCP 包验证。

主要逆向材料：

- `%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/DSH-THREE-LINKS-2026-11-04-rev6.md`
- `%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/CODEX-THREE-LINKS-REVIEW-2026-11-04.md`

### 7.3 PC 的 8100 数值

旧 PC 样本中出现 `PcFactoryDefaultBitrates max:8100` 等默认参数线索；它应作为该 PC 版本/配置的默认码率参数看待，不能仅凭字符串就断言为不可突破的硬上限。手机控制端另有实际可达更高码率的用户测试叙述，但不是本报告同步复测。需要 PC 侧运行日志或相同 PC 条件下的受控 A/B 才能确定限幅是否生效。

## 8. 当前主要问题与证据强度

| 事项 | 当前判断 | 证据强度 |
|---|---|---|
| 产品预设 1–4 没有明确 field 8 Mbps 上限 | 当前代码事实；这些预设依赖 PC 端质量策略 | 高（源码） |
| 自定义 field 8 是上限，不是实际目标 | 协议字段语义 + 代码仅写入上限字段 | 高 |
| REMB ON 会提高实际码率 | 100M A/B 中 B 更高，但两轮场景非严格同步 | 中（有限真机 A/B） |
| REMB 阶段 A 的 SSRC/compound 缺陷仍存在 | 阶段 A 源码已修复；真机未抓包确认实际线上封包 | 源码修复高，线上行为未证 |
| 当前 Switch 已接通 transport-cc | 没有；源码有基础模块但发送/协商关闭 | 高（构建选项和调用路径） |
| PC 有 8100 Mbps 硬上限 | 未证实；默认参数字符串不能证明硬上限 | 低/未知 |
| 当前设备安装了 A 还是 B | 缺少设备当前 NRO 回读哈希 | 未知 |
| 码率低是 Wi-Fi、PC BWE、REMB、内容复杂度中的哪一个 | 目前无法唯一归因；缺少 PC 侧状态和严格同步场景 | 未定 |

## 9. 下一步建议：先取得闭环证据，不先叠新算法

1. **确认部署身份**：从当前 Switch SD 卡只读回读 NRO 哈希/manifest，确认安装的是 0.3.75 哪个 build label；避免拿源码默认值推断设备运行包。
2. **复做严格单变量 A/B**：同一 PC、固定相同 720p60/H.265/场景/时间长度/field 8=100 Mbps，分别 OFF/ON；至少多轮交替次序，记录同一窗口的 RTP 中位、均值、P95/峰值、NACK/RTX/RTT/FPS/解码/队列等待。用户已完成的一组结果保留作为初步证据。
3. **补 PC 侧接收/应用证据**：若 PC 日志可用，采集 Incoming REMB 与 `SetBitRate` 当前 min/start/max/probe/adaptive 的数字状态；若不可用，抓取经过脱敏的 SRTCP 复合 REMB，核实实际网络包 sender SSRC、media SSRC、target 与顺序。不要只用 Client 的 `feedback=1` 代替。
4. **核实官方实际协商**：拿到一次官方手机/TV 与 PC 成功会话的最终 SDP/RTCP 证据，确认 transport-cc 与 REMB 中实际哪种被协商、对应 extmap ID、媒体方向及反馈包类型。静态 APK 字符串不能代替这一步。
5. **再决定 TWCC**：只有证明当前 PC 会话支持/使用 transport-cc，且固定 REMB 的边界明确后，再评估开启现有实验发送器；保持独立构建开关与可回滚，不改解码/播放链。
6. **档位定义单独决策**：若希望省流/清晰/高清/原画严格对应 5/12/20/30 Mbps 的上限，必须明确是否把四个档位改为 quality 6 + field 8。当前代码并非如此；这样改会改变“原生质量档”语义，不能当成无风险 UI 对齐。

### 建议验收指标

- 同档位的 field 8 请求值与 PC 收到的上限请求一致（自定义档）。
- REMB 网络包结构、身份及 target 可从脱敏抓包确认；PC Incoming REMB/SetBitRate 若可得应与之对照。
- 实际 RTP 输出以连续同长度窗口比较，不以 UI 上限、REMB target 或单次峰值代替。
- 码率上升不能以明显增加 NACK/RTX、丢帧、播放等待或卡顿为代价；解码路径保持不变。
- A/B 至少重复数轮并交替顺序，报告每轮和总体分布。

## 10. 已知文档之间的冲突/过时结论

- `docs/bitrate-code-comparison-and-solution-2026-11-04.md` 是阶段 A 前的审查快照。它指出远端 SSRC 复用及裸 REMB 的问题，这些问题后来在 `docs/remb-stage-a-2026-11-04.md` 中修正。
- `docs/bitrate-audit-2026-11-04.md` 同样保留了阶段 A 前的建议；不要把其中“当前 REMB sender SSRC 错误”直接搬到当前实现结论。
- A/B 构建说明 `docs/remb-ab-2026-11-04.md` 曾写“本次没有构建、部署”，而目录内后续已经存在 11:39 构建的 A/B NRO、11:47/11:52 运行日志和 11:52 对比报告。本文按实际存在的 NRO/运行记录总结，同时仍将设备当前安装状态标成未确认。
- Rev6 APK 静态逆向报告撤回了 rev5 把 Transport 链接成单一路径的推断；本文采用 rev6 的停止结论。

## 11. 供 Grok 独立复核的重点问题

请对当前工作区源码和本报告逐项核验，不要以旧审查报告替代当前代码：

1. `source/protocol.cpp`、`source/quality_rpc.cpp` 的初始/在线请求中，quality 1–4 与 quality 6 的 field 8 是否一致符合协议；配置迁移表是否只用于旧值识别。
2. `source/client.cpp`、`include/remb_feedback.hpp` 与 dependency override 中，首个主 RTP 前后 REMB gate、会话 sender SSRC/CNAME、RR report block、复合 RTCP、请求去重/失败重试是否有竞态或协议语义问题。
3. `UU_REMB_FEEDBACK` OFF/ON 是否真的是 A/B 唯一功能差异；现有 A/B 日志如何支持、又不能支持哪些因果结论。
4. 现有 `transport_feedback` 代码在当前默认构建是否只属于未启用基础设施；是否存在未被报告发现的 offer 广告、实际 start/poll/send 调用点。
5. 不修改解码路径的前提下，给出进一步闭环验证 PC 是否收到/采用 REMB、以及是否需要 transport-cc 的最小方案。

请把“源码证明、日志观察、逆向推断、假设、未知”分开，并明确指出任何与本报告相反的代码/证据及其路径或包位置。

## 12. 参考资料

- 当前 REMB 实现记录：`C:/uu/current/YOUYOU-Remote-ns/docs/remb-stage-a-2026-11-04.md`
- A/B 构建门控：`C:/uu/current/YOUYOU-Remote-ns/docs/remb-ab-2026-11-04.md`
- 当前运行 A/B 结果：`C:/uu/releases/diagnostics/0.3.75-remb-controlled-ab/results/COMPARISON.md`
- 当前码率链旧审查：`C:/uu/current/YOUYOU-Remote-ns/docs/bitrate-audit-2026-11-04.md`
- 阶段 A 前代码对比：`C:/uu/current/YOUYOU-Remote-ns/docs/bitrate-code-comparison-and-solution-2026-11-04.md`
- TV APK rev6 三链逆向：`%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/DSH-THREE-LINKS-2026-11-04-rev6.md`
- PC/手机 4.44 样本分析见上述码率链旧审查及其样本哈希。

## 13. 结论与原始材料逐项索引

下面按报告中的调查主题列出可直接复核的源码、报告和原始证据位置。报告正文引用的是“解释/结论文档”；A/B NRO、日志和逆向证据文件是底层材料。

| 报告主题/结论 | 解释报告 | 原始源码/证据 |
|---|---|---|
| quality 1–4、自定义 Q6、field 8 编码/单位、配置迁移 | `C:/uu/current/YOUYOU-Remote-ns/docs/bitrate-audit-2026-11-04.md`；`C:/uu/current/YOUYOU-Remote-ns/docs/bitrate-code-comparison-and-solution-2026-11-04.md`（阶段 A 前快照） | `C:/uu/current/YOUYOU-Remote-ns/include/quality_presets.hpp`；`source/protocol.cpp`；`source/quality_rpc.cpp`；`source/config_store.cpp`；对应测试 `tests/bitrate_settings_test.cpp`、`tests/config_store_test.cpp` |
| 当前 REMB SSRC、CNAME、复合 RTCP、首包门控、发送状态 | `C:/uu/current/YOUYOU-Remote-ns/docs/remb-stage-a-2026-11-04.md` | `include/remb_feedback.hpp`；`source/client.cpp`；`include/client.hpp`；`scripts/dependency-overrides/include/rtc/rtcpreceivingsession.hpp`；`scripts/dependency-overrides/src/rtcpreceivingsession.cpp`；对应测试 `tests/remb_feedback_test.cpp`、`tests/remb_packet_test.cpp`、`tests/remb_session_test.cpp` |
| REMB OFF/ON 唯一差异、构建选项 | `C:/uu/current/YOUYOU-Remote-ns/docs/remb-ab-2026-11-04.md` | `C:/uu/current/YOUYOU-Remote-ns/CMakeLists.txt`；构建门控 `source/client.cpp`；A/B 的 `A/manifest.json`、`B/manifest.json`、各自 `configure.log` 与 `build.log`，位于 `C:/uu/releases/diagnostics/0.3.75-remb-controlled-ab/` |
| A/B 实际 RTP/FPS/解码/队列统计 | `C:/uu/releases/diagnostics/0.3.75-remb-controlled-ab/results/COMPARISON.md`；`comparison.json` | `results/A/connection.log`、`results/A/connection.log.1`、`results/B/connection.log`，同位于 `C:/uu/releases/diagnostics/0.3.75-remb-controlled-ab/`。日志中含会话/网络诊断字段；对外分享前应脱敏，不应整份公开上传 |
| A/B 可执行包 | 本报告 §2.2；构建差异见 `docs/remb-ab-2026-11-04.md` | `C:/uu/releases/diagnostics/0.3.75-remb-controlled-ab/A/UURemote-ns.nro` 和 `.../B/UURemote-ns.nro`；哈希见 §2.2。设备当前安装状态无回读证据 |
| 旧版 50M 固定 REMB 运行统计 | `C:/uu/current/YOUYOU-Remote-ns/docs/bitrate-audit-2026-11-04.md` | `%USERPROFILE%/Downloads/rr/connection.log`、`connection.log.1`、`connection.log.2`（若文件仍在）；当时解析摘要 target 50 Mbps、中位约 10.9 Mbps、峰值约 14.9 Mbps。原日志是否仍保留需使用时检查 |
| 手机/TV APK 码率函数和接收反馈路径 | `%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/TV-MOBILE-BITRATE-CODE-2026-11-04.md`；`MOBILE-CONNECTION-COMPARISON-2026-11-03.md` | `evidence/bitrate-official/` 中 `mobile-bwe-xrefs.json`、`mobile-bwe-functions.asm`、`tv-bwe-xrefs.json`（如存在）、`tv-bwe-functions.asm`、`tv-receiver-feedback.asm`、`tv-feedback-rtcp-module.asm`、`tv-feedback-capability.asm`、`tv-extension-type-map.json`、`tv-sdp-symbols.json`；目录根为 `%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/` |
| TV Transport / SDP 三条间接链及修订结论 | `%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/DSH-THREE-LINKS-2026-11-04-rev6.md`；独立复核 `CODEX-THREE-LINKS-REVIEW-2026-11-04.md` | `evidence/bitrate-official/dsh-three-links/three-links-findings-rev6.json`、`three-links-chain-rev6.json`、`rev6-key-snippets.asm`。rev1–rev5 是历史迭代，rev6 撤回了 rev5 中错误拼接的强推断 |
| PC 4.43→4.44 函数变化、SetBitRate/BWE/码率边界 | `%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/PC-443-444-FUNCTION-REVIEW.md`；初步结果 `PC-443-444-PRELIMINARY.md` | `evidence/pc-443-444`（若目录存在）及 `evidence/pc-443-*.asm`、`pc-444-*.asm`、`pc-444-streamer-bitrate-xrefs.json`、`pc-444-capture-xrefs.json`；原始提取 DLL 在 `pc-443-extracted/streamer.dll`。分析基于离线文件，不代表 Switch 会话实测 |
| TWCC/transport-feedback 当前模块能力和未启用边界 | `C:/uu/current/YOUYOU-Remote-ns/docs/transport-feedback-foundation.md`；本报告 §5、§8 | `C:/uu/current/YOUYOU-Remote-ns/CMakeLists.txt` 中 `UU_RECEIVE_TIMESTAMPS`、`UU_TWCC_FOUNDATION`、`UU_TWCC_OBSERVE_CLIENT`、`UU_TWCC_SEND_EXPERIMENT` 选项；`include/transport_feedback_attachment.hpp`、`source/transport_feedback_attachment.cpp`、`source/client.cpp`。是否进入实际 offer/发送路径，应以当前同宏产物和调用点复核 |

### 索引范围说明

- 这份报告把每一项主要结论对应到一份结论文档和核心源码/证据目录；并不逐一列出逆向工作区里每个临时脚本、每个中间汇编片段或所有历史版本文件。原始材料目录内还保留了更细的脚本和交叉引用结果。
- 表中以“若文件仍在”“如存在”标出的路径未在本轮逐个读取验证；它们是此前报告引用的原始位置，使用前需检查文件是否仍保留。
- APK/EXE、PC DLL、SDP/RTCP 抓包与设备部署属于不同证据层。静态样本、Switch 客户端日志、PC 运行时反馈不能互相替代。
- 如把本文交给第三方，优先分享本报告及已脱敏的结论材料；不要直接附完整 `connection.log`、APK/EXE/DLL 或含个人路径/会话数据的中间文件。

## 14. 本轮 A/B 与官方码率链调查的报告文件逐份清单

以下逐份列出围绕 2026-11-04 REMB A/B 测试及随后 TV/手机/PC 码率链调查形成或引用的 Markdown 报告。部分源码审查和计划先于 A/B 实际运行；因此这里按调查阶段归档，不声称它们都晚于第一轮真机测试。这里列的是**每份报告本身**，不只是汇总主题；同一目录下的 JSON/ASM/日志是对应报告的底层证据，不冒充 Markdown 报告。

### 14.1 YOUYOU 源码、修复和 Switch A/B

1. 初始码率链审查：
   `C:/uu/current/YOUYOU-Remote-ns/docs/bitrate-audit-2026-11-04.md`
2. 官方与 YOUYOU 码率代码对比及阶段方案（阶段 A 前快照；其中 REMB sender/封包问题后来已修复）：
   `C:/uu/current/YOUYOU-Remote-ns/docs/bitrate-code-comparison-and-solution-2026-11-04.md`
3. REMB 阶段 A 源码修改、构建/定向测试及限制：
   `C:/uu/current/YOUYOU-Remote-ns/docs/remb-stage-a-2026-11-04.md`
4. A/B 构建门控、实验控制变量和测试方法：
   `C:/uu/current/YOUYOU-Remote-ns/docs/remb-ab-2026-11-04.md`
5. A/B 真机统计结果：
   `C:/uu/releases/diagnostics/0.3.75-remb-controlled-ab/results/COMPARISON.md`
6. 本次综合链路报告：
   `C:/uu/current/YOUYOU-Remote-ns/docs/bitrate-connection-status-2026-11-04.md`

对应的机器可读对比和运行日志：

```text
C:/uu/releases/diagnostics/0.3.75-remb-controlled-ab/results/comparison.json
C:/uu/releases/diagnostics/0.3.75-remb-controlled-ab/results/A/connection.log
C:/uu/releases/diagnostics/0.3.75-remb-controlled-ab/results/A/connection.log.1
C:/uu/releases/diagnostics/0.3.75-remb-controlled-ab/results/B/connection.log
C:/uu/releases/diagnostics/0.3.75-remb-controlled-ab/verification.json
C:/uu/releases/diagnostics/0.3.75-remb-controlled-ab/A/manifest.json
C:/uu/releases/diagnostics/0.3.75-remb-controlled-ab/B/manifest.json
```

### 14.2 TV/手机 APK 与 PC 函数调查

7. TV 与手机 APK 码率代码路径对比（独立静态分析报告）：
   `%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/TV-MOBILE-BITRATE-CODE-2026-11-04.md`
8. TV/手机三个 native Transport/SDP 链的 DSH 首轮报告（后续版本有修订）：
   `%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/DSH-THREE-LINKS-2026-11-04.md`
9. DSH rev2：
   `%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/DSH-THREE-LINKS-2026-11-04-rev2.md`
10. DSH rev3：
    `%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/DSH-THREE-LINKS-2026-11-04-rev3.md`
11. DSH rev4：
    `%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/DSH-THREE-LINKS-2026-11-04-rev4.md`
12. DSH rev5（其中 Transport 链强推断后来被 rev6 撤回）：
    `%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/DSH-THREE-LINKS-2026-11-04-rev5.md`
13. DSH rev6（当前采用的修订结论）：
    `%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/DSH-THREE-LINKS-2026-11-04-rev6.md`
14. Codex 对 DSH 三链结论的独立复核：
    `%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/CODEX-THREE-LINKS-REVIEW-2026-11-04.md`
15. DSH 三链调查任务范围（这是任务说明，不是结果报告）：
    `%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/DSH-THREE-LINKS-TASK-2026-11-04.md`

这些 DSH 版本是迭代过程文件，不应把 rev2–rev5 的中间结论和 rev6 并列当成当前事实；本综合报告采用 rev6，并记录它撤回了 rev5 的一项强推断。

### 14.3 更早的 PC 4.43→4.44 调查（作为本轮背景资料）

以下两份报告早于 11 月 4 日 A/B 测试，但属于本报告引用的 PC 码率控制背景，故单独保留路径：

```text
%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/PC-443-444-PRELIMINARY.md
%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/PC-443-444-FUNCTION-REVIEW.md
%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/FINDINGS.md
```

`FINDINGS.md` 是较早的 RevSkill 样本分析总报告；其当前码率结论应与 2026-11-04 的 TV/手机专门报告及 rev6 修订交叉阅读。

### 14.4 先前连接对照（不是本轮码率 A/B 报告）

```text
%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/MOBILE-CONNECTION-COMPARISON-2026-11-03.md
```

这份是手机连接链对照，不是 11 月 4 日的 REMB 开关 A/B；为避免名称混淆，不能把它当成第 5 项的码率结果。

---

**状态声明：** 本文是代码、构建清单、已有日志统计和离线逆向证据的综合报告。没有当前 Switch NRO 回读，也没有 PC 侧 REMB/BWE 应用证据；因此不声称已证明设备当前安装包、PC 采纳行为或码率问题已解决。

## 15. DSH 新增交付 rev7/rev8（2026-11-04 后续状态）

后续继续追查的具体报告路径（DSH停止后由Codex独立续查）：

```text
%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/TV-OFFER-PATH-2026-11-04-rev10.md
%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/CODEX-TV-OFFER-CHAIN-2026-11-04-rev11.md
```

rev11通过SessionDescriptionObserver包装构造和协程ADR地址引用，将控制端CreateIceConnection所属流程与Offer生成状态机连上，补充了rev10尚未找到的静态边；仍未证明最终TWCC扩展安装和本次会话选择。DSH余额不足已停止，无排队任务；不再向其派发。

最新调查文档补充（逐份路径）：

```text
%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/TV-DEX-CTOR-CORRECTION-2026-11-04-rev9.md
C:/uu/current/YOUYOU-Remote-ns/docs/tv-negotiation-parity-gaps-2026-11-04.md
C:/uu/current/YOUYOU-Remote-ns/docs/tv-chain-evidence-review-2026-11-04.md
```

**后续独立复核更正：** rev7/rev8 的“整数参数→CaptureSetting.l”结论与其原始DEX编码矛盾：unit7将v1置0，unit14把v1写l；整数寄存器v3写的是j。以下rev7/rev8摘要仅保留交付历史，不能作为已接受结论。完整复核及DSH重新调查方向见 `C:/uu/current/YOUYOU-Remote-ns/docs/tv-chain-evidence-review-2026-11-04.md`。用户目标已明确为还原官方TV实际协商后再选择适配/移植，暂停把自加TWCC广告试验当作官方路径。

本节在上述 rev6 综合结论后追加。接口现查 worker 为 idle、无排队任务、本轮 completed/awaitingReview；最近交付为 rev8。以下是阅读交付报告后的摘要，尚未完成 Codex 对新增 DEX/native 指令证据的逐条独立验收。

后续构建交付补充：DSH 新增 SDP 广告开关及观察模式约束，Codex 已全新完整编译出 `0.3.75-twcc-obs`，未部署/实机。具体报告及全部产物路径：

```text
C:/uu/current/YOUYOU-Remote-ns/docs/twcc-observation-build-2026-11-04.md
C:/uu/releases/diagnostics/0.3.75-twcc-observe/README.md
C:/uu/releases/diagnostics/0.3.75-twcc-observe/verification.json
C:/uu/releases/diagnostics/0.3.75-twcc-observe/manifest.json
C:/uu/releases/diagnostics/0.3.75-twcc-observe/UURemote-ns.nro
```

新增具体报告完整路径：

```text
%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/TV-PORT-CLOSURE-2026-11-04-rev7.md
%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/TV-PORT-CLOSURE-2026-11-04-rev8.md
```

- rev7/rev8 新找到 TV DEX `Ltc/l::j → CaptureSetting.l → Ldc/k::a → CaptureSettingRequest.h → setMaxCustomBitrate(I)` 的写入链。它补充了 rev6 未闭合的 Java/Protobuf 部分；UI/配置来源、int 数值单位以及 quality 1–4/6 分支仍未闭合。
- rev8 报告已区分 q/r 引用的读取与写入：在已检索范围中未找到构造器之外对 q/r 的直接 iput；这不能排除通过 LiveData.setValue 更新其持有值。仍需追具体数据流，不能用“没找到 iput”证明没有 UI 更新。
- rev8 在所搜索的 TWCC URI 直接引用中未识别到明确的本地 SDP extmap builder。只能说“尚未找到/未证明”，不能断言官方不广告 TWCC，也不能仅凭 feedback builder 就认定官方会话使用 TWCC。
- 两条 RTCP 包构造链在 DSH 报告中保留；external Transport 到具体 SRTCP 端点的静态桥、官方单次 SDP 结果及 PC 消费/应用仍未证明。
- DSH 声明本轮未修改 YOUYOU 应用代码、未构建或部署；当前阶段是调查交付，不能将其当作码率修复。

新增证据完整路径：

```text
%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/evidence/bitrate-official/dsh-three-links/three-links-findings-rev7.json
%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/evidence/bitrate-official/dsh-three-links/three-links-findings-rev8.json
%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/evidence/bitrate-official/dsh-three-links/rev7-tv-dex-field8.txt
%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/evidence/bitrate-official/dsh-three-links/rev8-tv-dex-viewmodel.txt
%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/evidence/bitrate-official/dsh-three-links/rev8-twcc-uri-xrefs.txt
```


## Codex 独立续查 rev12：工厂代理与 Offer 引擎

具体报告：`%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/CODEX-TV-PEER-OFFER-ENGINE-2026-11-04-rev12.md`。

新增构造器/虚表/成员方法指针/任务入口证据：TV 工厂与 Peer 均有线程代理；Peer 代理 +0x108 经底层虚方法进入 0x9822d4，再通过任务 0x992934 到 DoCreateOffer 0x989314。+0x118 对应设置本地描述。不能直接把代理虚表当实现虚表。

尚未闭合：媒体选项中的反馈/扩展来源、Offer 返回到 SOAC 发送的字段封装、Answer 后扩展安装及运行时反馈选择。未改应用代码、未构建部署，不据此默认开启 TWCC。


## Codex 独立续查 rev13：Offer/Answer 扩展与反馈模式分流

完整报告：`%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/CODEX-TV-EXTENSION-POLICY-2026-11-04-rev13.md`。

Offer 975c20→976798→97b010 与 Answer 97721c→978238→97b278 是不同路径；TWCC-02 优先跳过 01 的代码属于后者，不可直接证明 TV 本地 Offer 广告。Offer 会合并/过滤输入与 factory 扩展集合。

早期 6736e8/673ab8 引用现核实是音频发送配置，不作为视频接收扩展安装证明。视频接收创建 66dc34 查配置 TWCC-02，经7f8b9c/7b290c关闭proxy周期标记+228；周期处理及报文请求反馈分支分别已验证。仍需追实际 Answer 到接收配置及反馈能力来源，不能宣布官方真实会话已启用 TWCC。应用代码及解码播放路径未改。


## Codex 独立续查 rev14：描述方向与接收扩展安装

报告：`%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/CODEX-TV-RECEIVE-CONFIG-2026-11-04-rev14.md`。

已区分 VideoChannel::SetLocalContent_w(932a90) 与 SetRemoteContent_w(932fc0)：本地描述提取扩展下发接收参数，远端描述主要更新发送参数并按条件回写接收codec参数，不能笼统当成Answer扩展直接覆盖。接收通道75d354→75d50c将扩展写入现有流配置并调6915c8→6a07ac→6f7410，清旧映射后按实际ID和URI注册；创建/重建接收流7623fc→Call+38→66dc34也传递该配置。仍缺streamer远端Answer应用到通道调用及实际反馈协商的完整上游；不称当前低码率原因已证明。未改应用或解码播放代码。


## Codex 独立续查 rev15：远端应用下发与反馈配置

报告：`%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/CODEX-TV-ANSWER-PUSHDOWN-2026-11-04-rev15.md`。

已接上 DoSetRemoteDescription(98a1c4)→ApplyRemoteDescription(9853d0)→985a50→98495c→PushdownMediaDescription(98b600)→工作线程997484→通道local/remote入口。transport-cc检测75753c还通过75e72c写入接收配置+28，实际扩展列表另在+30；两者不可混同。goog-remb/transport-cc转换935a6c只证明类型识别，不证明选择优先级。仍需补远端参数到保存codec及接收反馈更新的完整边、能力交集和真实会话证据。应用代码/解码播放未改。


## Codex 独立续查 rev16：反馈交集与接收状态更新

报告：`%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/CODEX-TV-FEEDBACK-INTERSECTION-2026-11-04-rev16.md`。

媒体通道+c0(75b760)→75b2a8→75bb48保存codec，检查transport-cc后经75c560直接更新接收配置和接收对象+78。另已找到7565ac→755e98真实feedback交集：按名称/参数匹配，删除另一侧不支持项；97b898/97bbb4及Answer媒体生成有实际调用。该交集不等于TWCC优先删除REMB。初始能力到会话及实际反馈门仍需追，未改应用/解码路径，未构建部署。


## 2026-11-04 汇总文档与适配实施方案

最新统一入口：

- [详细链路证据总报告](./tv-bitrate-chain-dossier-2026-11-04.md)：汇总至rev16、纠错、未闭合项及每份具体报告完整路径。
- [适配实施方案](./tv-bitrate-adaptation-plan-2026-11-04.md)：S0–S4、明确阶段门与回退，不改编解码/组帧/原生GPU/播放路径。
- [受保护文件SHA-256](./tv-port-protected-paths-2026-11-04.json)：后续改动前后核对；共用文件仍需代码块diff审核。

本轮只写文档，未实施方案、未构建部署或开始采集。


## 2026-11-05 适配实施入口

[首轮S0/S1实施与验证](./tv-adaptation-s1-2026-11-05.md)：已改源码并完成定向测试/默认Switch构建，未部署；保护文件哈希一致；S2观察闭环和S3/S4反馈尚未完成。
