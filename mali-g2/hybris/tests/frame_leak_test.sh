#!/bin/sh
# The Mali's command buffer memory in the game process (mali_fixes.c's periodic RELEASE_RESOURCES):
# runs ../vortek/tests/frame_leak_test.c offscreen through the build-tree ICD (tests/run.sh) with a
# DXVK-like frame (16 render passes and a timestamp query per command buffer) and reports how much
# the process's own heap grows per frame, for vkResetCommandPool (DXVK's) and for
# vkResetCommandBuffer, each with the release off (DROIDDECK_HYBRIS_CMD_RELEASE_EVERY=0, must grow
# above FL_MAX_GROWTH, about 6.7 KB per frame on the Mali-G2) and at the default (must not).
#   sh tests/frame_leak_test.sh          after build.sh; needs libvulkan-dev and libx11-dev
# FL_FRAMES (default 8000), FL_MAX_GROWTH (default 64 bytes per frame, from the second FL report to
# the last). Prints FRAMELEAK-PASS or FRAMELEAK-FAIL.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TESTS=$HERE/../out/tests
mkdir -p "$TESTS"
cc -O2 -o "$TESTS/frame_leak_test" "$HERE/../../vortek/tests/frame_leak_test.c" -lvulkan -lX11 || exit 1
: "${FL_FRAMES:=8000}"
export FL_FRAMES FL_PASSES=16 FL_REPORT=1000 FL_SERVER_PID=self
max=${FL_MAX_GROWTH:-64}
fail=0

run() { # name expect(grow|flat) release-every steps cb-reset
    log=$TESTS/frame_leak_$1.log
    DROIDDECK_HYBRIS_CMD_RELEASE_EVERY=$3 FL_STEPS=$4 FL_CB_RESET=$5 timeout 600 sh "$HERE/run.sh" "$TESTS/frame_leak_test" >"$log" 2>&1
    rc=$?
    verdict=$(awk -v max="$max" -v expect="$2" '/^FL frame .* srvheap /{ f[n] = $3; r[n] = $(NF-2); h[n] = $NF; n++ }
      END {
        if (n < 3) { print "FAIL too few FL frame reports"; exit }
        per = (h[n-1] - h[1]) * 1024 / (f[n-1] - f[1])
        rss = (r[n-1] - r[1]) * 1024 / (f[n-1] - f[1])
        printf "heap %d kB -> %d kB over %d frames = %.0f bytes/frame (rss %.0f bytes/frame): ", h[1], h[n-1], f[n-1] - f[1], per, rss
        if (expect == "grow") print (per > max ? "PASS (grows without the release)" : "FAIL no growth to fix")
        else print (per <= max ? "PASS" : "FAIL growth above " max " bytes/frame")
      }' "$log")
    echo "$1 (release every ${3:-default}): $verdict"
    if [ $rc -ne 0 ]; then grep -E "FAIL|rror" "$log" | head -5; fail=1; fi
    case $verdict in *": PASS"*) ;; *) fail=1 ;; esac
}

run pool-off grow 0 timeline,submit2,draw,query,poolreset 0
run pool-on flat "${DROIDDECK_HYBRIS_CMD_RELEASE_EVERY:-}" timeline,submit2,draw,query,poolreset 0
run cmdbuf-off grow 0 timeline,submit2,draw,query 1
run cmdbuf-on flat "${DROIDDECK_HYBRIS_CMD_RELEASE_EVERY:-}" timeline,submit2,draw,query 1

if [ $fail -ne 0 ]; then echo FRAMELEAK-FAIL; exit 1; fi
echo FRAMELEAK-PASS
