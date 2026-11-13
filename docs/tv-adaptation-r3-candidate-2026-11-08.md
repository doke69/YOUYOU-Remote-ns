# R3：构建复现、离线回归与本地测试NRO

日期：2026-11-08。承接R2输出链。此阶段只调整构建/来源/许可/产物清单，既有126项运行时与测试源文件均保持R2字节内容；没有修改编解码、播放、GPU、输入或震动实现。

## 1. 现在测试哪个包

**先测主候选：0.3.75-r3tv。**

```text
C:/uu/releases/nro/0.3.75-r3tv/UURemote-ns.nro
SHA256: 496e374b931c232b673a4351a88539795f80b536626668779197e36c6ee25653
大小: 42942288 bytes
```

它优先使用实际共同协商的TWCC；仅在明确回退且有有效经典估计时才由RBE接管。02若只是正常等待请求，不因零提交而切走。它不是“所有会话必定同时发RBE与TWCC”的包。

另存一个隔离经典链的对照候选，**本轮无需两包同时反复测试**：

```text
C:/uu/releases/nro/0.3.75-r3rbe/UURemote-ns.nro
SHA256: fb83a2e72c74478629e7e7d20757c046cfec4da26a3d2e7b4551d33e4ce0b045
大小: 42901328 bytes
```

对照包关闭TWCC广告/观察/发送，保留foundation组件供RBE预算和认证endpoint使用，专门检查经典RBE路径。不能将它的策略差异当作随机A/B变量。

两份均为本地诊断构建，不是公开发布包。保留既有本地认证资源，不生成分发ZIP，不上传仓库或Release。

## 2. 手动更新与一次测试

1. 在电脑保留旧NRO副本，正常退出Switch程序。
2. 只用主候选替换 `SD:/switch/UURemote-ns/UURemote-ns.nro`；不复制默认INI，不删除/覆盖配对、配置或UI状态。
3. 从完整应用模式启动，确认版本显示`0.3.75-r3tv`。
4. 保持原CPU频率、画质/编码/分辨率/FPS、vsync、播放缓冲和网络，在同一运动场景串流约1分钟。
5. 留意画质、操作感、长停顿以及震动/声音恢复等功能。正常断开并退出后保留本次connection.log及有轮转时的connection.log.1/.2，不要拿旧会话混合判断。

当前未自动部署、未启动采集、未要求连接MTP；用户自行复制。日志可能包含地址/设备信息，本地分析前后不公开上传原始账号/会话文件。

## 3. 已通过的离线验证

| 项目 | 结果 |
|---|---|
| 核心回归 | 29/29：配置、画质请求、认证/协议、震动、输入、信令、异步日志/资源等 |
| RTC/反馈回归 | 8/8：随机线程状态、真实本机loopback、REMB session、能力准入、估计器/观察器/RBE输出及TWCC管线 |
| 独立来源复现 | 从固定公开commit重新下载，3016项文件SHA全部匹配；未依赖既有seed才能通过 |
| 来源故障测试 | 缺失/修改源码拒绝；所有参与编译的上游文件及生成器/许可被锁覆盖 |
| 依赖覆盖 | 34份overlay及实际SDK对应文件规范化SHA一致，未重跑patch覆盖现有依赖 |
| 媒体保护 | 10项保护哈希一致；126项既有运行时/测试源与R2阶段一致 |
| NRO检查 | NRO0/ASET、分段范围、AArch64 ELF、图标SHA、16个UTF-8标题、NACP版本/运行时标签/manifest一致 |
| 资源与许可 | FULL图标/字体/着色器校验；4项RBE许可随ROMFS打包并记录SHA；不包含旧brand.rgba |

本机loopback和模拟RTP不等于UU PC真机互通；以上不证明帧率/延迟/码率或全部页面功能已在设备验收。

## 4. 构建独立性变化

- 正式最小子集构建规则在`cmake/webrtc-rbe/`，不再从`C:/uu/study/...`加载CMake代码。
- `upstream.lock.json`固定WebRTC `218b56e516386cd57c7513197528c3124bcd7ef3` 与 Chromium third_party `fc733299410a7104a0848539baab0131b8a616b8` 的Abseil；下载树3016项，实际只编译显式sources.cmake子集。
- `scripts/fetch_rbe_sources.py`默认离线核验，只有显式`--download`才联网；可从已验证bundle `--seed`。CMake及打包均核验源锁，不使用未校验缓存直接宣布复现成功。
- 默认源缓存 `.tools/rbe/218b56e51638` 不进源码仓；锁、构建规则、窄线程API适配、获取工具和许可随源码保存。
- 正常`build.ps1`显式关闭所有TWCC/RBE实验门，避免旧缓存遗留。脚本语法已检查，但本轮没有运行其fetch/patch入口去覆盖SDK。
- manifest新增RBE开关、固定commit、锁SHA及许可资源，并覆盖新构建规则/获取工具的源码SHA；从不读取/哈希独立signing.key或用户配对文件。

平台适配与原算法保持R2/S4-B内容，未借依赖迁移换线程数、绑核、算法参数或媒体路径。

## 5. 发现并拦截的构建错误

首轮PowerShell命令的版本参数未加引号，被拆为`-DUU_BUILD_LABEL=0`及另一个token，生成了一份标签0的内部产物。包检查发现manifest/cache标签不符后拦截；该包没有进入交付目录。

随后改为显式字符串 `'-DUU_BUILD_LABEL=0.3.75-r3tv'` 重建，并核对NACP、ELF运行时字符串与manifest三处一致。错误前manifest保留在归档`label-command-before-fix.manifest.json`。对照包同样使用带引号版本参数并独立校验。

## 6. 未解决与验收界限

仍未识别TV精确WebRTC commit、Field Trial及动态接收cap来源；当前经典候选沿用参考无限observer cap及200ms默认媒体RTT。确认`peer_->rtt()`来自SCTP，未伪装成媒体RTCP RTT。field8仍仅为PC请求上限，未承诺指定码率持续跑满。

模式目前保守互斥，不覆盖官方可能的所有混合包/并行反馈组合；实验反馈预算不是完整传输所有RTCP的精确预算。01已有早期真机证据，02及本轮RBE候选仍需R4真实会话验收。

源码准备和本地交付已经达到可测试状态；R4仍未完成。尤其真实连接/重连、PC接受反馈、预设/自定义画质、操作感、震动/触控/隐私/声音恢复和长时间稳定性，须结合实际设备与用户反馈判定，不能用37项离线测试替代。

## 7. 证据位置

- 主包及其manifest、SHA和许可：`C:/uu/releases/nro/0.3.75-r3tv/`
- 对照包：`C:/uu/releases/nro/0.3.75-r3rbe/`
- 总归档：`C:/uu/releases/diagnostics/tv-adaptation-20261108-r3-candidate/`
- 核心构建：`C:/uu/repo/.tools/tmp/rbe-r3-core-codex-20261108`
- RTC构建：`C:/uu/repo/.tools/tmp/rbe-r3-rtc-codex-20261108`
- Switch构建：`C:/uu/repo/.tools/tmp/rbe-r3-nx-codex-20261108`，最后缓存为经典对照；交付的主包已独立保存，不受缓存后续模式影响。
- 干净下载验证：`C:/uu/repo/.tools/tmp/rbe-r3-download-proof-20261108`

各候选的ELF/NACP/cache和verification.json按版本分别归档。总verification.json记录最终保护、源码/产物、测试及无部署事实；使用交付manifest中的hash识别具体包，不只比较版本号。
