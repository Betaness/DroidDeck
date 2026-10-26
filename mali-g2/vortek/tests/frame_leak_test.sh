#!/bin/sh
# Runs frame_leak_test against a Vortek server (zero-copy presenter: zc_mock_compositor, on a private
# Xvfb) and reports how much the server's malloc heap grows per frame. Run on the device, like
# zc_acquire_test.sh:
#   frame_leak_test.sh <libvortekserver.so> <client-dir> <frame_leak_test> <zc_mock_compositor>
# <client-dir> is a build-client.sh output. FL_* pass through to the test (default here: a DXVK-like
# frame with 16 render passes and a timestamp query, FL_STEPS=timeline,submit2,draw,query,present,poolreset
# FL_PASSES=16 FL_FRAMES=12000), VORTEK_* to the server. FL_MAX_GROWTH (default 64) is the largest
# accepted growth in bytes per frame, measured from the second FL frame report to the last.
# Prints "server heap: A kB -> B kB over N frames = X bytes/frame" and FRAMELEAK-PASS or FRAMELEAK-FAIL.
# Without the server's periodic RELEASE_RESOURCES (VORTEK_CMD_RELEASE_EVERY=0) the default frame
# grows the server by about 6 KB per frame on the Mali-G2.
set -u
srv=$1 cdir=$2 test=$3 mock=$4
id=framelk-$$
dir=/data/local/vortek
tmp=$(mktemp -d /tmp/$id.XXXX)
sock=$tmp/server.sock psock=$tmp/present.sock
: "${FL_STEPS:=timeline,submit2,draw,query,present,poolreset}" "${FL_PASSES:=16}" "${FL_FRAMES:=12000}" "${FL_REPORT:=1000}"
export FL_STEPS FL_PASSES FL_FRAMES FL_REPORT

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
DISPLAY=:$disp VORTEK_SERVER_PATH=$sock VK_ICD_FILENAMES=$tmp/icd.json VK_DRIVER_FILES=$tmp/icd.json FL_SERVER_PID=$spid \
  timeout 600 "$test" >$tmp/client.log 2>&1
rc=$?

kill $spid 2>/dev/null; wait $spid 2>/dev/null
kill $mpid $xpid 2>/dev/null; wait $mpid $xpid 2>/dev/null
grep -E "^FL |FAIL" $tmp/client.log
grep -E "VORTEK_CMD_RELEASE_EVERY|abort|Scudo" $tmp/server.log
max=${FL_MAX_GROWTH:-64}
verdict=$(awk -v max="$max" '/^FL frame .* srvheap /{ f[n] = $3; h[n] = $NF; n++ }
  END {
    if (n < 3) { print "FAIL too few FL frame reports"; exit }
    per = (h[n-1] - h[1]) * 1024 / (f[n-1] - f[1])
    printf "server heap: %d kB -> %d kB over %d frames = %.0f bytes/frame\n", h[1], h[n-1], f[n-1] - f[1], per
    print (per <= max ? "PASS" : "FAIL growth above " max " bytes/frame")
  }' $tmp/client.log)
echo "$verdict"
rm -f $dir/$id-mock $dir/$id-srv
rm -rf $tmp
if [ $rc -ne 0 ] || ! echo "$verdict" | grep -q '^PASS'; then echo FRAMELEAK-FAIL; exit 1; fi
echo FRAMELEAK-PASS
