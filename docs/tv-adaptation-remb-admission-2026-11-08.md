# REMB 准入接统一协商能力

2026-11-08。只调整协商和反馈状态；编解码、组帧恢复、播放、原生GPU以及quality/field8构造不改。未生成新NRO、未部署或连接真实服务器。

## 问题与改动

旧路径在远端SDP视频段查找PT96或通配`goog-remb`文本就设置`rembAvailable_`，没有把本地能力、共同codec/PT、方向、mid、原始单传输证据及当前安装描述合起来准入。

现在由既有`inspectMediaFeedbackCapabilities()`输出双方共同能力，`FeedbackCapabilitiesResult::allowsRemb(video_->mid(),96)`只接受目标视频mid/PT的精确`goog-remb`项。Client必须有双方原始能力快照、实际Peer处于Stable，且两份快照分别匹配当前安装的local/remote描述，才把结果用于准入。保留原始重复属性检查、单BUNDLE支持范围和已有H.265参数检查；没有用这个能力选择器替代完整codec协商。

仅撤销布尔值仍不够：`RtcpReceivingSession`保存已请求目标，SR到来时会重复。新增复用的`invalidateFeedbackCapabilities()`统一清缓存、准入和本地接受门，并通过`requestBitrate(0)`清除RTCP保存目标。该0值只清本地状态，不发送零带宽REMB；没有身份/首RTP且无保存目标时，原处理器可以返回false，不强行发包。

## 生命周期

- local/remote描述更换、解析失败、控制权建立的新peer以及断开时，撤销旧能力/目标；原始快照由对应调用点清理或替换。
- 所有signaling-state通知都查询Peer的实际当前状态；不只按排队事件携带的旧枚举作准入决定。实际非Stable时撤销已有准入，新的Stable状态重新核对两份安装描述。
- 应用码率入口增加实际Stable检查。首主RTP、目标去重、有限失败重试、本机SSRC/CNAME和compound RTCP保持原有规则。
- TWCC运行时继续压住cap-REMB；TWCC失败先停止/分离，然后沿当前已协商REMB资格恢复。不是无条件恢复旧能力。
- 用户切档的RPC、Q1–4/Q6语义、field8单位和保存流程不变。跨模式重建轨道的既有处理保留，不在本轮迁移配置。

原始能力投影比较仅在描述/信令事件执行，不在每包/每帧解析SDP。没有新增线程、采集或UDP诊断schema。撤销在Client处理事件及现有RTCP锁串行清状态后生效；不宣称取消此前已经提交的在飞报文。

## 验证

发送ON/OFF各3/3定向测试：`feedback_capabilities`、`feedback_pipeline`、`remb_session`。

新增能力用例覆盖双方共同反馈、任意一侧缺REMB、参数不匹配、错误mid/PT、codec不匹配、音频单独声明、无原始单传输证明和通配反馈。保留方向、重复属性、安装描述变化、01/02及会话继承测试。

集成真实RTCP session及RembFeedbackGate验证：共同能力准入30M→本地接受→撤销并清保存目标→后续SR只包含RR/SDES→新共同能力准入12M→正常提交新目标。测试独立解析compound包类型和REMB标识；通过不等于PC已收到/采用目标。既有新会话首RTP、TWCC失败/REMB恢复和本机身份测试保留。

默认OFF和实验V2/发送ON的Switch完整ELF链接；最终构建状态、10个保护文件哈希与源码差异见`C:/uu/releases/diagnostics/tv-adaptation-20261108-remb-admission/verification.json`。host发送开关恢复OFF。初次构建误选只存在于CORE_ONLY配置的remb_feedback_test，未运行该目标；改为现有RTC配置的三个目标并全部通过，失败日志保留。

没有新NRO或真机验收。旧构建目录NRO不是本轮产物，不能直接用它测试这些修改。

## 尚未完成

这一步解决的是能力准入与旧状态撤销，**没有实现传统RBE带宽估计器**。兼容路径仍以用户cap构造REMB；后续需核对传统估计器的输入、abs-send-time/toffset切换、observer cap来源、估计/限幅/节流及可复用源码版本。既有PC-only边界不变，不用自拟涨降控制律代替原版。

本阶段只证明离线组件行为和完整链接；真实协商、切档、重连、失败回退与体感仍需后续独立包验收。此前操作感压力继续后置，不宣称这一步提升实际码率或流畅度。
