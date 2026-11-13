# R1：接收估计器封装与认证后只读观察

日期：2026-11-08。接续S4-B及剩余工作交接方案。用户授权继续实施，手柄震动由另一路开发，本轮保留其代码。

## 已实现

- 新增估计器封装的真实算法回放、AST/TOF切换与关闭屏障测试，不用替代算法。
- 修正Process调度：保存实际下次处理期限；AST返回无周期期限，切回TOF时清掉该期限，避免永久跳过TOF处理。不新建媒体线程、不忙等。
- `ReceiveBandwidthObserver`实际读取认证后全局媒体handler里的RTP，复用现有输入边界提取payload+padding、时间、SSRC和协商扩展；不复制/保留媒体消息、不改payload/引用/派发结果、不持有surface。
- 专用Clock使用与ICE早期到达元数据相同的`steady_clock`纪元，不把WebRTC平台Clock的未指定纪元直接当成相同。
- Client在当前描述已匹配且共同视频REMB准入有效时可挂观察器；描述失效、重连及断开时close并分离。关闭等待已准入短回调结束，不是整个客户端退出硬超时。
- 原Client循环调用Process；每2秒输出固定数值诊断，包含输入数、AST状态、transport跳过、无时间戳/畸形、估计次数及原始估计bps。
- **只有观察，没有新的REMB输出发送。**既有兼容cap-REMB保持原逻辑。

## 开关、依赖与共存

新增`UU_RBE_OBSERVE_CLIENT`，默认OFF；开启必须全树`UU_RECEIVE_TIMESTAMPS=ON`。`UU_RBE_SOURCE_DIR`目前显式指向本机固定S4-B工程，核验其WebRTC/Chromium third_party revision，不联网下载。该本机依赖是阶段集成，发布用vendor布局和完整第三方清单仍属R3。

上游算法源码未改。其头文件按SYSTEM导入，避免旧上游未使用形参告警被项目Werror升级；自有适配库保留`-Wall -Wextra -Werror`。原平台bridge及cstdint兼容选项保留。

本阶段采用明确保守边界：已存在全局handler（包括TWCC）时RBE不挂载，不改动活跃chain，日志`rbe-observation-global-handler-occupied`。如果TWCC和RBE都开启，优先已有TWCC，不能把RBE没有数据解释为失效。TWCC失败后自动切入RBE及反馈所有权交接尚属R2，没有实现“全模式自动降级完成”。

## 已完成验证

- 独立封装与原始算法回放2/2通过；封装加入产品依赖后，定向5/5通过：estimator、observer、feedback_capabilities、remb_session、feedback_pipeline。
- AST/TOF真实限速回放仍为12,019,600→2,040,000bps；这些是模拟输入下的原始估计，不是PC实际编码码率。
- 30包阈值、排除包不推进计数、AST零值有效、切换后Process唤醒、RTT方法调用、流移除、新实例隔离、关闭后无输出通过。
- 观察器测试验证原消息指针/内容/引用数不变、缺时间戳拒绝、控制消息忽略、没有send调用；它直接调用handler，**不是网络SRTP认证互通测试**。认证后的实际挂载位置另按现有PeerConnection实现核对。
- Switch完整ELF已分别验证TWCC共存、纯经典只读观察和全部实验OFF三种配置；前两者存在实际RBE调用符号，OFF产物无这些符号。具体hash见归档verification.json。没有生成NRO，没有部署或真机结果。
- 非法“RBE ON / 时间戳OFF”配置明确拒绝。初次检查忘设工具TEMP，在编译器探测阶段失败，补设后验证到预期门；两份日志均保留，首次失败不算开关测试通过。
- 初次完整编译因上游头部告警失败，正确标记第三方头后host和Switch再编译；失败日志保留，不修改算法或关闭自有Werror。

## 尚未完成，必须保留在交接中

1. R2：估计结果接原版RembThrottler/SRTCP，TWCC/兼容REMB所有权、回退及保存目标协调。
2. 接收cap设置来源、TV精确commit和Field Trial仍未知，不把field8当observer cap，不把测试0.65设为默认。
3. 已有`rttUpdate`接口和离线测试，但Client未向估计器送RTT：现可见的peer RTT来源不能未经核对就当作媒体RTCP平均/最大RTT。发送启用前明确该差异，不能宣称全部R1输入完全对齐TV。
4. 运行中独立SSRC移除接口已测；Client当前通过描述失效/整会话close移除所有已见流，未新增单流动态控制事件。现有上游超时逻辑保留。
5. R3发布依赖布局、所有配置和既有功能完整回归、NRO/manifest；R4真机01/02与经典模式的操作感/码率/连接回归。

## 路径

源码：`C:/uu/current/YOUYOU-Remote-ns`。

归档：`C:/uu/releases/diagnostics/tv-adaptation-20261108-r1-observation`，包含各配置日志/ELF、验证清单及本轮源快照。

host：`C:/uu/repo/.tools/tmp/rbe-r1-rtc-codex-20261108`。

Switch：`C:/uu/repo/.tools/tmp/rbe-r1-nx-codex-20261108`，最终恢复为本任务实验开关OFF；不同配置ELF先独立归档，不混装。

保护：10项编解码/播放/GPU哈希再次核对，具体结果见verification.json；共用Client只新增反馈观察调用和能力失效清理，不改变原媒体处理函数。震动实现与测试文件保留，未以本轮离线结果宣称震动真机通过。
