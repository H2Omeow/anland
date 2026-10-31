#!/usr/bin/env bash
# Build the container-side pieces of anland's VR support.
#
#   xr/build.sh              driver + Vulkan layer + wire self test
#   xr/build.sh --no-layer   driver only (no Vulkan headers on this box)
#   xr/build.sh --test       host-arch self test only (no artifacts kept)
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
test_only=0

for arg in "$@"; do
    case "$arg" in
        --no-layer) want_layer=0 ;;
        --test) test_only=1 ;;
        -h|--help) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

mkdir -p "$out"

# The wire format is the only part of the XR path that does not need hardware:
# it is text (anland-xr-display) plus two seqlock blocks. Checking it here means
# a change to anland_xr_wire.h fails on a laptop instead of on a headset.
echo "==> wire self test ($(uname -m))"
"$cc" -std=gnu11 -O1 -Wall -Wextra \
    -I"$here/include" \
    -o "$out/xr_wire_test" \
    "$here/tools/xr_wire_test.c"
"$out/xr_wire_test"

if [ "$test_only" -eq 1 ]; then
    echo
    echo "==> --test: skipping driver + layer"
    exit 0
fi

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
