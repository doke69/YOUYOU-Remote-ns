# 构建 YOUYOU远程 0.4beta

## 环境

需要 CMake 3.21+、Ninja、Python 3.12+、devkitPro Switch 工具链及对应 portlibs。Windows 工具链引导脚本另需 Python 3.14+。不要混用不同编译宏下的历史静态库。

主要 SDK 包：switch-dev、switch-sdl2、switch-sdl2_ttf、switch-curl、switch-ffmpeg、switch-libopus、deko3d，以及可用的 uam 着色器编译器。SDK/依赖来源记录见 scripts/*lock.json；历史 GPU 工具版本记录见 [v52 构建归档](archive/v52/build-v52.md)。字体和项目图标随源码提供。默认使用几何图标，不需要 UU APK 提取的界面纹理。

## 本地参考材料

当前签名提取脚本只识别它已验证的 UU TV 3.5.0 参考 APK。仓库不提供 APK、下载镜像、签名文件或内置签名常量。请自行确认有权使用所选材料；提取工具和服务兼容不代表授权。

```sh
export DEVKITPRO=/opt/devkitpro
python3 scripts/fetch_deps.py
python3 scripts/patch_deps.py
python3 scripts/fetch_rbe_sources.py --download
python3 scripts/extract_key.py /path/to/your/reference.apk
cmake -S . -B build-nx -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/switch.cmake -DCMAKE_BUILD_TYPE=Release \
  -DUU_BUILD_LABEL=0.4beta -DUU_MEDIA_DISPATCH=ON -DUU_SRTP_MBEDTLS=ON \
  -DUU_RECOVERY_FOCUS=OFF -DUU_SWITCH_BATCH_RX=ON -DUU_SWITCH_BATCH_REUSE=ON \
  -DUU_RECEIVE_TIMESTAMPS=ON -DUU_TWCC_FOUNDATION=ON \
  -DUU_TWCC_OBSERVE_CLIENT=ON -DUU_TWCC_SEND_EXPERIMENT=ON \
  -DUU_TWCC_ADVERTISE=ON -DUU_TWCC_ADVERTISE_V2=OFF -DUU_TWCC_CLIENT_SEND=ON \
  -DUU_RBE_OBSERVE_CLIENT=ON -DUU_RBE_CLIENT_SEND=ON -DUU_REMB_FEEDBACK=ON \
  -DUU_INPUT_MOTION_COALESCE=ON -DUU_ASYNC_QUALITY_SAVE=ON -DUU_ASYNC_SELECTED_PATH=ON \
  -DUU_CAP_ONLY_ACK_NO_PLI=ON -DUU_SESSION_SIZE_CAP=ON -DUU_SIGNAL_IO_WORKER=ON \
  -DUU_DSCP_FAMILY_CACHE=ON -DUU_UDP_FAMILY_CACHE=ON -DUU_POLL_FIXED_DSCP=ON \
  -DUU_LEAN_STATS=ON -DUU_SWITCH_CORES_012=OFF -DUU_DTLS_RX_BUDGET=OFF \
  -DUU_DTLS_STAGE_TIMING=OFF -DUU_CONTROL_STAGE_TIMING=OFF -DUU_UDP_SEND_TIMING=OFF \
  -DUU_UDP_RX_TRACE=OFF -DUU_UDP_RX_CPU_TIMING=OFF -DUU_PACKET_STAGE_TRACE=OFF \
  -DUU_VIDEO_SUPPLY_TRACE=OFF -DUU_VIDEO_REPAIR_TRACE=OFF -DUU_BSD_SEND_PROBE=
cmake --build build-nx --target nro --parallel 4
```

上述实验宏组合对应这次本地候选使用的组合；不等于向其他配置承诺相同效果。若 deko3d/uam 不在默认位置，显式设置 `UU_DEKO3D_ROOT` / `UU_UAM`。不要复制历史对象替代重新编译。

Windows 可在设置 DEVKITPRO 后使用 `scripts/build.ps1 -Apk <本地APK路径>`，默认构建本地 NRO；只有显式 `-PrivatePackage` 才生成含认证资源的本地 ZIP。旧路径不再作为当前构建指南。

## 产物与数据

`build-nx/UURemote-ns.nro` 包含项目资源、两份 Noto 字体/OFL、默认配置及本地认证材料。不是自动完成清理的公开发行包。`scripts/package.py` 要求显式 `--allow-private-signing-material` 才封装此类本地包。

在完整应用模式启动。安装 0.4beta 时，先备份并清空 `SD:/switch/UURemote-ns/` 全部内容，再复制本版 NRO，首次启动重新登录和配置。安装步骤见使用手册。当前公开脚本不生成 NSP，也不提供任天堂内容或打包密钥。

## 依赖复现

fetch_deps.py 校验锁定归档哈希。patch_deps.py 对照 manifest 先校验全部源文件，再应用35份覆盖文件；再次运行应应用0份。覆盖文件保留原许可证，能复现当前开发依赖的源文件，详见 dependency-overrides.md。

本版整合验证见 [检阅记录](reviews/v0.4beta-integration.md)，新环境构建与整合版真机回归仍需核验。

## 反馈模块与诊断配置

先在源码根目录执行：

```sh
python3 scripts/fetch_rbe_sources.py --download
python3 scripts/fetch_rbe_sources.py --verify-only
```

该脚本只从固定公开commit获取WebRTC/Abseil源码，逐文件核对仓库锁；不会下载APK、读取认证材料或安装全局软件。下载必须显式加`--download`，CMake配置及打包只做离线核验。缓存默认位于`.tools/rbe/218b56e51638`，构建规则和线程API适配在`cmake/webrtc-rbe/`，不再依赖维护者的研究目录。旧的`UU_RBE_SOURCE_DIR`研究目录缓存不可继续使用，请新建build目录或显式改为新的源码bundle。

基于上述Switch命令，RBE诊断需要`UU_RECEIVE_TIMESTAMPS=ON`、`UU_TWCC_FOUNDATION=ON`、`UU_REMB_FEEDBACK=ON`、`UU_RBE_OBSERVE_CLIENT=ON`、`UU_RBE_CLIENT_SEND=ON`。纯经典反馈测试保持TWCC广告/观察/发送OFF；TV优先候选则将`UU_TWCC_ADVERTISE`、`UU_TWCC_ADVERTISE_V2`、`UU_TWCC_OBSERVE_CLIENT`、`UU_TWCC_SEND_EXPERIMENT`、`UU_TWCC_CLIENT_SEND`一起设ON。两类包都保留原媒体/GPU参数，使用独立build目录和清楚的短版本标签。

新manifest记录RBE开关、锁SHA、固定commit、3016文件核验及随包许可。`scripts/build.ps1` 显式使用上方 0.4beta 参数组合，单独运行 CMake 时必须核对开关，不能把默认 OFF 配置当作同一候选。诊断构建不能按指定码率跑满与否判断成败；TV精确版本、动态cap和媒体RTT边界见阶段报告。
