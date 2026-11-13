# R4：修复 TWCC 扩展缺失时的启动前回退

日期：2026-11-08。候选版本：`0.3.75-r4tv`。只修反馈生命周期；未修改编解码、播放、原生 GPU、输入、震动或设备配置。

## 依据

R3 返回日志：`C:/uu/releases/diagnostics/tv-adaptation-20261108-r3-candidate/device-return-20261108-025630/ANALYSIS.md`。

日志选择 TWCC-02 / ID6，但五次最终观察统计 accepted 为 0，missing-extension 合计 316717，TWCC submitted 为 0。全局 handler 被 TWCC 附件持有，RBE 无法安装。Client 在 accepted=0 时返回，原两秒超时仅覆盖 sender 创建之后，故本场景永远无法进入故障回退。

## 修改

- `FeedbackAttachment::poll()` 覆盖 sender 创建前的观察准入：实际出现缺少协商扩展的 RTP 后开始两秒等待，仍未收到有效扩展则停止观察，报告新增的 `MissingExtensionTimeout`。既有枚举值不变，新原因追加为 8。
- 等待首个媒体包不计时；等待期间有效扩展出现，则取消缺失扩展计时。创建 sender 时重新开始端点阶段计时，避免沿用准入耗时。
- 新增 `WaitingForExtension` 状态，追加枚举值，不重排旧值。
- Client 在 accepted=0 时也轮询附件，超时先走原有停止/卸载路径，再按原有能力检查尝试安装 RBE。RBE 产生有效估计之前，兼容 REMB 继续保留；不会同时启用两种动态反馈。
- 新诊断：`twcc-send-extension-unobserved-fallback`，随后可见原有附件最终统计、回退及 RBE 安装/发送诊断。
- 正常 V2 的“有效扩展已到、PC 尚未请求反馈”仍是 `WaitingForRequest`，不伪造周期请求，也不因零提交强制切走。

保持当前 V2 广告和共同能力选择规则，不猜未协商的 ID5，不强制切回旧版。此修复解决确定的等待/回退缺口，不声称解释了 PC 为什么未发出匹配扩展。

## 验证

- 反馈管线新增 V1/V2 双版本“协商 ID6、实际包只有 ID5”回归：无媒体不误超时、两秒边界、停止后不继续观察/发送、故障不可重新启用。
- 有效扩展在期限内出现后，取消准入期限；V2 无请求等待到 20 秒仍不生成反馈、不错误回退。
- 真实经典估计器/REMB 测试改为包含协商 ID6、入站媒体不带该扩展的配置，验证此类 RTP 仍能产生估计与反馈。此测试不等于实际 Client/UU PC 的完整接管互通测试。
- 核心回归 29/29，RTC/反馈回归 8/8；Switch 完整编译、链接、打包通过。
- 10 个保护文件与本轮修改前 SHA-256 完全相同；SDK overlay 与现有清单一致。NRO/NACP/ELF 标签、图标、来源清单、固定 RBE 来源锁和许可证资源核对通过。

## 本地候选

`C:/uu/releases/nro/0.3.75-r4tv/UURemote-ns.nro`

SHA-256：`6f092c19535aa9bb3e583aeea3ca7d3e6e00c1029e402038a5056badd9ea1dc3`

大小：42942288 字节。现有 R3/经典对照及此前成功包保留不覆盖。未部署、未上传仓库或 Release；此为使用现有本地认证资源的本地诊断包。

备份、日志、校验脚本与 ELF：`C:/uu/releases/diagnostics/tv-adaptation-20261108-r4-startup/`。

## 真机待验证

沿用同一场景和设置，只更换 NRO。若 PC 仍未提供协商扩展，预期应出现缺失扩展回退、停止统计 reason=8、RBE 安装；只有 RBE 实际获得估计并成功接管后才会清掉兼容固定 REMB。若正常提供扩展，则应走原 TWCC 路径。

检查实际 RTP 码率、动态反馈提交以及操作流畅度。不能仅凭超时/安装事件称码率已恢复，不能凭本机测试证明 UU PC 的接收和 BWE 已工作。解码和原生 GPU 路径没有改动。
