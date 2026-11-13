# TWCC observation-only 构建说明（2026-11-04）

> 目的：仅验证 PC 是否响应本地 SDP 中的 TWCC 能力，**不发送 transport-cc feedback**。  
> 默认关闭；不是产品默认行为，也不是“官方 TV 使用 TWCC”的证明。

## 1. 改动范围

本观察模式只涉及两处：

1. `CMakeLists.txt`
   - 新增 `UU_TWCC_ADVERTISE`，默认 `OFF`；
   - 要求同时开启 `UU_TWCC_FOUNDATION` 与 `UU_TWCC_OBSERVE_CLIENT`；
   - 要求 `UU_TWCC_SEND_EXPERIMENT=OFF`，避免观察模式误开反馈发送；
   - 将 `UU_TWCC_ADVERTISE` 写入 NRO manifest 的构建选项。

2. `source/client.cpp`
   - 仅在 `UU_TWCC_ADVERTISE` 编译宏下，在视频 SDP 中增加：
     ```text
     a=rtcp-fb:96 transport-cc
     a=extmap:5 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
     ```
   - 保留既有的 `goog-remb`、`nack`、`nack pli`；
   - **不调用** `FeedbackAttachment::startSending()`；
   - 仍由现有 observation-only handler 记录协商/观测计数。

未修改解码、GPU、依赖覆盖或 REMB 发送路径。

## 2. 默认行为

```text
UU_TWCC_ADVERTISE=OFF
```

默认构建的 SDP、反馈路径和当前行为不变。只有显式开启观察模式才会广告 TWCC 能力。

## 3. 实验构建参数

```text
-DUU_RECEIVE_TIMESTAMPS=ON
-DUU_TWCC_FOUNDATION=ON
-DUU_TWCC_OBSERVE_CLIENT=ON
-DUU_TWCC_ADVERTISE=ON
-DUU_TWCC_SEND_EXPERIMENT=OFF
```

`UU_TWCC_ADVERTISE=ON` 与 `UU_TWCC_SEND_EXPERIMENT=ON` 同时出现时，CMake 配置阶段直接失败。

## 4. 已验证

- 使用上述实验宏对 `source/client.cpp` 做 `g++ -fsyntax-only`，退出码 `0`。
- `git diff --check` 通过。
- 本 worker **未构建 NRO、未部署、未接设备、未运行 APK**。
- 当前环境缺少 Switch shader 工具 `uam`，所以没有生成可运行实验包；实际构建由 Codex 在完整工具链环境完成。

## 5. 这个实验能证明什么

可证明：

- 本地 offer 是否包含 TWCC feedback/extmap；
- PC answer 是否返回同 URI/同 extmap ID；
- `twcc-observation-negotiation-status` 是否 Ready；
- `accepted > 0`、`missingExtension = 0` 是否成立。

不能证明：

- 官方 TV/手机 APK 原本是否使用 TWCC；
- 官方会话是否广告 TWCC extmap；
- PC 是否真正把 TWCC 用于 BWE/`SetBitRate`；
- 实际码率是否提升。

**结论边界：本实验只验证“当前 PC 对 TWCC 的响应能力”，不能反推官方 TV 的链路选择，也不能单独作为移植 TWCC 的依据。**

## 6. 观测日志

构建并运行后检查：

```text
twcc-sdp-advertised-extmap-5
twcc-observation-negotiation-status-*
twcc-observation-accepted-*
twcc-observation-missing-extension-*
twcc-observation-malformed-*
twcc-observation-stale-capabilities
```

判定：

- 本地有 extmap、远端无 → PC 未接受 TWCC；
- 双方有同 URI/ID 且 Ready、accepted>0 → 可以进入下一步发送实验；
- Ready 但 `missingExtension` 高 → 先查 extmap 安装，不要发送。

## 7. 相关文件

- `CMakeLists.txt`
- `source/client.cpp`
- 本文件

## 8. Codex 完整构建验收

2026-11-04 已在全新 `C:/uu/repo/.tools/tmp/twcc-observe-20261104-nx` 完整编译应用和依赖、链接并打包 NRO。通过现有 `C:/uu/pr1-fixes/uam-cached.cmd` 校验着色器源码/缓存二进制哈希，解决 worker 的 UAM 配置阻塞。未复用历史应用对象/静态库。

- 产物：`C:/uu/releases/diagnostics/0.3.75-twcc-observe/UURemote-ns.nro`
- NACP/构建标签：`0.3.75-twcc-obs`，最终 NRO 标题 YOUYOU远程。
- SHA-256：`8c241353da91e48dfab5b77d7eeecd945bc5ad3b9dd3119ed6bee76236ab4abc`
- 大小：42,720,720 字节。
- manifest 验证 ADVERTISE/OBSERVE/FOUNDATION/时间戳 ON，SEND_EXPERIMENT OFF，REMB ON。
- 依赖覆盖核验：34/34 匹配，应用 0 个文件。
- media/native_display/native_nv12/strict_video_receiver/video_assembler/media_dispatch_queue 六文件源码与 A/B B 清单哈希一致。
- 记录、构建日志、清单与测试方法：`C:/uu/releases/diagnostics/0.3.75-twcc-observe/README.md`。

未部署或真机验证；没有把编译结果当成 TWCC 协商/性能证明。仅广告不反馈的实验可能影响 PC BWE，因此以短时验证为目的。
