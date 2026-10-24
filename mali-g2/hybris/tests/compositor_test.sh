#!/bin/sh
# Several processes presenting zero-copy to the compositor at once (the Vortek server and in-process
# games in a session): compositor_harness.c runs the compositor's own vortek_present.c, two
# swapchain_test windows side by side present to it together, a third comes and goes meanwhile.
# Each must get its own buffers back (a release routed to the wrong presenter stalls acquire for 2 s).
# Run after build.sh and tests/wsi_test.sh (which builds swapchain_test), on a host as for wsi_test.sh.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
OUT=${OUT:-$ROOT/out/guest}
GUEST_DIR=${GUEST_DIR:-/usr/local/lib/droiddeck-hybris}
COMP=$(cd "$ROOT/../../app/src/main/cpp/waylandcomp/src" && pwd)
BIN=$ROOT/out/tests
mkdir -p "$BIN"
cc -O2 -Wall -Wno-unused-parameter -D_GNU_SOURCE -include stdio.h -I"$ROOT/src/compat" -I"$COMP" -I"$ROOT/work/stage$GUEST_DIR/include" \
    -o "$BIN/compositor_harness" "$HERE/compositor_harness.c" "$COMP/vortek_present.c" "$ROOT/src/ahb.c" \
    "$OUT$GUEST_DIR/lib/libhybris-common.so.1" -lwayland-server -lpthread || exit 1
[ -x "$BIN/swapchain_test" ] || { echo "run tests/wsi_test.sh first"; exit 1; }

TMP=$(mktemp -d /tmp/hybris-comp.XXXXXX)
disp=90
while [ -e /tmp/.X11-unix/X$disp ] || [ -e /tmp/.X$disp-lock ]; do disp=$((disp + 1)); done
Xvfb :$disp -screen 0 1280x720x24 -nolisten tcp >"$TMP/xvfb.log" 2>&1 &
xpid=$!
i=0
while [ ! -e /tmp/.X11-unix/X$disp ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
export DISPLAY=:$disp

XDG_RUNTIME_DIR=$TMP sh "$HERE/run.sh" "$BIN/compositor_harness" 3 >"$TMP/harness.log" 2>&1 &
hpid=$!
i=0
while [ ! -S "$TMP/vortek-present" ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
export VORTEK_PRESENT_SOCKET=$TMP/vortek-present VORTEK_X11_STATS=2
SC_FRAMES=600 SC_POS=0,0 sh "$HERE/run.sh" "$BIN/swapchain_test" >"$TMP/a.log" 2>&1 &
apid=$!
SC_FRAMES=600 SC_POS=640,0 sh "$HERE/run.sh" "$BIN/swapchain_test" >"$TMP/b.log" 2>&1 &
bpid=$!
sleep 2
SC_FRAMES=60 SC_POS=0,370 SC_SIZE=320x200 sh "$HERE/run.sh" "$BIN/swapchain_test" >"$TMP/c.log" 2>&1
crc=$?
wait $apid; arc=$?
wait $bpid; brc=$?
i=0
while kill -0 $hpid 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
kill $hpid $xpid 2>/dev/null
rc=0
for t in a b c; do
    echo "--- presenter $t"
    grep -E 'frames:|SC-|hybris-x11: window.*now|no image released|acquire waited' "$TMP/$t.log"
    grep -q 'no image released' "$TMP/$t.log" && rc=1
done
[ $arc -eq 0 ] && [ $brc -eq 0 ] && [ $crc -eq 0 ] || rc=1
echo "--- compositor"
grep -E 'HARNESS|presenter [0-9]+ (dis)?connected|refused' "$TMP/harness.log"
grep -q 'HARNESS-RESULT ok' "$TMP/harness.log" || rc=1
rm -rf "$TMP"
[ $rc -eq 0 ] && echo "COMPOSITOR TEST PASSED" || echo "COMPOSITOR TEST FAILED"
exit $rc
