# R2：真实RBE输出、原版节流与认证SRTCP

日期：2026-11-08。前置：`C:/uu/current/YOUYOU-Remote-ns/docs/tv-adaptation-r1-observation-2026-11-08.md`。这是默认关闭的受控实验实现，不代表完整TV复刻或已取得真机收益。

## 1. 实际接通的链路

```text
认证后的RTP / 早期到达元数据
→ 当前generation的AST或TOF估计器
→ 固定WebRTC版本原版RembThrottler
→ 仅保留最新一份待发送结果（估计值＋真实SSRC集合＋ticket）
→ Client轮询、预算及失败退避
→ RR（零报告块）＋SDES（现有CNAME）＋REMB
→ 当前peer绑定的认证SRTCP回调
```

不是读取窗口码率后自行加减，也不把field8当估计值或接收observer cap。原版200ms/明显下降提前输出门保留，cap setter未擅自调用。算法、媒体输入和GPU源文件均不修改。

## 2. 为何不用requestBitrate直接提交估计

当前SDK的requestBitrate非零请求会保存目标，后续每个SR都可能重复它。RBE输出若直接接该接口，会形成独立于原版节流器的重复发送路径。

因此RBE接管前仅调用requestBitrate(0)清除旧状态，不发送零带宽包；然后通过认证全局媒体回调发送复合反馈，不再保存RBE目标到该SDK状态。已有主视频session继续处理准确RR/SDES、序号/丢包/jitter/LSR/DLSR及NACK/PLI。新增反馈前缀采用零报告块RR，不伪造上述统计，不修改PrimaryVideoRtcpSession或其媒体调用。

REMB保留原始估计输出的全部SSRC，不把RTX强改为主视频SSRC；media-source header保持0，FCI携带实际列表。编码使用REMB指数/尾数，数量不超过255、复合明文RTCP不超过1200字节。超范围明确停止候选发送，不截断或伪造列表。

## 3. 组件与线程边界

- `include/authenticated_feedback_endpoint.hpp`、`source/authenticated_feedback_endpoint.cpp`：从原TWCC attachment提取现有endpoint，供两条反馈路径复用，保留原callback-copy隔离、绑定一次、短提交锁和stop屏障。未修改SDK认证实现。
- `source/receive_bandwidth_observer.cpp`：原版节流输出只进入单槽mailbox，网络提交只在pollFeedback。估计器锁、输出mailbox锁及预算锁均不跨网络回调。
- RBE只在首次需要时绑定认证回调，后续收包不重复获取发送锁。认证RTP或RTCP都可完成绑定；控制包仍不喂入估计器。这覆盖已经有真实视频估计、但静态桌面暂时只有SR/RR到达的情况。
- 不新建媒体线程/定时线程，不调整优先级、绑核、频率或队列。
- 新报告可替换尚未发送的旧报告。发送旧ticket成功后，仅清除同ticket；并发产生的新报告必须保留。单槽限制不等于整个估计器/进程内存上限。
- `startFeedback`校验一次本地identity/policy；这些typed内部值不在每次编码时重复检查。

## 4. Client准入、互斥和回退

新增`UU_RBE_CLIENT_SEND`，默认OFF；必须同时开启RBE观察、统一时间戳ABI、反馈foundation与兼容REMB。foundation只是复用预算/endpoint/类型，不意味着广告TWCC。

| 状态 | 行为 |
|---|---|
| 描述失效、未共同允许REMB、peer未就绪 | 不启动；能力失效时关闭旧观察器/发送端并清旧保存目标 |
| TWCC已占全局handler | RBE不替换它、不重排活跃chain、不同时发送经典反馈 |
| TWCC正常等待02请求 | 保持现有等待规则，不以零提交触发RBE |
| TWCC明确失败 | 先停/分离TWCC，再尝试挂RBE观察器；此前兼容路径仍可用 |
| 只有transport-sequence包、尚无经典估计 | 不用这些包编造RBE，也不抢占兼容路径 |
| 真实经典估计有效，且已见主视频RTP/SSRC | 清SR保存目标、使兼容gate失效、启动RBE输出；暂停兼容cap-REMB |
| RBE启动拒绝/终止 | 释放兼容抑制，gate失效后恢复兼容入口；不反复重启同一失败attachment |
| 断开/重连/描述变更 | 停新输出，等待已准入短提交完成，旧实例不能投递到新transport |

这是一条**保守的模式互斥路径**。没有实现官方控制器中所有可能的混合流/并行反馈组合；TWCC活跃时不额外观察非TWCC包，不宣称全模式逐位等同TV。

兼容回退仍遵守旧预设/Q6/field8语义，不将所有预设改为Q6。UI/数值诊断中的REMB目标、接受次数在RBE接管时改取实际本地提交结果；不是PC已采纳或实际吞吐。

## 5. 发送策略与错误语义

复用现有实验反馈参数：16000字节/秒、4800字节burst/初始credit、1200字节关键反馈预留；失败退避20ms起、200ms封顶、连续3次失败停止；待处理报告最长1秒；网络开销使用现有保守256字节allowance。它们是客户端实验策略，不是已证明的TV动态配置。

预算覆盖本候选反馈，不自动覆盖未适配的RR/NACK/PLI；不改变它们的发送策略。模式交接新建活动反馈预算，未连续继承旧模式token历史，不能称整个传输总RTCP开销已被精确约束。

发送抛异常只在实际外部SRTP提交边界隔离，不记录异常文本/数据，不中断媒体。失败不提交成功ticket；替换报告不会清除失败退避。预算不足延后，过期、时钟回退、尺寸不支持或失败达到上限均有明确终止原因。

RX可在调用者取now后产生新结果；未来于该采样点的mailbox返回等待，不错误判为过期。到达时间、处理时间和轮询时钟使用同一steady_clock纪元；没有用毫秒向下取整的now去误判微秒报告。

## 6. RTT及cap未决项的当前结论

**确认现有peer_->rtt()来自SCTP，不能直接当媒体RTCP RTT。**当前SDK `src/peerconnection.cpp`的rtt()调用getSctpTransport()->rtt()。固定估计器OnRttUpdate把平均RTT用于AIMD，不可随意用数据通道RTT替换；本候选未接入它。

目前保留固定上游AIMD的200ms默认RTT，日志明确`rbe-reference-default-cap-unlimited-media-rtt-unavailable`。200ms是当前参考源码的默认值，**不是已核实的TV运行时RTT**。

TV接收cap动态来源、精确WebRTC commit和Field Trial仍未闭合。候选保持参考默认无限observer cap，field8只留在PC请求侧，不推断已突破PC编码硬上限。上述差异必须随测试包告知，不能把“输出已接通”写成完整官方对齐。

## 7. 验证证据

归档：`C:/uu/releases/diagnostics/tv-adaptation-20261108-r2-output`。

- `test-host-latest.log`：发送开启6/6通过，包含真实估计器、只读生命周期、完整输出、多SSRC、能力准入、SDK清目标/SR行为及原TWCC反馈管线。
- `test-host-off-latest.log`：发送关闭5/5通过。
- 新输出测试从真实RTP输入产生估计，未向私有callback注入自拟结果。检查RR/SDES/REMB长度、sender、CNAME、media-source=0、两个原始SSRC、非零反馈值、消息引用保留及仅poll发送。
- 覆盖未start不发、重复start拒绝、无预算/CNAME不合法、退避、3次失败、预算延后、过期、时钟回退、endpoint不换绑/关闭禁发。
- 阻塞合成发送回调时继续500包真实估计输入，验证RX不等待已绑定endpoint、最新mailbox不被旧ticket提交误清。
- 已有真实RTP估计后，只有认证控制消息也能完成endpoint绑定；控制消息不成为视频带宽样本。
- Switch配置及最终产物、源hash、保护检查以`verification.json`为准：TWCC优先、纯RBE发送、默认实验OFF分别完整链接。

这些都是离线/链接证据。测试捕获的是进入SRTCP前的复合明文RTCP，不是已经连到UU PC、通过网络认证或取得PC编码响应的证明。无新NRO、无部署/采集，不宣称流畅度或码率改善。

## 8. 接下来的工作

1. 保留RTT/cap等未决项，继续有据定位，不拿方便取得的值代替协议输入。
2. R3：发布可重建依赖布局、完整既有功能/震动回归、候选NRO及manifest，明确模式开关与回退。
3. R4：按同设备/PC版本/场景/频率/编码/缓冲，对比真实反馈、实际RTP、连接重连、画质/操作感；02必须实际协商并有请求，不把未协商写为互通通过。

编解码/播放/GPU保护清单10项及震动源/测试文件再次核验；共用Client的主视频RTCP类与R1逐字一致。原算法源码锁3016文件核验，源路径与具体结果见归档。本文不授权推送或发布。
