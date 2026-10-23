#!/bin/sh
# Launches leak_test RUNS times against one Vortek server and records the server's memory after each
# client is gone (VmRSS, RssAnon, RssFile, RssShmem, VmSwap from /proc/<pid>/status, open fds,
# threads). Run on the device, like zc_acquire_test.sh:
#   leak_test.sh <libvortekserver.so> <client-dir> <leak_test> [runs]
# <client-dir> is a build-client.sh output. Environment:
#   LEAK_MODES   modes cycled through the runs (default "exit kill": exit() without freeing, SIGKILL)
#   LEAK_XVFB    1 (default when Xvfb exists): run the clients on a private Xvfb so they create a swapchain
#   LEAK_WAIT    seconds to wait for the server to log the closed connection and to release the
#                devices that were still busy then in the background (default 15)
#   LEAK_MOCK    path of zc_mock_compositor: the server presents zero-copy to it (VORTEK_PRESENT_SOCKET),
#                so the clients leave zero-copy chains behind; its MOCK-RESULT is checked at the end
#   LEAK_*       pass through to leak_test; VORTEK_* to the server
# Prints a table and the server's cleanup lines; exits 1 when a client failed or a connection was
# not closed in time. The numbers must stay flat from the second run on.
set -u
srv=$1 cdir=$2 test=$3 runs=${4:-20}
modes=${LEAK_MODES:-exit kill}
wait_s=${LEAK_WAIT:-15}
id=leak-$$
dir=/data/local/vortek
tmp=$(mktemp -d /tmp/$id.XXXX)
sock=$tmp/server.sock
rc=0

xpid=
if [ "${LEAK_XVFB:-1}" = 1 ] && command -v Xvfb >/dev/null; then
  disp=90
  while [ -e /tmp/.X11-unix/X$disp ] || [ -e /tmp/.X$disp-lock ]; do disp=$((disp + 1)); done
  Xvfb :$disp -screen 0 1280x720x24 -nolisten tcp >$tmp/xvfb.log 2>&1 &
  xpid=$!
  export DISPLAY=:$disp
else
  unset DISPLAY
fi

mpid= psock=
if [ -n "${LEAK_MOCK:-}" ]; then
  psock=$tmp/present.sock
  cp "$LEAK_MOCK" $dir/$id-mock && chmod 700 $dir/$id-mock
  $dir/$id-mock $psock >$tmp/mock.log 2>&1 &
  mpid=$!
fi

cp "$srv" $dir/$id-srv && chmod 700 $dir/$id-srv
if [ -n "$psock" ]; then VORTEK_PRESENT_SOCKET=$psock $dir/$id-srv $sock >$tmp/server.log 2>&1 &
else $dir/$id-srv $sock >$tmp/server.log 2>&1 &
fi
spid=$!
i=0
while { [ ! -S $sock ] || { [ -n "$xpid" ] && [ ! -e /tmp/.X11-unix/X$disp ]; } || { [ -n "$psock" ] && [ ! -S $psock ]; }; } && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
sed "s#\"library_path\": *\"[^\"]*\"#\"library_path\": \"$cdir/libvulkan_vortek.so\"#" "$cdir/vortek_icd.aarch64.json" >$tmp/icd.json
export VORTEK_SERVER_PATH=$sock VK_ICD_FILENAMES=$tmp/icd.json VK_DRIVER_FILES=$tmp/icd.json

field() { awk -v k="$1:" '$1 == k { print $2 }' /proc/$spid/status; }
# dma-bufs the server holds open: "<count>/<MiB>" from the fdinfo of every fd with an exporter name
dmabufs() {
  cat /proc/$spid/fdinfo/* 2>/dev/null | awk '$1 == "size:" { size = $2 } $1 == "exp_name:" { n++; total += size }
    END { printf "%d/%.0f", n, total / 1048576 }'
}
sample() {
  printf '%-5s %-5s %9s %9s %9s %9s %9s %5s %4s %9s\n' "$1" "$2" "$(field VmRSS)" "$(field RssAnon)" "$(field RssFile)" \
    "$(field RssShmem)" "$(field VmSwap)" "$(ls /proc/$spid/fd | wc -l)" "$(field Threads)" "$(dmabufs)"
}

printf '%-5s %-5s %9s %9s %9s %9s %9s %5s %4s %9s\n' run mode "VmRSS kB" "RssAnon" "RssFile" "RssShmem" "VmSwap" fds thr "dmabuf/MiB"
sample start -
set -- $modes
nmodes=$#
run=1
while [ $run -le "$runs" ]; do
  k=$(( (run - 1) % nmodes + 1 ))
  eval mode=\${$k}
  log=$tmp/client-$run.log
  if [ "$mode" = kill ]; then
    "$test" kill >$log 2>&1 &
    cpid=$!
    i=0
    while ! grep -q -e LEAK-READY -e FAIL $log && [ $i -lt 600 ]; do sleep 0.1; i=$((i + 1)); done
    kill -9 $cpid 2>/dev/null
    wait $cpid 2>/dev/null
  else
    timeout 120 "$test" "$mode" >$log 2>&1
  fi
  grep -q -e LEAK-READY -e LEAK-CLEAN-PASS $log || { echo "run $run ($mode): client failed:"; cat $log; rc=1; }
  i=0
  while [ "$(grep -c 'connection closed' $tmp/server.log)" -lt $run ] && [ $i -lt $((wait_s * 10)) ]; do sleep 0.1; i=$((i + 1)); done
  if [ "$(grep -c 'connection closed' $tmp/server.log)" -lt $run ]; then
    echo "run $run ($mode): server did not close the connection within ${wait_s}s"
    rc=1
  fi
  # devices still busy at disconnect are released in the background when they drain
  while [ "$(grep -c 'left to the background cleanup' $tmp/server.log)" -gt "$(grep -c 'busy devices drained' $tmp/server.log)" ] &&
    [ $i -lt $((wait_s * 10)) ]; do sleep 0.1; i=$((i + 1)); done
  if [ "$(grep -c 'left to the background cleanup' $tmp/server.log)" -gt "$(grep -c 'busy devices drained' $tmp/server.log)" ]; then
    echo "run $run ($mode): busy devices not released within ${wait_s}s"
    rc=1
  fi
  sleep 0.5
  sample $run $mode
  run=$((run + 1))
done

echo "--- server (cleanup and errors)"
grep -E "released|cleanup|drained|connection closed|swapchain destroyed|taken off|zero-copy|untracked|busy|error|FAIL|abort|signal" $tmp/server.log |
  sed 's/fd=[0-9]*/fd=N/; s/swapchain [0-9]* taken/swapchain N taken/; s/device 0x[0-9a-f]*/device P/; s/after [0-9.]* s/after X s/' | sort | uniq -c | sort -rn | head -20
kill $spid 2>/dev/null; wait $spid 2>/dev/null
if [ -n "$mpid" ]; then
  i=0
  while ! grep -q MOCK-RESULT $tmp/mock.log && [ $i -lt 30 ]; do sleep 0.1; i=$((i + 1)); done
  kill $mpid 2>/dev/null; wait $mpid 2>/dev/null
  echo "--- mock"
  grep -E "^MOCK|chain" $tmp/mock.log | sed 's/chain [0-9]*/chain N/' | sort | uniq -c | sort -rn | head -10
  grep -q "MOCK-RESULT ok" $tmp/mock.log || { echo "FAIL mock compositor did not report MOCK-RESULT ok"; rc=1; }
  rm -f $dir/$id-mock
fi
[ -n "$xpid" ] && { kill $xpid 2>/dev/null; wait $xpid 2>/dev/null; }
if [ -n "${LEAK_KEEP:-}" ]; then echo "logs kept in $tmp"; else rm -rf $tmp; fi
rm -f $dir/$id-srv
exit $rc
