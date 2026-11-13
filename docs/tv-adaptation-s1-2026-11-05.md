# TV 适配 S0/S1 首轮实施与验证

日期：2026-11-05。对应 [移植方案](./tv-bitrate-adaptation-plan-2026-11-04.md)。本轮已改源码并编译，但未部署、未真机验证，也未启用新反馈发送。

## 完成内容

1. S0：修改前保存当前五个涉及文件、既有工作树差异和受保护文件清单，不 reset/clean。
2. 将现有能力解析拆为独立 `uu_feedback_negotiation` 链接目标；无须打开接收时间元数据或 TWCC，默认连接也能记录协商能力摘要。
3. 复用 `NegotiationCapabilities`，增加双方公共反馈、codec/PT/clock、RTX apt 关联及接收扩展列表的类型化结果。
4. 按 mid 和接收/发送方向匹配，反馈保存完整名称/参数对；处理媒体通配 feedback，交集排序去重。
5. 分开保存本地接收 ID 和远端发送 ID；02 只记录版本，仍由原 TWCC 准入检查拒绝。能力摘要的 Ready 不授权反馈发送。
6. 在现有 local SDP、成功应用的 remote SDP 和 Stable 事件上刷新；断开/重协商清状态，继续使用既有事件代际准入，不增加后台任务。
7. 能力投影保留 fmtp 供 apt 检查；不保留 ICE 凭据、候选、指纹或原始 SDP 副本。输出只使用固定状态/布尔/计数/PT/ID，不输出 mid 或任意反馈字符串。

## 定向测试发现并修正的问题

当前 libdatachannel 的 `extMap(id)` 和 `rtpMap(pt)` 在缺项时抛异常，不是返回空指针。

旧 TWCC 检查曾从1到255探测扩展并检查空指针；本轮新增能力提取最初沿用了该错误假设。主机测试异常退出，调试栈定位到 `Entry::extMap`。现改为遍历已声明 ID，并在本地 PT 不存在时跳过，避免用异常作正常查找流程。

SDK 的 extIds 非 const；协商模块通过复制小型、已脱敏 Entry 取得ID列表。它仅在描述更新时执行，不在 RTP/解码热路径执行，没有修改 SDK、GPU或媒体数据。

## 未变内容

- `Client::applyBitrateCap` 与本轮修改前逐字一致；没有把上限改成新估计值。
- REMB 报文及 session override、NACK/PLI/RTX 恢复策略未改。
- 默认 TWCC FOUNDATION/ADVERTISE/OBSERVE/SEND 和接收时间元数据继续 OFF。
- 观察分支不调用 startSending；原最终描述匹配和全局观察器准入门没有删除或放宽。
- quality/field8、配置、认证、音频、输入、UI、解码、组帧、原生GPU与播放文件未修改。

## 仍待实施/验证

- 当前公共反馈使用 SDK 表示的完整字符串匹配；名称/参数大小写及其他合法语法兼容性仍需后续核对，不能称已逐字复刻官方 codec 协商。
- 此处只匹配 codec 格式/clock/PT，不代替完整 profile/fmtp 合法性协商；既有H.265校验和解码入口不变。
- S1 首轮功能已可编译/离线验证，但未完成真实会话状态/生命周期验收。
- S2 的旧 stale-capabilities 运行问题尚未复现并解决；本轮修正的是明确的 API 查询错误，不能把两者认作同一根因。
- 未让媒体观察默认启用，未恢复官方未知的初始能力集合，未启用 TWCC-02 或发送器。
- S3/S4 尚未进入；不把本包称为码率提升或官方BWE完成版。

## 验证结果

| 检查 | 结果 |
|---|---|
| 主机能力测试 | 通过：反馈交集/通配/参数区别、RTX apt、01/02、不同ID、方向、角色、codec不匹配、缺扩展、重复ID、BUNDLE/mux边界、音视频隔离、凭据不影响能力 |
| 既有 REMB session 测试 | 通过 |
| CTest 本轮所选测试 | 2/2，通过；没有执行联网loopback测试 |
| Switch 默认配置 | 独立新目录全量编译/链接/打包成功，最后源码变更后已增量重编译 |
| Client观察/广告宏 | 语法检查通过；不等于ON配置全量互通或最终包已验证 |
| 受保护媒体/GPU/播放文件 | 10/10 SHA-256与2026-11-04清单一致 |
| REMB apply函数 | 与本轮前一致 |
| diff空白检查 | 通过 |
| 真机/部署 | 未执行 |

## 源码范围

```text
C:/uu/current/YOUYOU-Remote-ns/CMakeLists.txt
C:/uu/current/YOUYOU-Remote-ns/include/client.hpp
C:/uu/current/YOUYOU-Remote-ns/source/client.cpp
C:/uu/current/YOUYOU-Remote-ns/include/transport_feedback_negotiation.hpp
C:/uu/current/YOUYOU-Remote-ns/source/transport_feedback_negotiation.cpp
C:/uu/current/YOUYOU-Remote-ns/tests/feedback_capabilities_test.cpp
```

## 交付和恢复材料

目录：`C:/uu/releases/diagnostics/tv-adaptation-20261105-s1/`

- `before/`：本轮前涉及文件副本。
- `pre-existing-diff.patch`：这些文件在本轮前相对HEAD的已有差异。
- `S1-only-changes.patch`：只包含本轮代码改动，避免回退覆盖用户原改动。
- `protected-baseline.json` / `verification.json`：保护与产物核对记录。
- `host-configure.log`、`host-build.log`、`host-rebuild.log`、`host-final-build.log`、`host-test.log`：主机编译及定向测试；初次失败保留在构建目录Testing日志/本报告中。
- `nx-configure.log`、`nx-build.log`、`nx-final-build.log`：Switch构建记录。
- `client-observe-syntax.log`：观察/广告宏Client语法检查记录。
- `UURemote-ns.nro`、`.elf`、`.nacp`、`.manifest.json`：本地候选产物，未上传。

NACP：`0.3.75-tv-s1`；NRO 42,728,912字节，SHA-256：

```text
75d964f262436c95dde8d5fbc49526f83811de7224bc3d228cdad0c5fa806032
```

后续从S2的描述匹配与只读观察闭环继续，保留本轮边界；不直接打开反馈发送来绕过未闭合项。
