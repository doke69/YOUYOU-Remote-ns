# TV 码率链证据复核及调查方向修正

日期：2026-11-04。目标是还原官方TV实际连接、请求与反馈行为，再决定YOUYOU适配/移植内容；设定上限不作为实际吞吐必达值。当前不继续应用策略修改。

## 独立复核发现：rev7/rev8 的构造参数归属错误

证据完整路径：`%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/evidence/bitrate-official/dsh-three-links/rev7-tv-dex-field8.txt`。

构造函数 `CaptureSetting::<init>(cg/s,cg/y,int,Point)` 的代码单位：

| unit | 指令编码 | 可直接解出的寄存器操作 |
|---|---|---|
| 7 | 274 = 0x0112，const/4 | v1 = 0 |
| 10 | 857 = 0x0359，iput | 对象 v0 的字段 j = v3 |
| 14 | 345 = 0x0159，iput | 对象 v0 的字段 l = v1，即 0 |

因此现有指令证据不支持 rev7/rev8 所称“整数参数 arg3 直接进入 l/custom bitrate”；参数与方法头的完整映射还需核对 registers/ins_size，但 l 使用刚置零的 v1 已直接可见。早先 TV-MOBILE-BITRATE-CODE 报告§1写的是 j=屏幕ID、l=0，与该指令一致。

字段 l 到 protobuf maxCustomBitrate 的序列化边，不等于参数/非零UI值到 l 的边已证明。还需复核 dump 中方法边界，不能将在线RPC的合并lambda误归到初始ConnectOptions构造函数。

## 已交DSH的定向任务

1. 正确解码DEX寄存器与方法边界，修订rev7/rev8及JSON，保留历史并明确更正。
2. 从 l 的真实写入点/其他入口追非零自定义值，而不是继续将q/r猜成custom bitrate。
3. 将初始连接、在线画质RPC、真实Offer生成、Answer后的扩展/反馈选择分开追，再建立调用关系。
4. 对每条边给出文件/方法/指令证据，未证实的间接边保持未知。

## 对现有实验的处理

主动加入TWCC extmap的YOUYOU观察包只能验证PC响应，不能证明TV原本协商方式。暂停将它作为移植方向；等待官方链路差异证据后决定是否保留、调整或移植。现有观察器故障和实验diff保留记录，不据其测试失败推断PC或TV能力。

未更改解码、播放、码率控制或协议代码。

## 后续独立追查：真实Peer创建与本地描述锚点

TV动态符号表已有 `LocalDescriptionObserver::OnSetLocalDescriptionComplete`（0xdf04c4）、`RemoteDescriptionObserver::OnSetRemoteDescriptionComplete`（0xde8a48）、`PeerConnectionDependencyFactory::CreatePeerConnectionFactory`（0xdf0d74）。Codex以既有ELF解析器按.eh_frame_hdr边界提取指令，并对.text直接BL/B目标交叉引用：factory的两个直接调用者是0xd1ef00（调用点0xd1f05c）和0xd472f8（调用点0xd47454），两个调用点均令w1=0。

这只是可追查的具体锚点，不证明调用者是控制端/被控端，不证明该参数是码率/捕获后端，也不能直接串成CreateOffer路径。本地描述完成回调本身亦不等于Offer构造器。已交DSH继续判定对象角色并追真实Peer/Offer及信令交接；不再重复搜索TWCC字符串。

本轮新增原始证据完整路径：

```text
%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/evidence/bitrate-official/codex-local-description-review.asm
%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/evidence/bitrate-official/codex-local-description-callers.json
%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/evidence/bitrate-official/codex-peer-factory-callers.asm
```

DSH已确认构造器寄存器并形成更正报告：

```text
%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/TV-DEX-CTOR-CORRECTION-2026-11-04-rev9.md
C:/uu/current/YOUYOU-Remote-ns/docs/tv-negotiation-parity-gaps-2026-11-04.md
```

可比性约束：TV常规预设请求应与YOUYOU原生预设比较；未经TV自定义路径证明，不能用TV预设field8=0对比YOUYOU自定义field8非零而直接宣称不兼容。当前未批准任何新的策略移植。


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
