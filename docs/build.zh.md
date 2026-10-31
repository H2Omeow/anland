# 编译 anland

> 中文版（本文件）· English: [build.md](build.md)

本文件里的每一步都在一台 Linux 主机上、针对引入 VR 路径（`xr/`）的那个提交验证过，
并产出了全部四个发布产物。CI 跑的是同样的步骤
（`.github/workflows/build.yml`、`.github/workflows/release.yml`）。

## 需要什么

| 工具 | 验证时用的版本 | 说明 |
| --- | --- | --- |
| Linux x86_64 主机 | Arch（WSL2）6.18 / CI 里的 Ubuntu 24.04 | NDK 这条路不支持 Windows/macOS 主机 |
| JDK | 17（`jdk17-openjdk`，CI 用 Temurin） | `make apk` 通过 wrapper 跑 Gradle 9.6 |
| Android SDK | cmdline-tools + `ndk;29.0.13113456`、`build-tools;36.0.0`、`platforms;android-36` | 给出 `ANDROID_HOME` 后，`make check-tools` 会自动装缺的部件 |
| CMake | >= 3.22 | Arch 上是 4.4，可用 |
| ninja、meson、pkg-config | 较新的任意版本 | 编 wayland-scanner 与 pulseaudio |
| patch、zip、unzip | 任意 | 模块 zip 与仓库里带的补丁集 |
| Vulkan 头文件 | `vulkan-headers` / `libvulkan-dev` | 只给 `xr/build.sh`（driver + layer）用 |
| gcc/g++ | 宿主编译器 | 只给 `xr/build.sh`；守护进程由 NDK 编 |

## Makefile 假定、但裸机上没有的两样东西

有两样东西不在仓库里，也不会自动安装：

**1. `wayland-scanner` 1.24 —— 从仓库锁定的 fork 编出来。** 守护进程需要的协议代码
必须来自它链接的同一个 libwayland 版本：

```sh
meson setup /tmp/wl-scanner third_party/wayland \
  -Dscanner=true -Dlibraries=false -Ddocumentation=false \
  -Dtests=false -Ddtd_validation=false
ninja -C /tmp/wl-scanner
sudo install -m755 /tmp/wl-scanner/src/wayland-scanner /usr/local/bin/
wayland-scanner --version   # wayland-scanner 1.24.0
```

**2. libffi 的 `configure` —— 用 release tarball 覆盖。** git submodule 里没有生成好的
`configure`，而 `autogen.sh` 在现代 libtool 上会失败；v3.4.6 的 release tarball
是同一个版本，并且自带 `configure`：

```sh
curl -fsSL https://github.com/libffi/libffi/releases/download/v3.4.6/libffi-3.4.6.tar.gz \
  | tar xz --strip-components=1 -C third_party/libffi
```

## 编译

```sh
git clone https://github.com/H2Omeow/anland && cd anland
git submodule update --init --recursive

export ANDROID_HOME=$HOME/android-sdk      # 里面要有 cmdline-tools/latest
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk

make libffi     # 一次性：交叉编译 libffi 到 build/libffi-android-out
make native     # build/waylandbridge + build/libawlshm.so   (arm64, NDK)
make apk        # build/anland-wayland.apk + build/anland-awllib.aar
make pulse      # 给 Android 的 PulseAudio -> build/pulse-stage
make module     # build/module/anland-awl.zip  (SukiSU / KernelSU 可刷)
make anlandx    # build/anlandx.tar.gz          (容器内会话)
```

只跑 `make` 等于 native + apk + module + anlandx。第一次 `make apk` 会从 SDK 自己
拉 Gradle、Android Gradle Plugin 和 `cmake;3.22.1`，所以给它几分钟和大约 500 MB
缓存空间。

SteamVR 驱动和 Vulkan 层**不**在这里交叉编译：NDK 的目标是 bionic，而 SteamVR 是
glibc。要在 SteamVR 跑的地方编它们：

```sh
cd build/anlandx/xr        # 或者源码树里的 xr/
./build.sh                 # -> build/driver_anland.so、build/libVkLayer_anland_vr.so
```

`./build.sh` 会先跑一遍 XR 文件格式自测（`xr/tools/xr_wire_test.c`）；
`./build.sh --test` 只跑自测，不编产物。

## 产物

| 文件 | 大小（已验证） | 用途 |
| --- | --- | --- |
| `build/waylandbridge` | 5.6 MB | root 守护进程，打进模块 zip |
| `build/libawlshm.so` | 346 KB | 第三方 App / testapp |
| `build/anland-wayland.apk` | 2.2 MB | `com.anlandnext` 宿主 App |
| `build/anland-awllib.aar` | 56 KB | 给第三方客户端用的 libawl |
| `build/module/anland-awl.zip` | 3.0 MB | magisk/KernelSU/SukiSU 刷入 |
| `build/anlandx.tar.gz` | 3.3 MB | 容器里 `tar xzf`，然后 `bash anlandx/setupanlandx.sh` |

## 走代理

`git`、`curl`、Gradle 和 `sdkmanager` 都认常见的环境变量：

```sh
export http_proxy=http://HOST:7890 https_proxy=http://HOST:7890
# Gradle 自己的 JVM 不认环境变量：
mkdir -p ~/.gradle && cat >> ~/.gradle/gradle.properties <<'EOF'
systemProp.http.proxyHost=HOST
systemProp.http.proxyPort=7890
systemProp.https.proxyHost=HOST
systemProp.https.proxyPort=7890
EOF
# sdkmanager:
JAVA_TOOL_OPTIONS="-Dhttp.proxyHost=HOST -Dhttp.proxyPort=7890 \
  -Dhttps.proxyHost=HOST -Dhttps.proxyPort=7890"
```

在 WSL2 下，Windows 宿主要通过默认网关访问
（`ip route | awk '/^default/ {print $3}'`），而不是 `127.0.0.1`。

## 发布

`.github/workflows/release.yml` 在每次 `v*` tag（或带显式 tag 的
`workflow_dispatch`）时构建同样的四个产物，并把它们连同 `SHA256SUMS.txt`
挂到 GitHub Release 上。它还会为 runner 自身的架构编译一遍 `xr/` 作为编译检查——
那些二进制不会发布，因为容器必须自己编一份。

```sh
git tag v1.0-xr && git push origin v1.0-xr
```

或者：Actions → `release` → *Run workflow* → tag 填 `v1.0-xr`。

## 排障

- `ERROR: sdkmanager missing` —— `ANDROID_HOME` 必须指向包含
  `cmdline-tools/latest/bin/sdkmanager` 的 SDK 根目录，而不是 `cmdline-tools` 本身。
- `third_party/libffi/configure: No such file` —— 用 release tarball 覆盖
  （上面第 2 步）；`make libffi` 会直接停下，而不是去跑坏掉的 autogen。
- `wayland-scanner: command not found` —— cmake 步骤会调用它，所以它必须在 `PATH`
  上，光编出来不够。
- pulse 依赖报 `policy CMP0xxx is not set` —— Makefile 已经传了
  `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`；如果之前用旧 cmake 跑过，需要删一次
  `build/pulse-deps-src`。
- 防火墙后 Gradle 报 `Could not resolve …` —— 代理只配给了守护进程，没配给
  wrapper 自己的下载；第一次跑 `./gradlew` 之前设好 `https_proxy`，并把
  `systemProp.*` 那几行写进 `~/.gradle/gradle.properties` 供依赖解析使用。
