# YOUYOU远程：TV码率适配剩余工作完整执行与交接方案

日期：2026-11-08。适用范围：接续S4-B，完成RBE输入、输出及产品集成，并验证已有TWCC适配。本文是执行方案，不表示所列待办已经实现。以当前源码、归档与真机结果为准，不以历史对话中的“完成”代替核验。

## 1. 目标和交付定义

目标：在保留现有功能和原生硬件路径的前提下，让Switch端的画质请求、反馈能力协商、接收带宽估计和反馈发送遵循已确认的官方TV链路。主机预设与自定义的实际画质应合理，连接、输入和画面稳定性不得退化。

不是要求设置100Mbps便恒定输出100Mbps。field8是请求上限，REMB是接收侧带宽反馈，实际RTP吞吐和PC编码码率是观测结果；四者必须分开。

剩余4个执行包，顺序为：

| 编号 | 内容 | 完成门槛 |
|---|---|---|
| R1 / S4-C | 认证后RTP元数据→真实估计器输入 | 输入语义、AST/TOF切换、Process、代际及流移除的定向回归通过 |
| R2 / S4-D | 估计输出→节流→SRTCP，与TWCC/兼容REMB协调 | 单一有效反馈所有者、发送结果与生命周期正确、可回退 |
| R3 | 产品依赖集成、完整回归和测试NRO | 同源同宏完整构建、保护检查、功能/震动回归和可核验manifest |
| R4 | 真机互通、画质/性能验收及文档收尾 | 真实会话/反馈/码率及用户体感证据，不以离线测试替代 |

R1/R2可分阶段编译，但在R3之前不能把独立研究ELF当作可安装产品交付。

## 2. 目录、基线和协作

| 用途 | 路径 |
|---|---|
| 唯一产品工作树 | `C:/uu/current/YOUYOU-Remote-ns` |
| 目录规范 | `C:/uu/LAYOUT.md` |
| 已验证独立估计器工程 | `C:/uu/study/youyou-rbe-subset-20261108` |
| S4-B证据归档 | `C:/uu/releases/diagnostics/tv-adaptation-20261108-rbe-subset` |
| 工具链宿主，非产品仓库 | `C:/uu/repo/.tools` |
| 公共临时构建位置 | `C:/uu/repo/.tools/tmp` |
| 原始逆向case | `%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440` |

**用户正在另一路接入手柄震动。**Client、协议、输入、CMake及测试可能同时变化。不得用旧副本覆盖这些文件，不得reset/clean当前工作树，也不得以HEAD等同当前脏工作树。

开始前：

1. 保存当前HEAD、git status、完整tracked diff、与本次相关的untracked源码清单及SHA。备份只在本机；不收集配对/登录内容。
2. 核对S4-B `verification.json`中的5项产品源哈希与10项保护记录。归档时已发现`source/client.cpp`相对上一阶段变动，用户随后说明正在接入震动；这只解释可能来源，仍需逐段审查，不能自动认定所有差异都是震动。
3. 与震动维护者按代码块协调：本任务负责能力、到包观察、反馈生命周期；对方负责震动协议及控制器输出。共用文件合并后同时回归。不要用整文件替换处理冲突。
4. 确认没有同目录构建正在运行，使用本任务专属build目录；保留旧候选包和日志。

推荐阶段性提交或本地差异快照，是否提交/推送另按用户指令；本文不授权发布、部署或上传密钥材料。

## 3. 不可修改的边界

本任务禁止改变：

- FFmpeg/NVTEGRA创建、参数、send/receive、硬解提交、驱动/fence等待。
- 压缩AU、AVPacket复制/转移、解码队列、flush、generation和帧恢复规则。
- StrictVideoReceiver组帧、排序、RTX/NACK/PLI策略；不得为反馈方便改恢复节奏。
- 原生GPU导入、surface引用、GPU同步、swapchain、绘制和呈现。
- 播放时钟、选帧、缓冲、vsync、音频解码/播放和A/V路径。
- 已有媒体线程数、优先级、绑核和频率；系统补丁、IPv6及PC端程序。
- 认证头、签名材料、登录/配对格式，及与本任务无关的触控/隐私/声音功能。

保护清单：`C:/uu/current/YOUYOU-Remote-ns/docs/tv-port-protected-paths-2026-11-04.json`。10项文件须与适用基线一致；若用户另有合法更新，先记录差异与新基线，不能静默重写清单绕过保护。共用`client.cpp`/CMake/依赖override仍需人工逐块审核，哈希清单不覆盖全部调用关系。

遵循当前AGENTS：未知输入在所属边界验证一次，内部传类型；不预抽单用helper，不引入重复框架；避免try/catch。只有真实外部异常边界才隔离异常，不吞失败。新增复杂竞态防护前先说明具体失败和成本，不借此扩展架构。

## 4. 已完成与未闭合的事实

### 已有成果，避免重复实现

- 统一反馈能力交集、描述有效性及失效清理；旧保存REMB撤销，不向网络发送“零带宽”包。
- TWCC-01已有实验真机发送、PC码率响应及用户反馈；预设原画和自定义画质已接近，但操作感压力尚未解决。
- TWCC-02扩展解析、T位编码、按请求调度、历史范围、双版本选择、等待请求/超时回退已做离线验证；02真机互通仍待验收。
- S4-B使用真实固定WebRTC估计器，通过host回放和Switch独立静态库/验证ELF链接，没有接产品Client。

### 不能当作既定事实

- TV精确WebRTC commit尚未识别。当前候选`218b56e516386cd57c7513197528c3124bcd7ef3`的API与行为锚点接近，不等于逐位一致。
- 接收observer cap的动态设置来源未闭合。旧`7fc784`候选已排除，实际属发送侧`CongestionControlHandler::GetUpdate`；不能继续拿它证明接收cap。
- 原版Field Trial具体配置未知。回放的`Enabled-0.65`只是参数生效测试，不能设为产品默认。
- 独立算法通过不证明PC接受反馈、不证明操作感改善，也不证明全部媒体生命周期正确。

## 5. R1：认证后RTP输入适配

### 5.1 接入位置和职责

先读当前`source/transport_feedback_observer.cpp`、`include/transport_feedback_observer.hpp`、Client挂载点及依赖认证后全局媒体handler。复用既有认证后边界，不在raw ICE入口估计，不移动SRTP认证或原媒体回调，不每Track重复观察同一个包。

独立RBE需要明确职责，可使用一个有独立名称的输入适配类型；不要为每个字段各建helper。先在独立工程验证，再以受控开关接产品。R1输出先留内存数值/离线observer，不能未完成R2便直接发网络反馈。

### 5.2 输入契约

| 字段 | 语义/要求 |
|---|---|
| arrival | 真实到达元数据、单调时钟域；认证后读取但不能用认证/排队完成时间冒充网卡收包时间；缺时间戳显式记数，不伪造 |
| size | RTP payload+padding，排除RTP头及UDP/IP/TURN开销，非AU/解码帧大小 |
| SSRC/PT/mid | 当前generation及已安装描述共同映射；主视频、音频、RTX关联必须正确 |
| timestamp | 视频90kHz RTP时钟，保留32位回绕；不能当毫秒 |
| AST | 按协商ID和包内存在位读取24位绝对发送时间，保留64秒回绕 |
| TOF | 有符号24位transmission offset，以RTP时钟单位处理，非毫秒 |
| transport sequence | 依据最终共同扩展和包内存在位分流；同包不重复送传统RBE和transport反馈 |

畸形RTP、扩展长度/ID冲突、越界padding只在解析边界拒绝。可见的诊断是固定计数，不记录明文payload。不要重复解析/复制整包，只传估计所需元数据，不持有AVFrame/surface。

音频/RTX如何进入传统估计器必须以固定receive-controller源码和TV证据逐项核对；不能因TWCC观察器覆盖音视频RTX，就无条件全喂90kHz TOF。不能把RTX的SSRC/序号改写成主视频然后重复记账。

### 5.3 估计器切换与处理

- 复核固定参考控制器再实现：有AST时切入AST并清缺失计数；AST模式连续30个适用包无AST后重建TOF。计数不是30ms，也不是网络丢包率。
- 保留真实算法状态重建语义；切换时不拿旧估计无条件覆盖新实例，也不改媒体管线。
- 按真实`Process()`返回期限调度，复用现有可用轮询入口；不忙等，不漏TOF的周期处理，不凭空创建高频媒体线程。
- 新peer/描述失效/关闭停止准入和旧generation输出，移除SSRC并释放估计状态。已准入回调的所有权/同步复用现有模型；全局Field Trial只能初始化阶段配置，不能在活跃会话改写。

### 5.4 R1定向测试

1. 已协商/未协商AST、TOF；ID变化、方向不符、缺时间戳、无扩展和畸形扩展。
2. AST进入、29/30连续缺失、期间恢复AST重置计数、重复切换重建。
3. RTP32位、AST24位、TOF正负值和包到达时间单位换算。
4. payload+padding准确计量，RTP头/RTX/音频分流符合证据，transport序号包不重复估计。
5. Process期限、长时间无包、流移除、关闭/重连/协商更新和旧generation输出拒绝。
6. 默认重复回放结果一致，限速响应保持S4-B结果；不能为通过测试改变算法或放宽语义。

验收交付：源码差异、输入契约、测试日志与哈希；产品发送仍关闭。

## 6. R2：估计输出、SRTCP与反馈所有权

### 6.1 输出链

```text
当前会话真实RBE估计 + 媒体SSRC集合
→ 原版RembThrottler（时间/下降门、可证实的cap）
→ 已协商且当前有效的REMB发送入口
→ 当前peer的认证SRTCP
```

不能把用户上限、最近RTP吞吐、PC报告码率当作算法输出。保留200ms和明显下降提前输出的原版判定；它不是500ms加15%/减20%的自拟控制器。

### 6.2 cap未决项的处理

继续以既有RevSkill case范围查实际接收cap设置调用桥，证据记函数/调用者/值来源。没有证据时：

- 独立验证可以保留参考默认无限cap，但明确这是参考行为，非TV运行时定论。
- 不将field8直接映射为observer cap，不宣称已完整复原TV限幅。
- 若产品候选采用参考默认，需明确标注兼容实验及未决差异；不能在报告中悄悄关闭该问题。

补逆向必须遵守已授权离线范围；若变成运行样本、附加进程或扩大敏感材料读取，另行处理授权，不默认为本文允许。

### 6.3 共存与回退

实现前画出当前实际状态转换，并用测试固定下列约束，复用已有状态而非另建一套平行状态机：

| 状态 | 允许动作 |
|---|---|
| 能力/描述未有效 | 不发新反馈，清掉会被后续SR重复发送的旧保存目标 |
| TWCC正常持有反馈权 | 保持现有TWCC模式规则，传统RBE/固定cap不得无条件竞争 |
| 02正常等待请求 | 是正常等待，不能用零提交数自动判失败 |
| 当前会话可用的传统REMB模式 | 只允许当前generation的RBE输出，按共同mid/PT/feedback准入 |
| 明确发送/绑定失败或能力撤销 | 停新反馈、清过期目标，按已定义兼容路径回退并记原因 |
| 关闭/新peer | 旧回调不能转发到新transport，旧窗口/估计/保存目标不能继承 |

传统RBE是否在TWCC失败时立即可用取决于是否已积累有效输入；不能拿未预热估计补位，也不能为预热而无依据重复喂同包。兼容cap-REMB保留为明确回退选项，不标成官方BWE。

保留本机sender SSRC/CNAME、远端media SSRC/SSRC列表、复合RR+SDES约束，沿现有媒体SRTCP发送，不开明文UDP。使用当前transport绑定，不只取易变peer指针。

发送返回成功表示本地处理器/传输接受，不是PC编码器已采纳；失败不得误提交成功、丢失待处理状态或高频重试。核对当前requestBitrate会保存目标并随SR重复的行为，避免与新节流器产生未说明的重复发送；需要变更时仅改反馈实现，不修改媒体恢复。

### 6.4 R2测试与诊断

覆盖正常输出、下降提前输出、cap语义、无能力、描述失效、首RTP前后、失败/重试、TWCC切换、正常等待请求、旧generation及重连。

用真实RTCP解析验证sender/media SSRC、反馈值、SSRC集合和复合包结构。验证清目标后后续SR不再带旧REMB。发送预算不能饿死RR/NACK/PLI或音频控制。

固定数值诊断建议复用既有通道：模式/原因、RBE输入包数、有效arrival、AST缺失计数/切换、估计值、节流输出值、提交/失败次数、反馈字节及generation。原始估计、实际发送与实际RTP分别统计；禁止热路径同步文件写或日志洪泛。

## 7. R3：依赖、完整构建和回归

1. 基于S4-B的显式源列表集成最小子集，不把下载的3016文件全部参与编译，也不引入整套WebRTC媒体引擎。
2. 固定WebRTC/Abseil来源与SHA，保留LICENSE/PATENTS/AUTHORS/第三方声明。平台适配只在窄边界，不修改算法。
3. 识别与现产品Abseil/WebRTC名字、符号、编译定义的实际冲突；有冲突才针对处理，不预先大规模改命名空间或替换全局依赖。
4. 新RBE功能建议独立默认OFF实验门，名称由实现者统一记录。依赖`UU_RECEIVE_TIMESTAMPS`时，所有Message ABI相关对象/库必须同宏重建。现有TWCC开关约束不能绕过。
5. 使用全新专属host/NX build目录，不混S4-B独立库、历史阶段库和现产品对象。TEMP/TMP/TMPDIR=`C:/uu/repo/.tools/tmp`；DEVKITPRO/DEVKITA64及命令见独立工程README、当前`docs/build.md`和`scripts/build-native.ps1`，运行前核对脚本当前内容。
6. 测试默认OFF、RBE开启/TWCC未接管、TWCC开启及回退配置；非法宏组合应在配置阶段明确拒绝。
7. 回归当前适用的协议/配置/画质映射、反馈能力、TWCC、REMB packet/session、信令、输入和震动测试。测试目标按当前CMake实际登记，不照抄旧报告的3/3总数。
8. 核对10项保护哈希及共用媒体调用段零功能改动；`git diff --check`；核对NACP显示版本、构建标签、图标及实际宏配置。
9. 候选NRO单独归档，manifest至少记录HEAD+脏树源hash、依赖hash、宏、工具链、NRO/ELF尺寸SHA、测试结果与未验证项。不覆盖已验证包，不宣称离线构建有性能收益。

回归需覆盖：登录/手动刷新/连接断开、预设与自定义请求、键鼠/手柄切换、触控、震动、隐私/息屏、电脑声音静音恢复、配置持久化。现有无关缺陷单列，不能顺手修改编解码解决。

## 8. R4：真机验证与判定

交付测试包后由用户安排复制；不自动写SD、不改配对配置或启动采集。部署需确认设备、退出程序、备份及回读；用户选择手动复制时尊重该方式。

### 8.1 固定条件

同一PC版本（当前已知用户曾升级4.46，测试时重新记录实际版本）、同一Switch/CPU频率、同编码/分辨率/FPS、vsync、播放缓冲、网络和持续运动场景。一次只变一个因素，保留可回退旧包。静止桌面/暂停不作为带宽上限或长卡顿证据。

### 8.2 最小分组

| 组 | 目的 |
|---|---|
| 基线兼容包 | 记录当前已知体验，确认实际包hash，非仅看版本号 |
| RBE候选，无TWCC接管 | 验证传统估计、REMB发送与PC实际响应 |
| TWCC-01 | 回归已验证的01，不因新RBE竞争而退化 |
| TWCC-02 | 必须实际共同协商02并收到真实请求，验证请求范围/等待/反馈；未协商不能算通过 |
| 降级/重连 | 缺扩展/能力撤销/反馈失败回退、关闭重连及旧目标清除 |

每组至少含持续运动约60秒；发生问题先标时间点/操作，避免要求无休止重复采集。没有真实02对端时将其标“待互通”，不能强填通过。再用另一台Switch做必要兼容回归，不把两台差异归因于同一个不可控场景。

### 8.3 指标与验收

- 共同能力、实际扩展ID/模式、请求及反馈类型/次数/字节/失败；RBE原始估计与最终反馈。
- 实际RTP平均/峰值、RTT、初始序号缺口、RTX/NACK/恢复；缺口不等于WiFi永久丢包。
- 唯一呈现帧、重复、追赶丢弃、长间隔、选帧等待、解码墙钟、输入体感；present返回不等于scanout。
- 同条件对照平均与尾部指标，不能只挑最高码率/平均FPS；新候选应无明显操作感/稳定性回退。具体容差在取基线后记录，不临时改变标准掩盖退化。
- 预设1/2/3/4与Q6语义正确；不把全部预设偷偷改成Q6，不承诺固定输出5/12/20/30，不新增每次保存配置的备份INI。

出现连接失败、明显延时/掉帧回退或震动/声音等功能退化时保留数值诊断，切回旧包；先定位反馈频率/计算/分配/同步的有据开销，不改解码/GPU、频率或绑核掩盖。

## 9. 各执行包交付清单

每包交付一个阶段目录：

```text
REVIEW.md             本包目标、事实、改动、风险和未决项
SOURCE-SHA256.json    实际源码和依赖清单，含工作树状态
CONFIG.json           编译宏/工具版本/平台
TEST-RESULTS.md       用例、原始日志路径、失败及修正
PROTECTED.json        10项保护及共用媒体代码审核
NEXT.md               下一步/回退条件
package/              仅R3/R4需要；NRO/ELF及SHA，不进源码仓
```

文档中逐项标“离线验证/完整链接/已打包/已部署/真机通过”，不可混写。未证实内容必须保留“假设/未知/待验收”。更新总方案状态表和第三方许可；未经指令不push或Release。不读取/输出key、token、配对配置或nxjs/forwarder日志。

## 10. 必读材料与逐阶段索引

以下路径为本地完整路径，按顺序读；历史段落中的未完成状态以相应日期和最新阶段报告为准。

1. 总方案：`C:/uu/current/YOUYOU-Remote-ns/docs/tv-bitrate-adaptation-plan-2026-11-04.md`
2. 全链证据及更早逐次报告路径索引：`C:/uu/current/YOUYOU-Remote-ns/docs/tv-bitrate-chain-dossier-2026-11-04.md`
3. 保护清单：`C:/uu/current/YOUYOU-Remote-ns/docs/tv-port-protected-paths-2026-11-04.json`
4. S2：`C:/uu/current/YOUYOU-Remote-ns/docs/tv-adaptation-s2-2026-11-05.md`
5. S3 transport：`C:/uu/current/YOUYOU-Remote-ns/docs/tv-adaptation-s3-transport-2026-11-05.md`
6. S3 Client：`C:/uu/current/YOUYOU-Remote-ns/docs/tv-adaptation-s3-client-2026-11-05.md`
7. PC SSRC修正：`C:/uu/current/YOUYOU-Remote-ns/docs/tv-adaptation-s3-ssrc-2026-11-05.md`
8. 02解析：`C:/uu/current/YOUYOU-Remote-ns/docs/tv-adaptation-v2-parser-2026-11-05.md`
9. 02编码：`C:/uu/current/YOUYOU-Remote-ns/docs/tv-adaptation-v2-encoding-2026-11-05.md`
10. 02调度：`C:/uu/current/YOUYOU-Remote-ns/docs/tv-adaptation-v2-scheduler-2026-11-05.md`
11. 协商/等待/回退：`C:/uu/current/YOUYOU-Remote-ns/docs/tv-adaptation-v2-negotiation-2026-11-08.md`
12. 02边界：`C:/uu/current/YOUYOU-Remote-ns/docs/tv-adaptation-v2-boundaries-2026-11-08.md`
13. REMB能力准入：`C:/uu/current/YOUYOU-Remote-ns/docs/tv-adaptation-remb-admission-2026-11-08.md`
14. S4-A函数证据/输入/cap纠正：`C:/uu/current/YOUYOU-Remote-ns/docs/tv-adaptation-s4-rbe-foundation-2026-11-08.md`
15. S4-B实现/结果：`C:/uu/current/YOUYOU-Remote-ns/docs/tv-adaptation-s4-rbe-subset-2026-11-08.md`
16. 独立工程命令和兼容说明：`C:/uu/study/youyou-rbe-subset-20261108/README.md`
17. 独立测试源码：`C:/uu/study/youyou-rbe-subset-20261108/replay_test.cpp`
18. 固定来源锁：`C:/uu/study/youyou-rbe-subset-20261108/upstream-lock.json`及`source-tree-lock.json`
19. S4-B产物/保护/测试事实：`C:/uu/releases/diagnostics/tv-adaptation-20261108-rbe-subset/verification.json`

原始TV反汇编位于上述case的`evidence/bitrate-official`，S4-A索引及归档含具体文件和SHA；不要把发送控制类偏移、手机行为或其他版本API混作TV已证实路径。

## 11. 开工与收尾勾选表

- [ ] 核对当前产品树、震动改动和保护基线，保存本地差异。
- [ ] R1输入/分流/切换/Process/代际测试完成。
- [ ] R2节流/SRTCP/反馈所有权/回退完成，cap未决项明确记录。
- [ ] R3默认及实验配置完整同宏构建、功能回归、manifest和NRO完成。
- [ ] R4真实反馈与码率、01/02及传统模式、连接/重连、操作感和震动回归完成。
- [ ] 更新状态、证据路径和许可；残余问题逐项记录，不宣称逐位等同官方或保证码率。

当前交接状态：以上待办均未在本文件任务中执行。只新增执行方案，不改代码、不构建、不部署。

2026-11-08后续实施状态：见`C:/uu/current/YOUYOU-Remote-ns/docs/tv-adaptation-r1-observation-2026-11-08.md`。R1输入/封装与默认OFF只读Client挂载已做定向离线验证；RTT来源、R2输出/自动回退、R3/R4完整验收仍未完成。上面的开工清单保留为原交接时点，不以本追加段落将其全部勾选。

最新R2执行：`C:/uu/current/YOUYOU-Remote-ns/docs/tv-adaptation-r2-feedback-2026-11-08.md`。原版节流/独立SRTCP输出及保守互斥回退已做实验实现；ON6项/OFF5项测试通过。仍需RTT/cap证据闭合、R3可重建依赖/全功能回归/NRO与R4真机验收；不把阶段代码和离线通过当成最终完成。

最新R3执行：`C:/uu/current/YOUYOU-Remote-ns/docs/tv-adaptation-r3-candidate-2026-11-08.md`。依赖获取/锁/构建规则与许可入源码，干净下载验证及29+8项离线回归通过，本地主候选与经典对照NRO已生成并核验。R3的真机功能部分仍归R4，未部署/发布；RTT/cap等未决问题不因打包而消失。
