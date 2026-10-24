# 构建 YOUYOU远程 0.3.9

## 环境

需要 CMake 3.21+、Ninja、Python 3.12+、devkitPro Switch 工具链及对应 portlibs。Windows 工具链引导脚本另需 Python 3.14+。不要混用不同编译宏下的历史静态库。

主要 SDK 包：switch-dev、switch-sdl2、switch-sdl2_ttf、switch-curl、switch-ffmpeg、switch-libopus、deko3d，以及可用的 uam 着色器编译器。SDK/依赖来源记录见 scripts/*lock.json；历史 GPU 工具版本记录见 build-v52.md。字体和项目图标随源码提供。默认使用几何图标，不需要 UU APK 提取的界面纹理。

## 本地参考材料

当前签名提取脚本只识别它已验证的 UU TV 3.5.0 参考 APK。仓库不提供 APK、下载镜像、签名文件或内置签名常量。请自行确认有权使用所选材料；提取工具和服务兼容不代表授权。

```sh
export DEVKITPRO=/opt/devkitpro
python3 scripts/fetch_deps.py
python3 scripts/patch_deps.py
python3 scripts/extract_key.py /path/to/your/reference.apk
cmake -S . -B build-nx -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/switch.cmake -DCMAKE_BUILD_TYPE=Release \
  -DUU_BUILD_LABEL=0.3.9 -DUU_MEDIA_DISPATCH=ON -DUU_SRTP_MBEDTLS=ON \
  -DUU_RECOVERY_FOCUS=OFF -DUU_SWITCH_BATCH_RX=OFF \
  -DUU_RECEIVE_TIMESTAMPS=OFF -DUU_TWCC_FOUNDATION=OFF \
  -DUU_TWCC_OBSERVE_CLIENT=OFF -DUU_TWCC_SEND_EXPERIMENT=OFF
cmake --build build-nx --target nro --parallel 4
```

上述实验宏组合对应这次本地候选使用的组合；不等于向其他配置承诺相同效果。若 deko3d/uam 不在默认位置，显式设置 `UU_DEKO3D_ROOT` / `UU_UAM`。不要复制历史对象替代重新编译。

Windows 可在设置 DEVKITPRO 后使用 `scripts/build.ps1 -Apk <本地APK路径>`，默认构建本地 NRO；只有显式 `-PrivatePackage` 才生成含认证资源的本地 ZIP。旧路径不再作为当前构建指南。

## 产物与数据

`build-nx/UURemote-ns.nro` 包含项目资源、两份 Noto 字体/OFL、默认配置及本地认证材料。不是自动完成清理的公开发行包。`scripts/package.py` 要求显式 `--allow-private-signing-material` 才封装此类本地包。

在完整应用模式启动；更新只替换程序，不删除 `SD:/switch/UURemote-ns/` 配置目录。默认配置迁移、UI偏好和会话版本见使用手册。当前公开脚本不生成 NSP，也不提供任天堂内容或打包密钥。

## 依赖复现

fetch_deps.py 校验锁定归档哈希。patch_deps.py 对照 manifest 先校验全部源文件，再应用31份覆盖文件；再次运行应应用0份。覆盖文件保留原许可证，能复现当前开发依赖的源文件，详见 dependency-overrides.md。

本次已验证全新下载依赖→覆盖→重复执行及与本地开发源的内容一致性。原始0.3.9已完成本地Switch编译/链接/打包；不声称在所有主机平台完成了全新工具链构建或全部真机回归。
