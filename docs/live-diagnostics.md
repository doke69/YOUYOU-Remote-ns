# v0.1.58 高码率实时诊断（schema 12）

> 这是**诊断-only**能力，用来区分“高码率下的接收/组帧/队列/解码/显示哪一段造成卡顿”，不修改变码率、播放缓冲、帧调度、接收期限、NACK/RTX 或任何协议行为。  
> 本版本构建已通过静态编译检查，但尚未在 Switch 真机上验证；Log 中所有数值都应按“观测证据”而非结论使用。

## 1. 数据路径与边界

Switch 每秒构造一个逻辑 sample，拆成 **4 个独立 UDP JSON datagram**（每组硬上限 **1200 bytes**）：

| kind | 内容 |
|---|---|
| `core` | 版本、流状态、分辨率/请求帧率、画质/码率目标、VSync、缓冲、pacing、诊断计数、路径与端点摘要 |
| `network` | RTT、jitter、初传缺口、RTP 累计计数、接收队列/IDR/PLI/NACK/RTX、严格接收器处理计数 |
| `media` | 解码/发布/CPU 转移/GPU 发布、解码耗时、队列、AU 大小/拒绝、队列等待、解码超阈值计数、最后解码年龄 |
| `render` | 显示/渲染频率、上传/work/present/loop 时间、长循环、帧间隔 p95/max、展示计数、GPU 映射、UI/stats 标志 |

四组共享同一 `schema=12, pair, session, seq, epoch, ticks_ms`；`parts=4`，`kind` 固定枚举。逻辑 sample 的 `seq` 相同，接收端按 `session+epoch+seq` 合并。

- 单组超过 1200 bytes：发送端只跳过该超限组，其他有效组继续发送；不终止进程、不改协议。接收端会记录不完整 sample。
- 不序列化 Snapshot、媒体 payload、SDP、凭据、完整配置或原始数据包。
- `core` 可包含受 opt-in 配置约束的 `path/local/remote` 摘要（每组最多 96 bytes，UTF-8 边界截断）；其他组不含端点。
- 发送端只用非阻塞 UDP `sendto`，不接收命令、不监听；不每帧写文件/网络。

### 静态尺寸边界

发送端在任何数值长度下执行硬检查；按字段集合和长数字的静态估算，`network` 组最坏约 1181 bytes，其他组更小。实际运行值不会超过 1200；该数字是静态估算，不是运行测试结果。

## 2. 发送端配置（现有 telemetry.ini）

Switch 上的发送配置仍为 `sdmc:/switch/UURemote-ns/telemetry.ini`。保持与已有接收规则兼容：

```ini
enabled=1
pair=<48位小写十六进制共享值>
host=<PC 接收端私网 IPv4>
port=26791
```

- `enabled` 必须为 `1`；`pair` 必须为 48 位小写 hex。
- `host` 必须是私网 IPv4（10/8、172.16/12、192.168/16）；不发送到公网。
- 现有部署的接收规则为 PC 端口 26791，接受两台 Switch 的私网源地址；具体地址只保存在本地配置，不写入文档、日志或仓库。
- 不打印 pair、端点或原始包内容；诊断计数只写入数值日志/发送组。

## 3. PC 接收端

脚本：`scripts/telemetry_receiver.py`（仅 Python 标准库，无第三方包）。

```bash
python scripts/telemetry_receiver.py --config private.json --out diagnostics-out
```

`private.json` **只在本地使用，不要提交**：

```json
{
  "pair": "<与 Switch 相同的48位小写hex>",
  "host": "<预期 Switch 私网 IPv4>",
  "bind": "<PC 本机私网 IPv4>",
  "port": 26791
}
```

字段含义：

- `host`：**预期的 Switch 源 IP**，用于校验 UDP 来源；不是 PC 地址。
- `bind`：PC 本机要绑定的地址，必须是显式私网 IPv4 或 `127.0.0.1`；不接受 `0.0.0.0` 的隐式全网绑定。
- `port`：PC 监听端口，现有规则为 26791。

安全性：

- 接收端不回复任何包，不实现 HTTP/dashboard。
- 每个 part 都校验 schema/keys/type、固定 `version=0.1.58`、私网源地址；pair 使用 `hmac.compare_digest` 比较。
- pair 不进入 `stdout`、`latest.json`、`telemetry.jsonl`、`health.json` 或 partial 记录。
- 掉包/重复/顺序错误只影响该 sample；不执行任何 Switch 命令。

### 输出

- `telemetry.jsonl`：每个完整 sample 一行（已移除 pair）；按 20 MB 上限滚动，保留最多 3 个历史文件。
- `latest.json`：最近完整 sample（原子替换，Windows 偶发权限忙时小范围重试）。
- `health.json`：每秒写出（即使空闲/超时）：`pid`、`bound`、`port`、`state`、`waiting`、`receiving`、`last_age_s`、`complete`、`partial`、`rejected`、`expired`、`log_errors`、`receiver_sequence_gaps`。
- 过期/被淘汰的 partial 以 `complete:false`、`parts_received` 和 `missing_kinds` 写入 jsonl，不把缺失字段填成 0；不写 pair，也不更新 `latest.json` 为“完整”样本。若 core 缺失，使用任意一个已通过校验的 part 作为公共信封，缺失字段保持缺失。
- 日志追加遇到 OSError 时只计入 `health.json.log_errors`，接收器继续收集；不会静默退出。
- partial pending 最多 64 组，约 5 秒过期；session/epoch 跟踪按 `(session,epoch)` 计数并限制为 64 项，避免长时间重连无界增长。
- `receiver_sequence_gaps` 是接收端 sample 序号缺口，**不是视频丢包**；视频初传缺口在 `network.initial_gap_pct`。

## 4. 字段与解读

### 4.1 core

`version,width,height,requested_fps,codecflag,actualmbps,targetcap,feedback,vsync,buffer,pacing,snapshot_age_ms,session_age_ms,sent,dropdiag,path,local,remote,send_us,recv_us,publish_us`

- `codecflag`：1 = HEVC，0 = H.264。
- `actualmbps`：视频实际 Mbps；`targetcap`：设置上限，0 表示自动。
- `buffer`：当前播放缓冲 ms；`pacing`：播放调度是否开启。
- `sent/dropdiag`：发送端累计成功/失败 datagram 计数（不是视频丢包）。
- `send_us/recv_us/publish_us`：解码阶段 EMA，单位 µs；不要当作 GPU 计算时间。
- `snapshot_age_ms`：统计快照到本次发布的时间；`session_age_ms`：当前诊断 sample 相对会话开始。

### 4.2 network

`rtt_ms,jitter_ms,initial_gap_pct,packets,lost,accepted,rx_drop,wait_idr,idr_ff,wait_recent,wait_max,pending,block_ms,repair_ms,nack,rtx,pli,nack_retry,nack_repeat,kf_requests,rx_buf,rx_buf_high,rx_msgs,rx_batches,rx_max_batch,rx_in_calls,rx_tick_calls,rx_in_us,rx_in_max_us,rx_tick_us,rx_tick_max_us,rx_mem_drop,rx_frame_max`

- `rtt_ms` 是同一路径往返时延，**不是单向端到端延迟**；未知用负值。
- `initial_gap_pct` 是 RTP 初传序列缺口，发生在修复/RTX 之前，不等于最终媒体丢帧。
- `rx_*` 是严格接收器线程的**被动 wall-time 处理**计数：
  - `rx_in_us/rx_tick_us` 包含当前函数持锁、当前回调/组帧处理的时间；不是 GPU/CPU 计算时间。
  - `rx_in_calls/rx_tick_calls`、`rx_msgs/rx_batches/rx_max_batch`、`rx_buf/rx_buf_high` 用于区分“高码率下包处理/组帧拥塞”与“AU 解码队列拥塞”。
  - 这些值在现有 mutex 下更新；没有新的逐包日志或 JSON 写入。
- `rx_mem_drop/rx_frame_max`：严格接收器 `memory_bound` 分支的累计 retire 数和尝试帧最大 bytes。2 MB 单帧防护在 `Media::video` 之前 retire，因此 `media.au_over` 单独不能显示该原因；若 `rx_mem_drop`/`rx_frame_max` 上升需优先看这里。
- `repair_ms` 是 NACK 修复观测，不能等同于整体网络 RTT。
- `rx_drop` 是接收器丢弃，`lost` 是 RTP 序列缺口；两者不要混为同一指标。

### 4.3 media

`decoded,published,cpu_xfer,gpu_pub,dec_fps,dec_ema,dec_err,drops,q_in,ready,skip_due,skip_bound,late,hw,au_max,au_bytes,au_count,au_over,q_high_frames,q_high_bytes,q_wait_last,q_wait_max,q_wait_oldest,dec_max,dec_gt8,dec_gt16,dec_gt33,last_dec_age`

- `au_max/au_bytes/au_count`：已接受代际的访问单元大小累计/最大/数量；`au_over` 是超过 2 MB 被拒绝的累计数（2 MB 防护保持不变）。
- `q_wait_last/max/oldest`：出队时测量 `dequeue_steady - enqueue_steady` 的 ms；**不含解码处理**，也不含等待队列 mutex 前的时间；`oldest` 是当前队首等待。
- `dec_max/dec_gt8/16/33`：实际解码调用 wall-time 的会话最大值和 >8/16/33 ms 累计次数；`dec_ema` 仍是既有 EMA。
- `last_dec_age`：最后成功解码帧到快照的年龄；-1 表示尚无。
- `q_in/ready` 是瞬时队列；`q_high_*` 是会话高水位。
- 这些是诊断计数，不改变队列上限/丢帧策略。

### 4.4 render

`display_fps,render_hz,upload_avg_ms,upload_max_ms,work_avg_ms,work_max_ms,present_avg_ms,present_max_ms,loop_avg_ms,loop_max_ms,loop_over25,loop_over50,gap_p95_ms,gap_max_ms,gap_overflow,frame_age_ms,present_total,native_display,gpu_present,map_hit,map_new,map_live,active_ui,stats_flag,refresh_ms,stable_swaps`

- `upload/work/present/loop` 是 CPU 侧 elapsed 时间，单位 ms；`present` 包含 VSync 等待，**不是 GPU compute**；`work` 包含 UI/输入/上传准备等，不是纯 GPU。
- `gap_p95/max` 是实际新帧提交间隔的窗口 p95/max；直方图 0..511 ms 为精确桶，索引 512 的桶表示 >=512 ms，`gap_overflow` 表示 >=512 ms 的样本计数；p95 为 512 时按溢出桶解读。
- `loop_over25/50` 是单次循环 >25/50 ms 的累计；`frame_age` 是最后提交新帧年龄。
- `native_display/gpu_present/map_*` 是显示路径/映射缓存计数；没有 GPU fence 等待新增。
- `active_ui/stats_flag` 表示当前 sample 窗口内是否处于本地 UI/统计面板。

## 5. 高码率排查方法（最小轮次）

1. 同一场景、相近运动内容分别采一次低码率与高码率；只比较同一会话/路径，不要用不同游戏/场景横比。
2. 先看 `network.rx_buf/rx_msgs/rx_batches/rx_in_us/rx_tick_us` 是否随码率上升明显；若是，优先怀疑**包/组帧处理**。
3. 再看 `media.q_wait_* / au_* / dec_*` 是否上升；若是，区分 AU 过大/解码排队还是网络。
4. 最后看 `render.upload/work/present/loop/gap_*`；`present` 高不等于 GPU 慢，需结合 `active_ui` 与 VSync。
5. 若 `initial_gap_pct` 正常但 `rx_*`/`q_wait` 上升，不能归因于网络丢包；若 `rtt` 正常但媒体/渲染升高，也不能归因于“端到端 RTT”。
6. 同一配置只做少量重复采样，达到能区分阶段即可；不要无限收集日志。

## 6. 限制

- schema 12 与旧 schema 11 接收器不兼容；旧接收器不会解析四组语义，需使用本脚本或自行实现同 schema。
- 尚未在目标 Switch/高码率会话上验证；上述字段和阈值是诊断定义，不是已证明的根因。
- 本版本不改码率自适应、队列、pacing、接收期限、NACK/RTX 或任何协议行为；不能把“已采集”当作“已修复”。
- pair 是共享值比较，不是对每个包做独立 HMAC；私网环境仍应限制源地址/端口，不要把接收端暴露到公网。
