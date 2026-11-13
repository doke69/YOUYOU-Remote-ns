# S4-B：真实接收估计器独立构建与回放

日期：2026-11-08。前置：[S4-A](./tv-adaptation-s4-rbe-foundation-2026-11-08.md)。本阶段只在独立研究目录编译，未向产品加入依赖或调用，未修改编解码/播放/原生GPU。

## 1. 来源与边界

独立源码：`C:/uu/study/youyou-rbe-subset-20261108`。

固定 WebRTC commit `218b56e516386cd57c7513197528c3124bcd7ef3`；Abseil 来自该版 DEPS 指向的 Chromium third_party `fc733299410a7104a0848539baab0131b8a616b8`。这是 API/行为接近 TV 的候选参考，**不是已经识别出的 TV 精确版本**。LICENSE/PATENTS/AUTHORS 和 Abseil LICENSE 保留，完整下载树3016文件有SHA锁；下载目录不等于编译范围。

参与编译的是 AST/TOF 两种真实估计器、AIMD/InterArrival/Overuse/LinkCapacityEstimator/RembThrottler 及其基础依赖，未引入 WebRTC 解码/渲染引擎。没有另写固定增长/下降的带宽控制律。

## 2. 已完成验证

| 项目 | 结果与含义 |
|---|---|
| host 回放 | CTest 1/1通过，两估计器各3轮，默认重复结果一致 |
| 输入限速 | 发送8Mbps不变，接收从8Mbps减到2.4Mbps，两者默认原始估计12,019,600→2,040,000bps |
| Field Trial | 独立回放官方 `WebRTC-BweBackOffFactor/Enabled-0.65/`，结果1,560,000bps，证明参数路径有效；不表示TV使用该配置 |
| 回绕/流生命周期 | AST64秒/RTP32位回绕、RemoveStream零估计和新实例隔离通过；不等于真实Client重连已验收 |
| 输出节流 | 原版200ms门、明显下降提前输出通过 |
| Switch | 独立静态库及调用两估计器的完整验证ELF链接成功，未打NRO、未执行 |

这些数字是估计器原始输出，不能解释为实际RTP吞吐/PC编码码率/链路容量；原始观察样本数也不是RTCP发送次数。所有算法回放使用模拟Clock，不能证明真机性能与操作感改善。

该版本 Field Trial 接口为进程全局；测试逐次运行并恢复空配置，不能当作可并发修改的会话参数接口。接收observer cap来源仍未闭合，未擅自把field8接到此处。

## 3. 兼容工作与实际失败

算法源不改。构建强制包含标准cstdint，适配GCC15与旧Abseil的传递include问题；平台线程ID/ref通过窄适配接Win32/libnx，线程命名为空操作，解决原Windows源使用MSVC SEH的MinGW编译错误，不改变调度/核心/优先级。Field Trial registry使用固定上游生成器；补入真实field_trial_units.cc修正TimeDelta链接缺项。初始失败日志保留，未用猜测实现填洞。

早期单文件并发取源发生HTTP429后停止，后续采用固定版本目录归档和串行取源；没有绕过限额或安装全局工具。`fetch_sources.py`只是早期include闭包辅助，完整当前依赖以source-tree-lock和CMake为准。

## 4. 归档与产品一致性

归档：`C:/uu/releases/diagnostics/tv-adaptation-20261108-rbe-subset`。

- `verification.json`：结果、构建产物SHA、产品源码对比、保护检查。
- `source-tree-lock.json`、`upstream-lock.json`：固定来源和文件哈希。
- `README.md`：精确构建命令、兼容边界、许可。
- `host/`、`switch/`：各自独立且同配置的静态库依赖、生成头、构建图及验证程序，不能混装。
- `test-host-verified.log`、`build-nx-link.log`：最终验证；早期失败日志另外保留。

10个编解码/播放/GPU保护文件哈希全部一致，`git diff --check`通过。本轮没有写产品C++/CMake，但归档时发现 `source/client.cpp` 比上一阶段哈希发生变化（修改时间01:22:03），其余4个阶段源对照一致。已记录当前hash，未覆盖/回退该变化，不能声称整个产品源码与上阶段完全相同；本次独立库结果不包含这部分新改动的验证。

## 5. 下一步

S4-C先核对当前Client差异，再实现认证后RTP元数据输入适配：真实arrival、payload+padding、SSRC、90kHz timestamp、协商AST/TOF及连续缺失切换、Process/移除/重连。随后S4-D才接throttler/SRTCP，并与TWCC和兼容REMB互斥。当前没有发送新的REMB，没有Client集成、NRO、部署或真机收益证据，S4整体未完成。
