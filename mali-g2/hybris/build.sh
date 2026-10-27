#!/bin/sh
# Builds the in-process Mali Vulkan path for the DroidDeck guest: libhybris (pinned taowen/libhybris,
# patches/), its glibc Vulkan ICD for the Android HAL, and libdroiddeck_hybris_vk.so, the ICD the
# guest loader is pointed at (X11 WSI presenting zero-copy to DroidDeck's compositor, and the Mali
# workarounds the Vortek client carries). Run on an aarch64 Linux host with glibc <= the guest's
# (Ubuntu 24.04 / ubuntu-24.04-arm in CI). Needs git, autoconf, automake, libtool, pkg-config, make,
# gcc, g++, libvulkan-dev, libxcb1-dev and libxcb-shm0-dev. No network after the first fetch.
#
#   ./build.sh [<dest-dir>]      default ./out/guest
#
# <dest-dir> receives a tree to copy over the guest's /:
#   $GUEST_DIR/libdroiddeck_hybris_vk.so, $GUEST_DIR/droiddeck_hybris_icd.aarch64.json,
#   $GUEST_DIR/lib/ (libhybris-common.so.1 - the one to LD_PRELOAD -, libhardware.so.2,
#   libhybris-vulkan-icd.so.0, libhybris/linker/q.so), usr/local/share/licenses/droiddeck-hybris/.
# Real files named by their sonames, no symlinks (the APK's assets cannot hold them).
# GUEST_DIR (default /usr/local/lib/droiddeck-hybris) is compiled into libhybris (linker plugin
# directory, RUNPATH); tests elsewhere set HYBRIS_LINKER_DIR and LD_LIBRARY_PATH (tests/run-*.sh).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$HERE/work}
DEST=${1:-$HERE/out/guest}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 2)}
GUEST_DIR=${GUEST_DIR:-/usr/local/lib/droiddeck-hybris}
CC=${CC:-cc}

LIBHYBRIS_URL=https://github.com/taowen/libhybris.git
LIBHYBRIS_COMMIT=7076971ba6a2fe8834cb0f3aaf4ee721cc1b678d   # arlinux branch, 2026-10-27
HEADERS_URL=https://github.com/Halium/android-headers.git
HEADERS_COMMIT=2c6ac3dcc4f8db593dd69906b0ec22822abfed91     # Android 11 headers

fetch() { # dir url commit
    if [ "$(git -C "$1" rev-parse HEAD 2>/dev/null || true)" != "$3" ]; then
        rm -rf "$1"
        git init -q "$1"
        git -C "$1" fetch -q --depth 1 "$2" "$3"
        git -C "$1" -c advice.detachedHead=false checkout -q FETCH_HEAD
    fi
}

mkdir -p "$WORK"
fetch "$WORK/libhybris" "$LIBHYBRIS_URL" "$LIBHYBRIS_COMMIT"
fetch "$WORK/android-headers" "$HEADERS_URL" "$HEADERS_COMMIT"

# The source tree is patched and configured once per set of patches and options.
SRC=$WORK/libhybris/hybris
STAGE=$WORK/stage
STAMP=$(cat "$HERE"/patches/*.patch "$0" | sha256sum | cut -d' ' -f1)
if [ "$(cat "$WORK/libhybris/.droiddeck-stamp" 2>/dev/null || true)" != "$STAMP" ]; then
    git -C "$WORK/libhybris" checkout -q -f HEAD
    git -C "$WORK/libhybris" clean -q -fdx
    for p in "$HERE"/patches/libhybris-*.patch; do
        git -C "$WORK/libhybris" apply --whitespace=nowarn "$p"
    done
    (cd "$SRC" && NOCONFIGURE=1 ./autogen.sh >/dev/null 2>&1 &&
        ./configure -q --prefix="$GUEST_DIR" --libdir="$GUEST_DIR/lib" \
            --with-android-headers="$WORK/android-headers" \
            --enable-arch=arm64 --enable-mali-quirks --enable-property-cache \
            --with-default-hybris-ld-library-path=/vendor/lib64/egl:/vendor/lib64/hw:/vendor/lib64:/system/lib64:/system_ext/lib64 \
            CPPFLAGS="-idirafter /usr/include" LDFLAGS="-Wl,--no-as-needed")
    echo "$STAMP" > "$WORK/libhybris/.droiddeck-stamp"
fi

rm -rf "$STAGE"
for dir in include common properties hardware; do
    make -C "$SRC/$dir" -j"$JOBS" --no-print-directory -s
    make -C "$SRC/$dir" install DESTDIR="$STAGE" --no-print-directory -s
done
make -C "$SRC/vulkan" -j"$JOBS" --no-print-directory -s libhybris-vulkan-icd.la
make -C "$SRC/vulkan" install-libLTLIBRARIES lib_LTLIBRARIES=libhybris-vulkan-icd.la DESTDIR="$STAGE" --no-print-directory -s

S=$STAGE$GUEST_DIR
D=$DEST$GUEST_DIR
rm -rf "$D" "$DEST/usr/local/share/licenses/droiddeck-hybris"
mkdir -p "$D/lib/libhybris/linker" "$DEST/usr/local/share/licenses/droiddeck-hybris"
for lib in libhybris-common.so.1 libhardware.so.2 libhybris-vulkan-icd.so.0; do
    cp -L "$S/lib/$lib" "$D/lib/$lib"
done
cp -L "$S/lib/libhybris/linker/q.so" "$D/lib/libhybris/linker/q.so"

OBJ=$WORK/obj
rm -rf "$OBJ"
mkdir -p "$OBJ"
CFLAGS="-std=gnu11 -O2 -g -fPIC -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers"
INCLUDES="-I$HERE/src/compat -I$HERE/src -I$HERE/../vortek/src -I$S/include"
for c in "$HERE"/src/*.c "$HERE/../vortek/src/zero_copy.c"; do
    $CC $CFLAGS $INCLUDES -c "$c" -o "$OBJ/$(basename "$c" .c).o"
done
$CC -shared -o "$D/libdroiddeck_hybris_vk.so" "$OBJ"/*.o -Wl,-soname,libdroiddeck_hybris_vk.so \
    -Wl,--no-undefined -Wl,-z,nodelete -L"$S/lib" -lhybris-common -lxcb -lxcb-shm -lpthread -ldl -Wl,-rpath,"$GUEST_DIR/lib"
strip --strip-unneeded "$D/libdroiddeck_hybris_vk.so" "$D"/lib/*.so.* "$D/lib/libhybris/linker/q.so"

cat > "$D/droiddeck_hybris_icd.aarch64.json" <<JSON
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "$GUEST_DIR/libdroiddeck_hybris_vk.so",
        "api_version": "1.4.0"
    }
}
JSON

L=$DEST/usr/local/share/licenses/droiddeck-hybris
install -m644 "$HERE/NOTICE" "$L/NOTICE"
install -m644 "$HERE/../vortek/LICENSE.LGPL-2.1" "$L/LGPL-2.1"
install -m644 "$WORK/libhybris/LICENSE.Apache2" "$L/Apache-2.0"
install -m644 "$WORK/libhybris/LICENSE.BSD-2" "$L/BSD-2-Clause"
echo "libhybris $LIBHYBRIS_COMMIT ($LIBHYBRIS_URL), android-headers $HEADERS_COMMIT" > "$L/SOURCES"
chmod -R a+rX "$DEST"

echo "built $D"
ls -l "$D" "$D/lib"
echo "highest glibc symbol version: $(objdump -T "$D/libdroiddeck_hybris_vk.so" "$D"/lib/*.so.* | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
