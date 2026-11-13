# REMB 第一阶段修复记录（2026-11-04）

> 范围：`C:/uu/current/YOUYOU-Remote-ns`，保留已有未提交连接诊断工作。  
> 非目标：未启用/实现 transport-cc；未修改 PC、解码播放、版本号、配置语义或 50 Mbps 上限；未部署、提交或推送。

## 1. 修正后的身份与封包规则

- 本地 RTCP 会话 SSRC 每次连接生成一个非零稳定值；同一连接内 RR、SDES、REMB 均使用该值。
- 本地 CNAME 每次连接生成一个稳定、非秘密值（UUID），SDES CNAME 与本地 SSRC 属于同一会话。
- REMB 公共反馈头的 media-source SSRC 固定为 0。
- REMB 的 FCI 目标列表只包含已收到的主视频 RTP SSRC；不从 SR/RR 的 sender SSRC 推断目标。
- 首包未到时 `requestBitrate` 返回 false，不发送、不保存目标，后续 SR 也不会重发；Client 同样不标记本地接受。
- 每个 REMB 请求/重复只发一个 `RR + SDES(CNAME) + REMB` 复合 RTCP 消息，由现有 SRTCP 路径提交；不存在 `RR + REMB` 或裸 REMB 回退。
- SDK 在 SR 后自动重复的路径使用同一复合封包和同一本地身份；无有效目标/身份时只保留既有 RR 路径，不发裸 REMB。
- Client 主动提交与 SR 重复路径共用一个 RR report block 修正函数；两条路径的 RR 发送者、report block 目标和复合封包布局一致。
- 复合 RR 的 DLSR 使用最近一次匹配主视频 SSRC 的 SR 收到时的单调时间计算，单位为 1/65536 秒；延迟主动发送时 DLSR 随实际间隔增长，未收到匹配 SR 时为 0。
- 已有本地身份且已收到主视频 RTP 的会话，即使未启用 REMB，也在 SR 后发送同一本地 SSRC/CNAME 的 `RR + SDES(CNAME)`；report block 仍指向主视频。REMB 只在有目标时追加。
- RR report block 仍指向远端主视频 RTP SSRC；音频会话与既有 NACK/PLI/RTX 路径未改动。

## 2. 修改文件

| 文件 | 修改 |
|---|---|
| `scripts/dependency-overrides/include/rtc/rtcpreceivingsession.hpp` | 新增 `RembReportBlockValues`、`makeCompoundRrSdes`/`makeCompoundRemb`、`dlsrUnits`、`setRtcpIdentity`/`hasRtcpIdentity`、独立的 `mRtpSsrc` 与本地 sender/CNAME 字段、匹配 SR 的单调接收时间；移除裸 `pushREMB` 声明。 |
| `scripts/dependency-overrides/src/rtcpreceivingsession.cpp` | RTP 才记录 REMB 目标；`requestBitrate` 首包/身份门控；SR 路径按是否有 REMB 目标选择 `RR+SDES` 或 `RR+SDES+REMB`；`makeReportBlockValues` 生成匹配 SR 的 DLSR；`pushRR` 保持原样。 |
| `scripts/dependency-overrides/manifest.json` | 新增头文件覆盖项，更新 `rtcpreceivingsession.cpp` 哈希；共 34 个覆盖文件。 |
| `include/remb_feedback.hpp` | 新增 Client 侧 REMB 门控：首包、同目标/同 media SSRC 去重、目标/媒体变化、失败重试、连接重置。 |
| `include/client.hpp` | 接入 `RembFeedbackGate`、本地 RTCP 身份字段；`applyBitrateCap` 接受可选 `NetworkStats` 样本。 |
| `source/client.cpp` | 连接时生成本地 SSRC/CNAME；删除 `commitQuality` 内两处直接 REMB 发送；统一由 `applyBitrateCap` 发送；抽出 `sendCorrected` 让 Client 主动与 SR 重复路径共用 RR report block 修正；首包前不标记成功；增加本地接受/延迟诊断。 |
| `include/stream_stats.hpp`、`source/stream_stats.cpp` | 新增主视频 RTP SSRC、REMB 本地接受计数/目标/发送者/媒体 SSRC 字段。 |
| `source/main.cpp` | `stream-rate`/`encoder-bitrate-status` 明确区分 `feedback`（本地处理器接受）与 `actual-mbps`（PC 实际 RTP 输出），并输出 REMB SSRC/目标。 |
| `source/live_telemetry.cpp` | 仅注释说明既有 `feedback` 字段语义，未改 schema。 |
| `CMakeLists.txt` | 注册 `remb_feedback_test`、`remb_packet_test`（core-only）和 `remb_session_test`（RTC tests）。 |
| `tests/remb_feedback_test.cpp`、`tests/remb_packet_test.cpp`、`tests/remb_session_test.cpp` | 新增定向测试。 |
| `docs/dependency-overrides.md` | 覆盖文件计数更新为 34。 |

`external/libdatachannel` 已应用同一覆盖内容；`patch_deps.py` 只读复核为 `Verified 34 adaptations; applied 0 files.`。

## 3. 实测结果

### 3.1 本机编译与运行（Windows，w64devkit GCC 16.2.0）

- Core-only 配置/构建成功；`remb_feedback_test`、`remb_packet_test` 构建并运行通过。
- `ctest -R "remb_"`：2/2 通过。
- RTC 配置/构建成功；`remb_session_test` 构建并运行通过；`ctest -R remb_session`：1/1 通过。
- 既有 `rtc_loopback`、`rtc_random_thread_state`：2/2 通过。
- 既有非协议测试逐项运行通过：`playout_policy_test`、`media_integrity_test`、`config_store_test`、`input_admission_test`、`rtp_jitter_test`、`latest_snapshot_worker_test`、`diagnostics_log_test`、`present_identity_test`、`background_test`、`rtc_diagnostics_test`、`switch_tls_io_test`。
- 未能运行依赖 `uu_protocol` 的既有测试（`protocol`、`auth`、`bitrate_settings`、`signaling_*`、`signal_pump`）：本机 w64devkit 没有 zlib，而当前未提交工作已让 `uu_protocol` 依赖 ZLIB；这是本机构建环境限制，不是本次 REMB 代码失败。

### 3.2 Switch 交叉编译

`UU_SWITCH=ON` 的 ELF 目标 `UURemote-ns` 构建成功（未打包 NRO、未复制到发布/桌面目录）。最后一次 `sendCorrected` 重构后重新执行了该构建；构建告警均为既有告警。

### 3.3 覆盖可重建性

- `python scripts/patch_deps.py`：`Verified 34 adaptations; applied 0 files.`
- 独立只读校验：manifest 34 项中 34 项与 overlay 和已应用目标哈希一致，0 个异常；两个 REMB 覆盖文件的 `upstream_sha256` 与归档 pristine 文件一致。
- 在本次受限工作区内未对 `external/` 执行“从 pristine 重新写入”的实际演练：该目录的 shell/脚本写入被工作区文件策略拒绝（`write` 工具可写代码文件，但 `patch_deps.py` 的写入路径无法在沙箱内复现）。需在正常构建环境运行一次 `fetch_deps.py` + `patch_deps.py` 作为最终复现确认。

## 4. 风险与边界

1. 仅修正 REMB 的 SSRC/compound/身份；PC 是否真正采用该反馈仍需真机抓包/日志确认。`feedback=1` 只表示本地媒体处理器接受了 SRTCP 提交，不等于 PC 已应用。
2. 未启用 transport-cc；固定 REMB 仍不是接收端 BWE，码率是否上升取决于 PC 的 Incoming REMB/SetBitRate 行为。
3. 有本地身份且已收到主视频 RTP 的会话，未启用 REMB 时也发送 RR+SDES；只有无身份或无主视频 RTP 的会话（例如音频）保留既有裸 RR 路径。裸 RR 的 sender/report block 语义未改，避免影响既有 RR/NACK/PLI/RTX。
4. `mRtpSsrc` 只认首个主视频 RTP SSRC；同连接内远端更换主 SSRC 不会自动重发，重连会建立新会话。该边界与现有 `RtpTrackStats`/`PrimaryVideoRtcpSession` 的主 SSRC 选择一致。
5. 本地 SSRC 为随机值，理论上存在与远端视频 SSRC 相同的极小概率；构造器会拒绝 `sender == media` 的封包，Client 记录延迟并重试，不会发出歧义包。
6. CNAME 为每连接随机 UUID；没有引入用户/设备/会话凭据，也没有输出 CNAME 原文（只输出长度）。
7. 本机未运行依赖 zlib 的协议测试；Switch ELF 编译覆盖了 `uu_protocol` 源码。

## 5. 待实机 A/B

同一 PC、同一 720p60 H.265、高运动窗口、≥60 秒：

| 组 | 客户端 | 记录 |
|---|---|---|
| A | 修复前固定 REMB 50M（基线） | RTP 中位/峰值、loss/nack/rtx |
| B | 修复后复合 REMB 50M | `remb-local-accepted`、`remb-target-bps`、`remb-sender-ssrc`、`remb-media-ssrc`、`stream-rate actual-mbps` |
| C | 修复后 + 后续 transport-cc（不在本阶段） | 只有 B 无效时再评估 |

验收关注：

- 抓包中 REMB sender 为本地 SSRC、media-source 头为 0、FCI 为 PC 视频 SSRC、顺序为 RR→SDES→REMB；
- 首包前无 REMB，首包后只发一次同目标；改码率/重连各只发一次新目标；
- SR 后重复仍为同一复合包；
- 无持续丢包/重传增加，播放/解码无回归；
- 若 B 仍不升码率，记录 PC 侧 Incoming REMB/SetBitRate/实际 RTP，作为是否进入 transport-cc 阶段的依据。

## 6. 审核状态

本次实现与定向测试已完成；等待 Codex 独立审核。审核重点：依赖覆盖哈希/可重建性、复合包字节布局、首包门控、SR 重复路径、Client 单入口与统计语义、以及是否触碰了非目标路径。

## 7. 第二阶段 RTCP 补充（2026-11-04）

本阶段只修改 RTCP 覆盖、清单、定向测试和本交付记录；未修改 Client、音频、RTP 包/组帧、硬解/软解、FFmpeg、GPU、播放或调度路径，也未改画质、TWCC、版本、PC 或构建产物。

- **DLSR**：收到 sender SSRC 与主视频 RTP SSRC 匹配的 SR 时，记录该 SR 的 NTP 与单调接收时间。复合 RR 的 DLSR 按发送时刻与该单调时间的差生成（1/65536 秒），因此延迟主动发送会得到非零 DLSR；无匹配 SR、时钟回退或相等时间为 0，超过 32 位范围时钳位。
- **未启用 REMB 的 RR+SDES**：有本地身份且已收到主视频 RTP 的会话，在 SR 后发送同一本地 SSRC/CNAME 的 `RR + SDES(CNAME)`；report block 仍指向主视频。只有在存在 REMB 目标时才追加 REMB。无身份或无主视频 RTP 时保留原有裸 RR 路径（例如音频）。

第二阶段实测（Windows，w64devkit GCC 16.2.0）：

- `remb_packet_test`：通过；新增 `RR+SDES` 无 REMB 的字节/顺序/身份断言，以及 DLSR 的 0、1 ms、500 ms、1 s、时钟回退和 32 位钳位断言。
- `remb_session_test`：通过；覆盖“先 SR 后延迟 REMB”的身份一致性：SR 触发的 `RR+SDES` 与后续 REMB 的 RR sender、report block target、SDES sender 相同；延迟 REMB 的 DLSR 大于 0 且小于 1 秒；SR 重复仍为单个复合包。
- `ctest -R "remb_"`：2/2 通过。
- `ctest -R "remb_session|rtc_loopback"`：2/2 通过。
- Switch 工具链的 `datachannel-static` 目标编译成功，覆盖 ARM 构建下的 RTCP 覆盖源；未打包 NRO、未部署。
- `python scripts/patch_deps.py`：`Verified 34 adaptations; applied 0 files.`；overlay 与 `external` 对应文件逐字节一致，manifest 哈希已更新。

剩余边界同第 4 节；本阶段仍未做真机 A/B，也未证明 PC 实际采用 REMB。
