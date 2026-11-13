# S4-A：传统RBE输入、限幅节流及可复用源码复核

2026-11-08。承接[REMB统一准入](./tv-adaptation-remb-admission-2026-11-08.md)。本轮是实现前的定向证据与依赖准备：没有修改应用C++/CMake、没有重新构建NRO、没有部署/采集。编解码、播放和原生GPU不动。此前可用cap-REMB兼容路径保留，不能把本页写成传统估计器已经上线。

## 1. 可确认的TV链

固定TV库SHA256：`e4c2445d822bd7b83024f7175dd51c320a6663070362e684f1d3b6bb99ad4814`。沿用已授权offline-sample case及guard；样本不执行、不附加进程。上游公开源码仅作为参考下载，不对UU服务器作网络测试。

| 环节 | TV地址/事实 | 适配要求 |
|---|---|---|
| 原始输入 | 66f404..66f428：arrival从微秒舍入到毫秒；size取RtpPacket+18与+2相加 | 输入为payload+RTP padding，不含RTP头、UDP/IP/TURN开销；不能用解码帧大小或窗口平均码率 |
| 分流 | 7f8afc先交proxy，header+98有transport序号则返回 | 同一包不能同时喂传统估计器和transport反馈；不以用户档位决定估计器 |
| AST选择 | 7f87d8读取header+70；有AST立即切换，且清缺失计数 | 使用实际协商扩展映射及包内存在位，不猜ID |
| TOF选择 | AST模式连续30包无AST后重建7fb7ac对应估计器 | 30是包计数；切换时重建状态，不能当30ms或丢包率 |
| AST输入 | vtable f28818+18→7f953c；读header+74的24位AST、header+8 SSRC，并把arrival ms转us | 保留24位发送时钟及回绕语义，不能用本地接收时间替代发送时间 |
| TOF输入 | vtable f28860+18→7fb994；RTP timestamp加transmission offset，按90kHz视频时钟使用 | offset是带符号的RTP时间增量，不是毫秒；加法保留32位回绕 |
| 构造 | 7f8944→7f8e7c(AST)；7f8994→7fb7ac(TOF) | 两个估计器共享observer和Clock，原始到包与处理时钟职责要分清 |
| 周期处理 | 7f8bb4调用当前RBE虚表+30，再与proxy处理期限取较小值 | 不能只喂IncomingPacket而漏掉TOF的Process；无需新增媒体线程 |
| 输出 | observer 7fc4d4→7fc5e4→7fc63c→既有REMB链 | 输出应是估计结果与其媒体SSRC集合，不是field8回读值 |

尺寸旁证来自固定上游`rtp_packet.h`的成员顺序：padding_size_位于+2、payload_offset_位于+10、payload_size_位于+18（64位该布局）。TV的6fa54c也分别把padding/header长度写入RTPHeader，与66f424的相加一致。

## 2. RembThrottler完整判定，及旧候选纠正

TV 7fc478初始化last time为负无穷、last raw estimate与cap为正无穷。7fc4d4的输出门为：

```text
floor(raw_estimate * 103 / 100) > last_raw_estimate
且 now < last_time + 200000us
    → 暂不发送
否则：
    last_time = now
    last_raw_estimate = raw_estimate
    output = min(raw_estimate, observer_cap)
    → callback(output, ssrcs)
```

先记录的是未限幅的估计值，限幅在最后。103/100是明显下降可提前发送的比较门，不是“每次增长3%主动探测”，200ms也不是完整BWE采样算法。该行为与WebRTC公开`RembThrottler::OnReceiveBitrateChanged`逐项吻合。

**必须更正的旧线索：**7fc784不能继续当接收observer cap的setter候选。其7fc7d0从`[x0+28]`读取的是对象自身已存状态，7fc914写该对象的缓存+80；并非外部参数写入接收cap。本轮从其日志地址1a790f读到明确源文件`modules/congestion_controller/rtp/control_handler.cc`，固定上游`CongestionControlHandler::GetUpdate()`的输出缓存/暂停编码/队列判定也吻合。相同的+80偏移不足以合并两个类。

接收observer cap的动态设置来源仍未闭合，不能把field8接进去，也不能断言它始终保持无穷。上游存在`SetMaxDesiredReceiveBitrate()`不证明TV运行时调用了它；本轮没有找到实际调用桥。之后独立估计器验证可保持其默认无限cap，不能为凑“上限生效”而发明来源。

## 3. 固定参考源码与版本边界

参考根目录：

`%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/evidence/webrtc-rbe-reference-20261108/`

| 参考 | 固定commit | 用途 |
|---|---|---|
| main快照 | eeb7e6beb2e20c5a273922fdf22b75a75cc28ad5 | 确认模块职责；不是直接移植基准，新的Environment/CCFB接口与TV不同 |
| branch-heads/4896 | ffd9187dc0d9211ad52173bf0daa5001ca7d45ee | 对照旧WrappingEstimator/Process接口 |
| branch-heads/5481 | 218b56e516386cd57c7513197528c3124bcd7ef3 | 候选兼容基准：Clock/RTPHeader输入、直接RBE持有、MaybeProcess形态较接近TV |

**这些是固定、可审核的参考版本，不是已识别出的TV精确WebRTC commit。**TV可能含修改或不同field trials，不能把行为锚点一致提升为全算法逐位一致。

5481材料含两个估计器、AIMD、throttler、receive controller、RTPHeader/RtpPacket及BUILD.gn，另保存同commit的LICENSE/PATENTS/AUTHORS与文件SHA清单。源码不自动进入产品external或构建，尚无新增第三方链接依赖。

公开来源：

- [固定RembThrottler](https://webrtc.googlesource.com/src/+/218b56e516386cd57c7513197528c3124bcd7ef3/modules/congestion_controller/remb_throttler.cc)
- [固定ReceiveSideCongestionController](https://webrtc.googlesource.com/src/+/218b56e516386cd57c7513197528c3124bcd7ef3/modules/congestion_controller/receive_side_congestion_controller.cc)
- [固定构建依赖](https://webrtc.googlesource.com/src/+/218b56e516386cd57c7513197528c3124bcd7ef3/modules/remote_bitrate_estimator/BUILD.gn)
- [同版本许可](https://webrtc.googlesource.com/src/+/218b56e516386cd57c7513197528c3124bcd7ef3/LICENSE)

后续移植保留源码版权与同版本许可/专利声明；不能把WebRTC代码改标成本项目原创。依赖还包含units、field trials、rate statistics、mutex、InterArrival、OveruseEstimator/Detector、LinkCapacityEstimator及Abseil。只复制两份.cc或只接throttler不能组成完整估计器；不引入整套WebRTC媒体/解码引擎来解决这些依赖。

## 4. 后续实施顺序

1. **S4-B独立构建**：以固定候选版本剥离两个真实估计器和必需数学/基础依赖，保留算法主体。先建立可注入Clock/field trials/observer边界和host回放，不连接Client、不替换现有兼容反馈。版本或必要依赖不匹配时记录差异，不能用自拟增长/回退律填洞。
2. **S4-C输入适配**：从已认证RTP取实际arrival、payload+padding、SSRC、90kHz RTP timestamp及已协商AST/TOF；验证有/无扩展、回绕、连续30包切换、流移除、重连和Process。输入不经过解码器、呈现队列或surface。
3. **S4-D输出接入**：完整估计结果进入已核对throttler，再交当前SRTCP路径；与TWCC/兼容cap-REMB保持明确互斥和回退。observer cap来源未证时不拿field8充当它。PC编码上限请求和本地接收估计分别保存。
4. **最后真机验收**：保持同场景/频率/编码/分辨率，比较实际RTP、反馈、长间隔和操作感；不要求100M必须跑满，也不因离线估计曲线好看就宣称性能改善。

## 5. 本轮验证与限制

新增native证据在既有case的`evidence/bitrate-official/`，前缀`tv-rbe-`、日期20261108。详细索引见case报告及归档verification.json。读取样本前核对固定SHA；公开参考按commit固定并逐文件哈希。

本轮不运行新的应用测试或编译，因为产品源码没有变化；保存上阶段5个源码哈希和10个保护文件的再次核对结果。没有用“报告完成”冒充S4整体完成，也没有生成独立但无人使用的生产helper。

后续：[S4-B独立子集构建与回放](./tv-adaptation-s4-rbe-subset-2026-11-08.md)已完成host算法测试和Switch验证ELF链接；仍未产品接入或真机验证。
