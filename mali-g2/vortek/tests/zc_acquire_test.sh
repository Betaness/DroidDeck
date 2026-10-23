#!/bin/sh
# Runs zc_acquire_test against a Vortek server whose zero-copy presenter is zc_mock_compositor, on a
# private Xvfb display. Run on the device (the server and the mock are bionic programs, started from
# /data/local/vortek like the DroidDeck app's copy):
#   zc_acquire_test.sh <libvortekserver.so> <client-dir> <zc_acquire_test> <zc_mock_compositor>
# <client-dir> is a build-client.sh output. The environment passes through: ZC_* to the test,
# MOCK_* to the mock, VORTEK_* to the server. Exit status is the test's, or 1 when the mock does not
# print MOCK-RESULT ok.
# Two checks:
#   performance (default MOCK_RELEASE=early): acquire must return while the previous frame is still
#     rendering (ZCACQ-PASS); the release fences are the next present's acquire fences, which the
#     server's queue orders anyway, so this mode cannot see a missing GPU wait.
#   correctness: MOCK_RELEASE=gpu ZC_EXPECT_FAST=0 (the mock's release jobs, MOCK_GPU_MS, slow the
#     frames down): the mock fails when a present's acquire fence signals before the image's release
#     fence did, i.e. the server let the GPU write an image the compositor had not released yet. Run
#     it with ZC_ACQUIRE_FENCE=0/1/2 and VORTEK_RELEASE_WAIT=import/batch.
set -u
srv=$1 cdir=$2 test=$3 mock=$4
id=zcacq-$$
dir=/data/local/vortek
tmp=$(mktemp -d /tmp/$id.XXXX)
sock=$tmp/server.sock psock=$tmp/present.sock

disp=90
while [ -e /tmp/.X11-unix/X$disp ] || [ -e /tmp/.X$disp-lock ]; do disp=$((disp + 1)); done
Xvfb :$disp -screen 0 1280x720x24 -nolisten tcp >$tmp/xvfb.log 2>&1 &
xpid=$!

cp "$mock" $dir/$id-mock && chmod 700 $dir/$id-mock
$dir/$id-mock $psock >$tmp/mock.log 2>&1 &
mpid=$!
cp "$srv" $dir/$id-srv && chmod 700 $dir/$id-srv
VORTEK_PRESENT_SOCKET=$psock $dir/$id-srv $sock >$tmp/server.log 2>&1 &
spid=$!

i=0
while { [ ! -S $sock ] || [ ! -S $psock ] || [ ! -e /tmp/.X11-unix/X$disp ]; } && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
sed "s#\"library_path\": *\"[^\"]*\"#\"library_path\": \"$cdir/libvulkan_vortek.so\"#" "$cdir/vortek_icd.aarch64.json" >$tmp/icd.json
DISPLAY=:$disp VORTEK_SERVER_PATH=$sock VK_ICD_FILENAMES=$tmp/icd.json VK_DRIVER_FILES=$tmp/icd.json timeout 120 "$test"
rc=$?

sleep 0.3
kill $spid 2>/dev/null; wait $spid 2>/dev/null
i=0
while ! grep -q MOCK-RESULT $tmp/mock.log && [ $i -lt 30 ]; do sleep 0.1; i=$((i + 1)); done
kill $mpid $xpid 2>/dev/null; wait $mpid $xpid 2>/dev/null
echo "--- server"
grep -E "zero-copy|swapchain|acquire|release|VORTEK_MIN" $tmp/server.log
echo "--- mock"
cat $tmp/mock.log
grep -q "MOCK-RESULT ok" $tmp/mock.log || { echo "FAIL mock compositor did not report MOCK-RESULT ok"; [ $rc -ne 0 ] || rc=1; }
rm -f $dir/$id-mock $dir/$id-srv
rm -rf $tmp
exit $rc
