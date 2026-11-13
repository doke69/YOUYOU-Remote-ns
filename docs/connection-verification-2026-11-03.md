# 连接分析报告修复与验证状态

核对对象：`C:/uu/current/YOUYOU-Remote-ns` 当前工作区；对应原报告 `connection-analysis-2026-11-03.md`。

结论：不能标记为全部修复。原报告包含源码事实、设计风险和待证假设；本轮已实现可观测性补充和一个有限的 TURN 故障恢复路径，尚无真机闭环证明个别用户建连失败已解决。

## 逐项核对

| 原报告项目 | 当前代码状态 | 验证结论 / 剩余边界 |
|---|---|---|
| 5.1 单一 45 秒总期限 | 增加从 WebSocket open 后起算的阶段耗时、剩余预算、成功/超时/失败耗时 | 计时策略仍为原 45 秒，没有进度续期；日志未证明增加期限能解决故障。不是已修复的超时根因 |
| 5.2 Peer 回调队列满静默丢弃 | 增加拒绝事件类别、次数和高水位，成功/失败时汇总 | 已消除“无可见记录”的缺口；2048 门槛下仍会拒绝事件，没有交付保障变更，未做满队列客户端集成验证 |
| 5.3 SOAC/candidate/log 各有界队列 | 原 128/128/512 行限制及行为保留 | 原报告为不同语义说明，非全部属于应删除的缺陷；日志 writer 仍可能丢旧行 |
| 5.4 TURN 桥故障未消费 | 周期读取 failed()，关联连接代次、桥序号、Peer/ICE 状态；所有桥失败且 Peer Connected、选中本地 Relayed 时抛可重试错误 | 编译通过；错误映射测试通过。未做真实/注入 TURN 桥故障恢复验证；建连中、无选中候选或仍有健康桥时只记录，实际失败时刻仍不是精确 bridge 时间戳 |
| 5.5 Peer Connected 不等于媒体就绪 | 增加首个音视频 RTP、严格 AU 接受/legacy AU 发出、解码帧发布、首帧呈现提交标记 | 只完成诊断分离；UI streaming 仍沿用 Peer Connected，不是独立媒体就绪状态。Client 标记由约 500ms 轮询首次观察，不能推断精确事件时间或同次采样中的先后；submit 不代表 scanout |
| 5.6 数据通道较小队列门槛 | 统计高水位、拒绝批次数和首个匹配类别 | 既有 >124 门槛/整批拒绝规则保留；多类别同批时类别标记不是完整逐类计数，尚未验证运行中实际触发 |
| 5.7 两秒 nomination 窗口 | 保持原 ICE 策略和 force_relay 语义 | 没有重写选路；报告是行为边界，尚无证据支持改 nomination 或强制 LAN |

高水位是对应 callback 观察到的**共享 Inbox 总长度**，不是该类别独占的排队数量，也不覆盖所有 UI/本地命令生产者的全局最高值。统计在每次 connect/retry 开始重置。

## 本轮已执行验证

1. 读取 `C:/uu/LAYOUT.md` 核实规范源码和构建入口。实际现有构建目录为 `C:/uu/repo/.tools/tmp/current-build-nx`，其 CMAKE_HOME_DIRECTORY 指向当前规范源码。此前仅检查源码下 build-nx 就声称没有构建环境的判断不完整。
2. 使用该缓存与现有 Switch 工具链编译链接 `UURemote-ns` ELF 目标，退出码 0。没有生成/发布新 NRO 或部署设备。构建有 warning，不能称零警告；主要涉及现有 UI 缩进/未使用符号及依赖声明。
3. 由当前源码重新编译并运行 6 项离线 host 测试，均 exit=0：`rtc_diagnostics`、`event_queue`、`input_admission`、`present_identity`、`signal_pump`、`signaling_envelope`。新增 TURN 错误映射断言在 rtc_diagnostics 测试中执行。
4. `git diff --check` 通过。

测试范围：诊断映射/敏感内容过滤、事件异常与公平性、输入代际准入、呈现身份、信令泵公平性、SOAC envelope 验证。它们不验证真实 TURN/ICE 互通、不执行完整 Client 的满队列压力、不能证明选中中继桥故障自动恢复或 UI 首帧体验。

验证日志：
- `C:/uu/repo/.tools/tmp/connection-verify-20261103-build.log`
- `C:/uu/repo/.tools/tmp/connection-verify-20261103/host-tests.log`
- ELF：`C:/uu/repo/.tools/tmp/current-build-nx/UURemote-ns.elf`

首次 host 编译因系统 TEMP 写入受限失败；设置专属 ASCII TEMP/TMP/TMPDIR 后全部重编译运行通过，没有修改系统 ACL。

## 用户补充日志

`%USERPROFILE%/Downloads/uu全部文件/connection.log` 的独立分析见 [连接失败日志分析](connection-log-analysis-2026-11-03.md)。两次完整失败在 Peer Connecting / ICE Checking 超时；没有选中中继桥全部失效的证据，不能用于宣称本次恢复修复成功。其构建标签为 0.3.73-beta，实际包由用户标识为 0.3.74，缺少二进制无法核实源码对应。
