# R5：当前源码上的 TWCC-01 兼容候选

日期：2026-11-08。版本：`0.3.75-r5t1`。

## 依据和范围

R4 已证实缺失扩展回退生效，经典 RBE 实际提交反馈，但没有恢复此前成功候选的 TWCC-01 路径。详见：

- `C:/uu/releases/diagnostics/tv-adaptation-20261108-r4-startup/device-return-current/ANALYSIS.md`
- `C:/uu/releases/diagnostics/tv-adaptation-20261105-s3-ssrc/device-results-20261105-030319/ANALYSIS.md`

本轮不撤回旧源码，不改协议解析、算法或媒体实现。在 R4 当前源码上只关闭 `UU_TWCC_ADVERTISE_V2`，下一次 offer 继续提供 TWCC-01 / ID5，不再提供 TWCC-02 / ID6。仍需最终双方共同协商后才能使用，不根据本地配置猜远端扩展。

## 构建差异

与交付 R4 manifest 对照，`source_files` 全部相同，`build_options` 仅以下两项变化：

| 参数 | R4 | R5 |
| --- | --- | --- |
| UU_BUILD_LABEL | 0.3.75-r4tv | 0.3.75-r5t1 |
| UU_TWCC_ADVERTISE_V2 | ON | OFF |

TWCC 基础/广告/观察/发送、接收时间戳、RBE 观察/发送、媒体派发、SRTP 后端和 FULL 资源等均保持 R4 配置。R4 的缺失扩展启动期限、停止卸载、RBE 接管与正常 V2 请求等待实现都保留。V2 代码仍在源码中，本候选仅不广告它。

## 校验

- Switch 完整程序编译、链接、打包通过。
- NRO/NACP/ELF 版本、图标、段范围、来源清单和 RBE 固定来源锁核对通过。
- 10 个编解码/播放/原生 GPU 保护文件 SHA-256 未变，依赖覆盖仍匹配现有清单。
- 沿用 R4 相同源码已通过的核心 29/29、RTC/反馈 8/8 结果；本轮未重复运行这些测试，不把旧结果称作新一轮测试。
- 尚未部署或真机验证，不宣称码率已经恢复。

## 本地测试包

`C:/uu/releases/nro/0.3.75-r5t1/UURemote-ns.nro`

SHA-256：`53b3f0279450bbcf6e16dca41d47f756c17a8003cd1563b325bd71af5cd60bc7`

大小：42942288 字节。旧包保留不覆盖。构建日志、ELF、参数对照与校验结果：`C:/uu/releases/diagnostics/tv-adaptation-20261108-r5-twcc01/`。此包仅作本地诊断，不上传公开仓库或 Release。

## 一次对照步骤

正常退出程序，保留原 NRO 副本，手动替换为本包。启动确认版本 `0.3.75-r5t1`，保持同一台 PC、同一场景、原有画质/编码/分辨率/频率/缓冲设置，不改 config.ini。无需重新登录或删除配对。

主要看新会话是否选择 `twcc-observation-version-1-extension-id-5`，随后出现 `twcc-send-started-remb-suppressed` 和持续增长的提交数。最终 ID 以共同协商为准。

如果仍没有有效扩展，R4 回退机制应继续工作；不能仅凭候选关闭02就称01已互通。返回日志时按 app-start 标记划分版本，轮转文件头不一定代表其全部内容。最终比较实际 RTP 码率、画质与操作感，反馈提交仅证明本地提交，不等于 PC 已消费。
