# TWCC-02：无时间戳反馈编码阶段

> 2026-11-08更正：本页是早期阶段记录。后续复核799ddc..799e80发现T=0跳过delta计算并把已收状态设为small-received，先前“仍计算delta/保留large符号及delta范围拆分”的判断不准确。当前已按[边界复核](./tv-adaptation-v2-boundaries-2026-11-08.md)修正；以下对应旧阶段代码与测试，不作为当前编码契约。

2026-11-05。继续移植，不处理暂后置的操控感优化；原s3ssrc包保留，未写设备或发布新包。

## 已追到的原版语义

固定TV库：`7b2b00`将请求的include-timestamps传给`799af8`，后者在对象`+0x1d`保存该值。`799db8`仍计算接收delta及状态，`79a838`输出参考时间和状态chunk，`79a9f4..79aa50`只在该布尔为真时序列化接收delta字节。

所以T=0不是新的ACK格式，也不是所有已收包都改成同一状态；它保留原有small/large received符号和参考时间，但省略delta字节。当前PC同节码样本的`18027a7a4`会依据状态所需delta总长与剩余payload比较；不足时进入无时间戳解析分支。此结论只来自静态代码，不是PC实际接收测试。

证据位于既有case的`evidence/bitrate-official/tv-v2-wire-parser-20261105.asm`及新增`tv-v2-no-timestamps-ctor-20261105.asm`；PC依据`evidence/pc445-twcc-20261105/function-18027a7a4.asm`，4.45/4.46模块文件节一致性已单独核验。

## 源码与验证

`encodeFeedback()`增加默认true的includeTimestamps参数。false只省略接收delta的字节存储/输出，保留状态、参考时间、序号、count、MTU/padding和delta范围检查；默认01编码不变。没有把这个参数直接接到Client或协商准入。

测试独立解析线格式，覆盖small/missing/large组合、1/7/8/511/512状态边界、序号跨65535、feedback count255、padding、有/无时间戳前缀一致、全缺失显式参考、空输入拒绝及过大delta要求分包。原能力与REMB互斥测试保留；ON/OFF各3/3通过，Switch辅助库编译通过。

10个编解码/恢复/播放/GPU保护文件哈希一致。归档：`C:/uu/releases/diagnostics/tv-adaptation-20261105-v2-requests/`，含before、changes.patch、日志、辅助库及verification.json。未新建NRO，不把新库手工混入旧对象。

## 下一项：请求调度的真实边界

尚未实现请求式调度，不能据此启用02。它必须解决：

- 使用到包时解开的transport序号，范围为[current-count+1,current+1)，不能拿16位值直接相减跨回绕。
- 按本地实际保留历史生成反馈；超出历史和单包512项时必须明确分包/不可用结果，不能无限分配或伪造时间。
- 多请求到达、失败重试与提交必须事务化且有界；不能覆盖尚未提交的范围或在失败后提前推进。
- 02不再由01的100ms周期自动产生报告；请求的T位分别选择现有两种编码，预算与停止机制仍适用。
- 原始协商、01/02切换和真实互通最后接入。传统RBE/REMB移植仍未完成，不用自拟控制律替代。

本阶段没有需要用户执行的测试步骤；继续源码和离线验证。
