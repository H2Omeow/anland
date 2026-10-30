# Building anland

Everything in this file was verified on a Linux host against the commit that
introduced the VR path (`xr/`), producing all four release artifacts. CI runs
the same steps (`.github/workflows/build.yml`, `.github/workflows/release.yml`).

## What you need

| Tool | Version used | Notes |
| --- | --- | --- |
| Linux x86_64 host | Arch (WSL2) 6.18 / Ubuntu 24.04 in CI | Windows/macOS hosts are not supported by the NDK path |
| JDK | 17 (`jdk17-openjdk`, Temurin in CI) | `make apk` runs Gradle 9.6 through the wrapper |
| Android SDK | cmdline-tools + `ndk;29.0.13113456`, `build-tools;36.0.0`, `platforms;android-36` | `make check-tools` installs whatever is missing, given `ANDROID_HOME` |
| CMake | >= 3.22 | Arch ships 4.4, that works |
| ninja, meson, pkg-config | any recent | wayland-scanner build + pulseaudio |
| patch, zip, unzip | any | module zip and the vendored patch sets |
| Vulkan headers | `vulkan-headers` / `libvulkan-dev` | only for `xr/build.sh` (driver + layer) |
| gcc/g++ | host compiler | `xr/build.sh` only; the daemon is NDK-compiled |

## Host setup that the Makefile assumes but a bare machine lacks

Two things are not in the repo and are not auto-installed:

**1. `wayland-scanner` 1.24 — build it from the pinned fork.** The protocol
code the daemon needs has to come from the same libwayland revision it links:

```sh
meson setup /tmp/wl-scanner third_party/wayland \
  -Dscanner=true -Dlibraries=false -Ddocumentation=false \
  -Dtests=false -Ddtd_validation=false
ninja -C /tmp/wl-scanner
sudo install -m755 /tmp/wl-scanner/src/wayland-scanner /usr/local/bin/
wayland-scanner --version   # wayland-scanner 1.24.0
```

**2. libffi `configure` — overlay the release tarball.** The git submodule
ships no generated `configure`, and `autogen.sh` fails on modern libtool; the
v3.4.6 release tarball is the same pin and does ship one:

```sh
curl -fsSL https://github.com/libffi/libffi/releases/download/v3.4.6/libffi-3.4.6.tar.gz \
  | tar xz --strip-components=1 -C third_party/libffi
```

## Build

```sh
git clone https://github.com/H2Omeow/anland && cd anland
git submodule update --init --recursive

export ANDROID_HOME=$HOME/android-sdk      # contains cmdline-tools/latest
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk

make libffi     # one-time: cross-compile libffi into build/libffi-android-out
make native     # build/waylandbridge + build/libawlshm.so   (arm64, NDK)
make apk        # build/anland-wayland.apk + build/anland-awllib.aar
make pulse      # PulseAudio for Android -> build/pulse-stage
make module     # build/module/anland-awl.zip  (SukiSU / KernelSU flashable)
make anlandx    # build/anlandx.tar.gz          (in-container session)
```

`make` alone runs native + apk + module + anlandx. The first `make apk` pulls
Gradle, the Android Gradle Plugin and `cmake;3.22.1` from the SDK itself, so
allow a few minutes and ~500 MB of caches.

The SteamVR driver and the Vulkan layer are **not** cross-compiled here: the
NDK targets bionic, SteamVR is glibc. Build them where SteamVR runs:

```sh
cd build/anlandx/xr        # or xr/ in the checkout
./build.sh                 # -> build/driver_anland.so, build/libVkLayer_anland_vr.so
```

## Artifacts

| File | Size (verified) | Goes to |
| --- | --- | --- |
| `build/waylandbridge` | 5.6 MB | root daemon, inside the module zip |
| `build/libawlshm.so` | 346 KB | third-party apps / testapp |
| `build/anland-wayland.apk` | 2.2 MB | `com.anlandnext` host app |
| `build/anland-awllib.aar` | 56 KB | libawl for third-party clients |
| `build/module/anland-awl.zip` | 3.0 MB | magisk/KernelSU/SukiSU flash |
| `build/anlandx.tar.gz` | 3.3 MB | `tar xzf` inside the container, then `bash anlandx/setupanlandx.sh` |

## Behind a proxy

`git`, `curl`, Gradle and `sdkmanager` all honour the usual variables:

```sh
export http_proxy=http://HOST:7890 https_proxy=http://HOST:7890
# Gradle ignores the environment for its own JVM:
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

On WSL2 the Windows host is reachable through the default gateway
(`ip route | awk '/^default/ {print $3}'`), not through `127.0.0.1`.

## Releases

`.github/workflows/release.yml` builds the same four artifacts on every `v*`
tag (or `workflow_dispatch` with an explicit tag) and attaches them plus
`SHA256SUMS.txt` to a GitHub Release. It also compiles `xr/` for the runner's
architecture as a compile check — those binaries are not shipped, because the
container has to build its own copy.

```sh
git tag v1.0-xr && git push origin v1.0-xr
```

Or: Actions → `release` → *Run workflow* → tag `v1.0-xr`.

## Troubleshooting

- `ERROR: sdkmanager missing` — `ANDROID_HOME` must point at the SDK root that
  contains `cmdline-tools/latest/bin/sdkmanager`, not at `cmdline-tools`.
- `third_party/libffi/configure: No such file` — overlay the release tarball
  (step 2 above); `make libffi` stops instead of running a broken autogen.
- `wayland-scanner: command not found` — the cmake step shells out to it; it
  has to be on `PATH`, not just built.
- `policy CMP0xxx is not set` from the pulse deps — the Makefile already passes
  `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`; a stale `build/pulse-deps-src` from an
  older cmake run has to be deleted once.
- Gradle `Could not resolve …` behind a firewall — the proxy is configured for
  the daemon but not for the wrapper's own download; set `https_proxy` before
  the first `./gradlew` run, and put the `systemProp.*` lines in
  `~/.gradle/gradle.properties` for dependency resolution.
