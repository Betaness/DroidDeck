#!/bin/sh
# Build the glibc aarch64 Vortek client ICD (libvulkan_vortek.so) and its ICD
# manifest for the DroidDeck guest. Run on an aarch64 Linux host (the CI uses
# ubuntu-24.04-arm) after ./fetch.sh. Needs: cmake, make, a C compiler, libx11-dev,
# libxcb1-dev, libxcb-shm0-dev and libvulkan-dev (for vk_icd.h / vulkan_xlib.h /
# vulkan_xcb.h; the other Vulkan headers are vortekrenderer's 1.4.302 copy).
#
#   ./build-client.sh <dest-dir>     default ./out/client
#
# <dest-dir> receives libvulkan_vortek.so and vortek_icd.aarch64.json, whose
# library_path is the guest path GUEST_DIR (default /usr/local/lib/droiddeck-vortek).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$HERE/work}
DEST=${1:-$HERE/out/client}
JOBS=${JOBS:-2}
SYSVK=${SYSVK:-/usr/include/vulkan}
GUEST_DIR=${GUEST_DIR:-/usr/local/lib/droiddeck-vortek}
BUILD=${BUILD:-$HERE/out/build-client}

INC=$BUILD/vkinc
rm -rf "$BUILD"
mkdir -p "$INC/vulkan" "$DEST"
cp "$WORK"/winlator-app/app/src/main/cpp/vortekrenderer/include/vulkan/*.h "$INC/vulkan/"
cp -r "$WORK/winlator-app/app/src/main/cpp/vortekrenderer/include/vk_video" "$INC/"
cp "$SYSVK/vk_icd.h" "$SYSVK/vulkan_xlib.h" "$SYSVK/vulkan_xcb.h" "$INC/vulkan/"

cmake -S "$WORK/vortek" -B "$BUILD/cmake" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="-I$INC -include $HERE/src/droiddeck_defaults.h" \
  -DVORTEK_STANDALONE_SOURCES="$HERE/src/client_x11_present.c" \
  -DVORTEK_STANDALONE_INCLUDE="$HERE/src"
make -C "$BUILD/cmake" -j"$JOBS" --no-print-directory
install -m644 "$BUILD/cmake/libvulkan_vortek.so" "$DEST/libvulkan_vortek.so"
strip --strip-unneeded "$DEST/libvulkan_vortek.so"

cat > "$DEST/vortek_icd.aarch64.json" <<JSON
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "$GUEST_DIR/libvulkan_vortek.so",
        "api_version": "1.3.128"
    }
}
JSON
echo "built $DEST/libvulkan_vortek.so"
readelf -d "$DEST/libvulkan_vortek.so" | grep NEEDED || true
echo "highest glibc symbol version: $(objdump -T "$DEST/libvulkan_vortek.so" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
