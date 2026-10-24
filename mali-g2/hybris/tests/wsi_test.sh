#!/bin/sh
# Builds the tests and runs the X11 WSI on a private Xvfb display with the device's Mali driver, in
# PalmCode's Ubuntu or any aarch64 glibc shell on the device that can load the vendor driver
# (/system, /vendor and /apex visible). Needs build.sh's output, Xvfb, libxcb1-dev, libvulkan-dev.
#   tests/wsi_test.sh            all scenarios; exit status 0 when all pass
# Scenarios:
#   copy        no compositor: frames are copied into the window (MIT-SHM), checked with GetImage
#   copy-rgba   the same with an RGBA swapchain (R/B swizzle)
#   zero-copy   mock_compositor.c as the compositor: AHardwareBuffer images, acquire and release
#               fences, pixels checked through CPU-readable buffers (DROIDDECK_HYBRIS_AHB_CPU_READ=1)
#   zero-copy-late  the same, the mock releasing without fences
#   zero-copy-afbc  the same, buffers as the session allocates them (pixels not readable)
#   resize      the window shrinks halfway: VK_SUBOPTIMAL_KHR, swapchain recreated
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
OUT=${OUT:-$ROOT/out/guest}
GUEST_DIR=${GUEST_DIR:-/usr/local/lib/droiddeck-hybris}
BIN=$ROOT/out/tests
mkdir -p "$BIN"
STAGE_INC=$ROOT/work/stage$GUEST_DIR/include
cc -O2 -Wall -o "$BIN/swapchain_test" "$HERE/swapchain_test.c" -lvulkan -lxcb || exit 1
cc -O2 -Wall -I"$ROOT/src/compat" -I"$STAGE_INC" -o "$BIN/mock_compositor" "$HERE/mock_compositor.c" "$ROOT/src/ahb.c" \
    "$OUT$GUEST_DIR/lib/libhybris-common.so.1" -lpthread || exit 1

TMP=$(mktemp -d /tmp/hybris-wsi.XXXXXX)
disp=90
while [ -e /tmp/.X11-unix/X$disp ] || [ -e /tmp/.X$disp-lock ]; do disp=$((disp + 1)); done
Xvfb :$disp -screen 0 1280x720x24 -nolisten tcp >"$TMP/xvfb.log" 2>&1 &
xpid=$!
i=0
while [ ! -e /tmp/.X11-unix/X$disp ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
export DISPLAY=:$disp

failed=""
run() { # name, then environment assignments for the test
    name=$1
    shift
    echo "=== $name"
    env "$@" timeout 120 sh "$HERE/run.sh" "$BIN/swapchain_test" >"$TMP/$name.log" 2>&1
    rc=$?
    grep -v -E '^WARNING: linker|normalize_path|library "lib(binder_ndk|nativewindow|sync).so" not found|MEMPROF' "$TMP/$name.log"
    [ $rc -eq 0 ] || failed="$failed $name"
}

run_zc() { # name, MOCK_RELEASE, then environment for the test
    name=$1 release=$2
    shift 2
    sock=$TMP/present-$name.sock
    MOCK_RELEASE=$release sh "$HERE/run.sh" "$BIN/mock_compositor" "$sock" 1 >"$TMP/mock-$name.log" 2>&1 &
    mpid=$!
    i=0
    while [ ! -S "$sock" ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
    run "$name" VORTEK_PRESENT_SOCKET="$sock" "$@"
    i=0
    while kill -0 $mpid 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    kill $mpid 2>/dev/null
    grep '^MOCK' "$TMP/mock-$name.log"
    grep -q 'MOCK-RESULT ok' "$TMP/mock-$name.log" || failed="$failed $name(mock)"
}

run copy SC_EXPECT_COPY=1
run copy-rgba SC_EXPECT_COPY=1 SC_FORMAT=rgba
run_zc zero-copy early DROIDDECK_HYBRIS_AHB_CPU_READ=1 VORTEK_X11_STATS=1
run_zc zero-copy-late late DROIDDECK_HYBRIS_AHB_CPU_READ=1
run_zc zero-copy-afbc early
run resize SC_RESIZE=1 SC_EXPECT_COPY=1

kill $xpid 2>/dev/null
wait $xpid 2>/dev/null
rm -rf "$TMP"
if [ -n "$failed" ]; then
    echo "FAILED:$failed"
    exit 1
fi
echo "ALL PASSED"
