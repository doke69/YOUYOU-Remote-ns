# 来源、引用与致谢

## 直接上游与实现依赖

- [Antman2023/UURemote-ns](https://github.com/Antman2023/UURemote-ns)：本项目直接上游。保留GPL许可和作者/贡献者署名；本分支扩展原生界面、输入、硬件显示、配置与传输处理。
- 上游记录的 UURemote-web：历史请求签名、连接参数和信令实现参考；此处保留来源说明，不虚构额外授权或未核实的仓库链接。
- [libnx](https://github.com/switchbrew/libnx)、[deko3d](https://github.com/devkitPro/deko3d)、[dekotools](https://github.com/devkitPro/dekotools)：Switch homebrew接口、显示和着色器工具。
- [FFmpeg](https://ffmpeg.org/) 与 [devkitPro portlibs](https://github.com/devkitPro/pacman-packages)：编解码和Horizon/NVTEGRA适配来源。
- SDL2、SDL_ttf、FreeType、HarfBuzz：界面、文本和音频等基础能力。
- libdatachannel、libjuice、Mbed TLS、usrsctp、libSRTP：网络、加密及数据/媒体通道。修改过的依赖源保留原版权与许可，并附带哈希清单。
- nlohmann/json、Nayuki QR Code generator、Opus、curl及相关依赖：各自许可见 THIRD_PARTY.md 和对应源码。
- [Noto CJK](https://github.com/notofonts/noto-cjk)：Noto Sans SC Regular/Bold，原样分发，附OFL-1.1。

## 研究与文档参考，不等于代码依赖

- [VallierDev/uu-remote-linux](https://github.com/VallierDev/uu-remote-linux)：Linux社区互通实现与非官方身份/项目说明参考。本次未复制它的签名常量，也不声称取得其或网易的认证授权。
- [GaryOAO/UUWay](https://github.com/GaryOAO/UUWay)：官方客户端与兼容层的分发边界参考。本项目不使用Wine方案。
- Moonlight Switch、averne 的 SwitchWave、Envideo、nv-video-info、deko3d/gpu_playground 等：视频、GPU和低延迟处理的研究参考。没有把阅读过的仓库全部列为已链接依赖，也不声称它们提供本项目整体实现或背书。
- [网易UU远程集成说明](https://github.com/netease-uuyc/uuremote-integration-guide)：公开跨App协议参考；其iOS/Android拉起官方应用的方式不等于Switch原生客户端授权。

感谢相关作者、维护者、社区测试者及为Lite/标准机提供反馈的用户。项目图标由用户选定，未将官方UU品牌标志作为本项目Logo。署名与致谢不改变第三方许可，也不替代使用授权。
