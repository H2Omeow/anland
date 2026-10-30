#!/usr/bin/env bash
# Build the container-side pieces of anland's VR support.
#
#   xr/build.sh              driver + Vulkan layer, into xr/build/
#   xr/build.sh --no-layer   driver only (no Vulkan headers on this box)
#   OUT_DIR=/tmp/xr xr/build.sh
#
# The result is what xr/anland-steamvr install copies into SteamVR, so it has to
# be built for the container's architecture (aarch64 on a phone, x86_64 on a
# normal Linux box used for testing). Nothing here is cross-compiled.
set -euo pipefail

here=$(cd -- "$(dirname -- "$0")" && pwd)
out=${OUT_DIR:-$here/build}
cc=${CC:-cc}
cxx=${CXX:-c++}
want_layer=1

for arg in "$@"; do
    case "$arg" in
        --no-layer) want_layer=0 ;;
        -h|--help) sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

mkdir -p "$out"

echo "==> driver (OpenVR server driver, $(uname -m))"
# -fvisibility=hidden is fine: HmdDriverFactory carries an explicit
# default-visibility attribute, everything else in the driver is internal.
"$cxx" -std=c++17 -O2 -fPIC -shared -fvisibility=hidden \
    -Wall -Wextra -Wno-unused-parameter \
    -I"$here/include" \
    -o "$out/driver_anland.so" \
    "$here/driver/driver_anland.cpp"

if [ "$want_layer" -eq 1 ]; then
    echo "==> vulkan layer"
    # vk_layer.h ships with vulkan-headers; without it the layer is simply not
    # built and SteamVR still runs windowed thanks to the driver's
    # IsDisplayRealDisplay()=false - it just loses the belt-and-braces case
    # where vrcompositor probes VK_KHR_display anyway.
    "$cc" -std=gnu11 -O2 -fPIC -shared -fvisibility=hidden \
        -Wall -Wextra \
        -o "$out/libVkLayer_anland_vr.so" \
        "$here/layer/vk_layer_anland_vr.c" -lpthread \
        || { echo "!! layer build failed (install vulkan-headers?)" >&2; exit 1; }
else
    echo "==> skipping vulkan layer (--no-layer)"
fi

echo
ls -l "$out"
