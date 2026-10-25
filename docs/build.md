# 构建 YOUYOU远程 0.3.44

本页对应仓库当前公开源码快照。应用版本以根目录 `CMakeLists.txt` 的 `project(... VERSION ...)` 为准；常规构建标签由构建脚本从该版本读取。内部试验版才显式覆盖 `UU_BUILD_LABEL`，并应使用不同构建目录，避免把旧缓存版本带入产物。

## 环境与依赖

需要 CMake 3.21+、Ninja、Python 3.12+、devkitPro Switch 工具链及对应 portlibs。Windows 工具链引导脚本另需 Python 3.14+。主要依赖包括 switch-dev、SDL2/SDL2_ttf、curl、FFmpeg、Opus、Deko3D 和 uam 着色器编译器。来源、版本和归档哈希见 `scripts/*lock.json`；依赖覆盖细节见[依赖源覆盖说明](dependency-overrides.md)。不同宏或SDK生成的历史静态库不可混用。

## 本地构建

在 Linux/macOS shell 中：

```sh
export DEVKITPRO=/opt/devkitpro
python3 scripts/fetch_deps.py
python3 scripts/patch_deps.py
python3 scripts/extract_key.py /path/to/your/reference.apk
cmake -S . -B build-nx -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/switch.cmake -DCMAKE_BUILD_TYPE=Release \
  -DUU_MEDIA_DISPATCH=ON -DUU_SRTP_MBEDTLS=ON \
  -DUU_RECOVERY_FOCUS=OFF -DUU_SWITCH_BATCH_RX=OFF \
  -DUU_RECEIVE_TIMESTAMPS=OFF -DUU_TWCC_FOUNDATION=OFF \
  -DUU_TWCC_OBSERVE_CLIENT=OFF -DUU_TWCC_SEND_EXPERIMENT=OFF
cmake --build build-nx --target nro --parallel 4
```

Windows 可设置 `DEVKITPRO` 后执行：

```powershell
.\scripts\build.ps1 -Apk <本地参考APK路径>
```

脚本会从 `CMakeLists.txt` 读取0.3.44版本并写入应用元数据，拉取并校验锁定依赖、应用覆盖，再构建本地NRO。若本地已合法准备好所需认证资源，可省略 `-Apk`；是否有权使用参考材料由构建者自行确认。脚本默认不生成发布ZIP。`-PrivatePackage` 会生成包含本地认证材料的私用包，不得当作可公开分发的脱敏产物。

如 Deko3D/uam 不在默认位置，可通过 `UU_DEKO3D_ROOT` / `UU_UAM` 指定。不要从其他版本拷贝对象文件或静态库代替完整重编译。以上宏组合是0.3.44源码快照的构建配置，不代表这些实验选项已获得所有设备的性能验证。

## 版本与产物核对

发布前确认以下版本一致：

1. `CMakeLists.txt` 的项目版本。
2. README、CHANGELOG、项目状态、使用手册、构建指南和对应发布说明。
3. 构建 NACP 与应用界面显示的版本。
4. Git 标签和发布说明标题。

正常构建不要手动设置不同的 `UU_BUILD_LABEL`。本地候选若覆盖标签，应明确标记为候选且不得冒充正式快照。NRO/ELF/NSP默认不进入源码仓库；公开二进制前，必须另行审查内嵌认证材料、许可、对应源码和分发授权。

## 签名材料与安装

仓库不提供参考APK、签名文件或打包密钥。构建NRO会嵌入由构建者本地提供的认证材料；这不是公开发行包，也不代表取得分发许可。`scripts/package.py` 对私用包要求显式 `--allow-private-signing-material`。

在 Switch 完整应用模式运行。更新时退出旧程序，只替换 `SD:/switch/UURemote-ns/UURemote-ns.nro`，保留同目录配置与会话。当前仓库不提供 NSP 构建流程，也不提供任天堂内容或打包密钥。

## 验证边界

0.3.44发布前的锁定依赖和覆盖源核对记录见[依赖覆盖说明](dependency-overrides.md)及发布说明。这个记录不代表本次文档维护重新下载依赖、重跑完整工具链构建或重做真机测试。源码构建成功也不代表所有主机、电脑端和网络组合均已验证。
