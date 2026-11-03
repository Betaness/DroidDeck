#!/bin/sh
# Runs fshack_test.c (Proton's fullscreen hack, an application's swapchain recreation loop) against a
# Vortek server on a private 1280x720 Xvfb display, on the device like zc_acquire_test.sh (the server
# and the mock are bionic programs, started from /data/local/vortek):
#   fshack_test.sh <libvortekserver.so> <client-dir> <fshack_test> <zc_mock_compositor>
# <client-dir> is a build-client.sh output; fshack_test is built from fshack_test.c (cc -O2
# -I<tests> -o fshack_test fshack_test.c -lvulkan -lxcb). Scenarios (the same as hybris's
# tests/wsi_test.sh):
#   fshack          zero-copy, zc_mock_compositor checking the compute shader's pixels
#   fshack-copy     no compositor: copied into the window
#   fshack-plain    no fullscreen hack, zero-copy
#   fshack-windowed A.V.A.'s windowed layout (client window in a 1x1 parent), copied
#   recreate-loop   the same mismatched swapchain recreated on every SUBOPTIMAL: stopped after
#                   VORTEK_RECREATE_LIMIT, again after the window changes size
# Exit status 0 when all pass.
set -u
srv=$1 cdir=$2 test=$3 mock=$4
id=fshack-$$
dir=/data/local/vortek
tmp=$(mktemp -d /tmp/$id.XXXX)

disp=90
while [ -e /tmp/.X11-unix/X$disp ] || [ -e /tmp/.X$disp-lock ]; do disp=$((disp + 1)); done
Xvfb :$disp -screen 0 1280x720x24 -nolisten tcp >$tmp/xvfb.log 2>&1 &
xpid=$!
cp "$mock" $dir/$id-mock && chmod 700 $dir/$id-mock
cp "$srv" $dir/$id-srv && chmod 700 $dir/$id-srv
sed "s#\"library_path\": *\"[^\"]*\"#\"library_path\": \"$cdir/libvulkan_vortek.so\"#" "$cdir/vortek_icd.aarch64.json" >$tmp/icd.json
i=0
while [ ! -e /tmp/.X11-unix/X$disp ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done

failed=""
run() { # name, zc (1: with the mock compositor), then environment for the test
    name=$1 zc=$2
    shift 2
    echo "=== $name"
    sock=$tmp/$name-server.sock psock=$tmp/$name-present.sock
    mpid=
    if [ "$zc" = 1 ]; then
        $dir/$id-mock $psock >$tmp/$name-mock.log 2>&1 &
        mpid=$!
        VORTEK_PRESENT_SOCKET=$psock $dir/$id-srv $sock >$tmp/$name-server.log 2>&1 &
    else
        $dir/$id-srv $sock >$tmp/$name-server.log 2>&1 &
    fi
    spid=$!
    i=0
    while { [ ! -S $sock ] || { [ "$zc" = 1 ] && [ ! -S $psock ]; }; } && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
    env "$@" DISPLAY=:$disp VORTEK_SERVER_PATH=$sock VK_ICD_FILENAMES=$tmp/icd.json VK_DRIVER_FILES=$tmp/icd.json \
        timeout 120 "$test" >$tmp/$name.log 2>&1
    rc=$?
    grep -E '^(screen|window [0-9]|B8G8|frame |[0-9]+ frames|FS-)|times within a second|again|zero-copy|failed' $tmp/$name.log
    [ $rc -eq 0 ] || failed="$failed $name"
    sleep 0.3
    kill $spid 2>/dev/null; wait $spid 2>/dev/null
    if [ -n "$mpid" ]; then
        i=0
        while ! grep -q MOCK-RESULT $tmp/$name-mock.log && [ $i -lt 30 ]; do sleep 0.1; i=$((i + 1)); done
        kill $mpid 2>/dev/null; wait $mpid 2>/dev/null
        grep -E '^chain|MOCK' $tmp/$name-mock.log | tail -3
        grep -q "MOCK-RESULT ok" $tmp/$name-mock.log || failed="$failed $name(mock)"
    fi
}

run fshack 1 FS_MODE=fshack
run fshack-copy 0 FS_MODE=fshack
run fshack-plain 1 FS_MODE=plain
run fshack-windowed 0 FS_MODE=plain FS_LAYOUT=offscreen FS_APP=1024x768
run recreate-loop 0 FS_MODE=fixed FS_RESIZE=1 FS_FRAMES=300
grep -q 'created 11 times within a second with the same parameters' $tmp/recreate-loop.log || failed="$failed recreate-loop(not-logged)"
grep -q 'reports VK_SUBOPTIMAL_KHR again' $tmp/recreate-loop.log || failed="$failed recreate-loop(no-again)"

kill $xpid 2>/dev/null; wait $xpid 2>/dev/null
rm -f $dir/$id-mock $dir/$id-srv
rm -rf $tmp
if [ -n "$failed" ]; then
    echo "FAILED:$failed"
    exit 1
fi
echo "ALL PASSED"
