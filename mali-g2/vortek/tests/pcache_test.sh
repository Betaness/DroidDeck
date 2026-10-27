#!/bin/sh
# Checks the Vortek server's persistent pipeline cache (src/pipeline_cache.c) with pcache_test.
# Run on the device, like the other server tests:
#   pcache_test.sh <libvortekserver.so> <client-dir> <pcache_test>
# <client-dir> is a build-client.sh output. Each step starts a fresh server on a private socket
# with VORTEK_PIPELINE_CACHE_DIR set to a new directory; PC_N (default 60) pipeline sets per run.
#  1 cold      first run: compiles and exits without destroying anything (a killed game): the
#              server writes <app>.<driver>.vkpc when it cleans up after the client
#  2 warm      the same pipelines from a new server process (SIGKILLed before): from the file
#  3 periodic  new pipelines (4 threads), the client keeps its device and the server is SIGKILLed
#              15 s later (the app's session end): the saver thread must have written them already
#  4 merge     two devices of the same application add different pipelines at the same time:
#              the file ends up with both
#  5 damaged   bytes of the file overwritten: ignored ("unusable file"), pipelines still right,
#              a good file is written again
#  6 limits    VORTEK_PIPELINE_CACHE_MAX_MB=1 keeps a cache over 1 MB out; TOTAL_MB=1 removes the
#              least recently used file of another application
#  7 off       without VORTEK_PIPELINE_CACHE_DIR nothing is written and the second run is cold
#  8 ident     vertex libraries asked for by shader module identifier (DXVK's recompiles) do not
#              come from the cache (PC_IDENT: none of them may be created without compiling)
#  9 marker    a <file>.loading left behind (the server died while the driver parsed the file):
#              one death (an empty marker of an older build, too) is read once more: warm, the
#              marker gone; two in a row ("2"): the file is deleted instead of loaded, the run is
#              cold, the next one warm again
# 10 no-merge  device X only takes pipelines from the file while device Y adds new ones and
#              saves: X's save finds nothing of its own missing and writes nothing
# 11 own       the application passes its own cache (PC_CACHE_FILE), over 1 MB, and saves it:
#              vkGetPipelineCacheData's reply is larger than the reply ring; loaded again, warm
# "Warm" means the median monolithic pipeline (and fragment library) took under a third of the
# cold median. Every run also checks each pipeline's pixel (pcache_test's PC verify), so a
# pipeline taken from the cache under a wrong key fails. Prints PCACHE-PASS or PCACHE-FAIL.
set -u
srv=$1 cdir=$2 test=$3
N=${PC_N:-60}
dir=/data/local/vortek
tmp=$(mktemp -d /tmp/pcache-test.XXXX)
id=pcache-$$
cp "$srv" $dir/$id-srv && chmod 700 $dir/$id-srv
sed "s#\"library_path\": *\"[^\"]*\"#\"library_path\": \"$cdir/libvulkan_vortek.so\"#" "$cdir/vortek_icd.aarch64.json" >$tmp/icd.json
export VK_ICD_FILENAMES=$tmp/icd.json VK_DRIVER_FILES=$tmp/icd.json
fails=0
spid=

fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

# start_server <cache-dir or ""> [VAR=value...]
start_server() {
  cache=$1; shift
  n=$(ls $tmp | grep -c '^server')
  sock=$tmp/s$n.sock log=$tmp/server$n.log
  if [ -n "$cache" ]; then env VORTEK_PIPELINE_CACHE_DIR=$cache "$@" $dir/$id-srv $sock >$log 2>&1 &
  else env -u VORTEK_PIPELINE_CACHE_DIR "$@" $dir/$id-srv $sock >$log 2>&1 &
  fi
  spid=$!
  i=0; while [ ! -S $sock ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
  export VORTEK_SERVER_PATH=$sock
}

kill_server() { kill -9 $spid 2>/dev/null; wait $spid 2>/dev/null; }

# run <out> [VAR=value...]: one pcache_test run
run() {
  out=$1; shift
  env PC_N=$N "$@" timeout 300 "$test" >$out 2>&1
  grep -q '^PC-PASS' $out || fail "$out: $(grep -E 'PC verify|FAIL|wrong' $out | head -3 | tr '\n' ' ')"
}

median() { awk -v k="$2" '$1 == "PC" && $2 == k":" { for (i = 3; i < NF; i++) if ($i == "median") print $(i + 1) }' $1; }

# warm <cold-out> <out>: medians of full and fslib under a third of the cold ones
check_warm() {
  for k in full fslib; do
    c=$(median $1 $k) w=$(median $2 $k)
    awk -v c="$c" -v w="$w" 'BEGIN { exit !(c != "" && w != "" && w * 3 < c) }' || fail "$2: $k median $w ms not warm (cold $c ms)"
  done
}

check_cold() {
  c=$(median $1 full) w=$(median $2 full)
  awk -v c="$c" -v w="$w" 'BEGIN { exit !(c != "" && w != "" && w * 2 > c) }' || fail "$2: full median $w ms, cold $c ms: unexpectedly warm"
}

# wait_log <log> <pattern>: up to 20 s
wait_log() { i=0; while ! grep -q "$2" $1 && [ $i -lt 200 ]; do sleep 0.1; i=$((i + 1)); done; }

files() { ls "$1" 2>/dev/null | grep -c '\.vkpc$'; }

# 1, 2
c1=$tmp/cache1
start_server $c1
run $tmp/cold.out PC_NO_DESTROY=1
wait_log $tmp/server0.log 'pcache_test: [0-9]* pipelines in'
kill_server
[ "$(files $c1)" = 1 ] || fail "cold: $(files $c1) cache files after the device was destroyed"
grep -q 'pipeline cache pcache_test: new' $tmp/server0.log || fail "cold: no 'new' line"
start_server $c1
run $tmp/warm.out
check_warm $tmp/cold.out $tmp/warm.out
sleep 1
kill_server
grep -q 'pipeline cache pcache_test: loaded' $tmp/server1.log || fail "warm: no 'loaded' line"
grep -q ': saved [0-9]* KB in' $tmp/server1.log && fail "warm: rewrote an unchanged cache"

# 3: new pipelines (PC_SALT), the device stays, the server is killed after the saver's idle wait
start_server $c1
run $tmp/periodic-cold.out PC_SALT=7000 PC_NO_DESTROY=1 PC_HOLD_S=30 PC_THREADS=4 &
cpid=$!
i=0; while ! grep -q 'PC verify' $tmp/periodic-cold.out 2>/dev/null && [ $i -lt 600 ]; do sleep 0.5; i=$((i + 1)); done
sleep 15
kill_server
wait $cpid 2>/dev/null
ls $c1 | grep -q '\.tmp-' && fail "periodic: a temporary file was left"
start_server $c1
run $tmp/periodic-warm.out PC_SALT=7000
check_warm $tmp/periodic-cold.out $tmp/periodic-warm.out
run $tmp/periodic-old.out
check_warm $tmp/cold.out $tmp/periodic-old.out
sleep 1
kill_server

# 4: device A (PC_SALT 20000) holds, B (30000) runs and saves, then A saves (merging)
c4=$tmp/cache4
start_server $c4
run $tmp/mergeA.out PC_SALT=20000 PC_HOLD_S=12 &
apid=$!
i=0; while ! grep -q 'PC verify' $tmp/mergeA.out 2>/dev/null && [ $i -lt 600 ]; do sleep 0.5; i=$((i + 1)); done
run $tmp/mergeB.out PC_SALT=30000 PC_THREADS=3
wait $apid
sleep 1
kill_server
grep -q 'merged with a newer file' $tmp/server4.log || fail "merge: A's save did not merge B's file"
start_server $c4
run $tmp/mergeA2.out PC_SALT=20000
run $tmp/mergeB2.out PC_SALT=30000
kill_server
check_warm $tmp/mergeA.out $tmp/mergeA2.out
check_warm $tmp/mergeB.out $tmp/mergeB2.out

# 5: damage the file: 4 KB of zeros in the middle
f=$(ls $c1/*.vkpc | head -1)
size=$(stat -c %s "$f")
dd if=/dev/zero of="$f" bs=4096 count=1 seek=$((size / 8192)) conv=notrunc 2>/dev/null
start_server $c1
run $tmp/damaged.out
sleep 1
kill_server
grep -q 'unusable file' $tmp/server6.log || fail "damaged: the damaged file was not refused"
check_cold $tmp/cold.out $tmp/damaged.out
start_server $c1
run $tmp/repaired.out
kill_server
check_warm $tmp/cold.out $tmp/repaired.out

# 6: limits
c6=$tmp/cache6
start_server $c6 VORTEK_PIPELINE_CACHE_MAX_MB=1
run $tmp/cap.out PC_N=100
sleep 1
kill_server
grep -q 'over the 1 MB limit' $tmp/server8.log || fail "limits: no 'over the limit' line"
[ "$(files $c6)" = 0 ] || fail "limits: a cache over VORTEK_PIPELINE_CACHE_MAX_MB was written"
start_server $c6 VORTEK_PIPELINE_CACHE_TOTAL_MB=1
run $tmp/lruA.out PC_N=20 PC_APP=appA.exe
sleep 1.2
run $tmp/lruB.out PC_N=20 PC_APP=appB.exe PC_SALT=100
sleep 1.2
run $tmp/lruC.out PC_N=20 PC_APP=appC.exe PC_SALT=200
sleep 1
kill_server
ls $c6 | grep -q '^appA\.exe\.' && fail "limits: the least recently used file stayed: $(ls $c6 | tr '\n' ' ')"
ls $c6 | grep -q '^appC\.exe\.' || fail "limits: the newest file is missing: $(ls $c6 | tr '\n' ' ')"

# 7: off
start_server ""
run $tmp/off1.out
run $tmp/off2.out
kill_server
check_cold $tmp/cold.out $tmp/off2.out
grep -q 'pipeline cache' $tmp/server10.log && fail "off: the server still used a pipeline cache"

# 8
start_server $c1
run $tmp/ident.out PC_IDENT=20
kill_server
grep -q '^PC ident: 0/20 ' $tmp/ident.out || fail "ident: $(grep '^PC ident' $tmp/ident.out): pipelines came from the cache by identifier"

# 9
f=$(ls $c1/*.vkpc | head -1)
: >"$f.loading"
start_server $c1
run $tmp/marker1.out
sleep 1
kill_server
grep -q 'died while the driver read this file last time, reading it once more' $log || fail "marker: one death was not read once more"
grep -q 'deleted it' $log && fail "marker: the file was deleted after one death"
[ -e "$f.loading" ] && fail "marker: the .loading marker stayed after a good load"
check_warm $tmp/cold.out $tmp/marker1.out
echo 2 >"$f.loading"
start_server $c1
run $tmp/marker.out
sleep 1
kill_server
grep -q 'died while the driver read this file the last 2 times, deleted it' $log || fail "marker: the file was loaded despite two deaths"
[ -e "$f.loading" ] && fail "marker: the .loading marker stayed"
check_cold $tmp/cold.out $tmp/marker.out
start_server $c1
run $tmp/marker-warm.out
kill_server
check_warm $tmp/cold.out $tmp/marker-warm.out
ls $c1 | grep -q '\.loading$' && fail "marker: a .loading marker was left after a good load"

# 10: X (PC_SALT 20000, all from the file) holds its device while Y (40000) compiles and saves
start_server $c4 VORTEK_PIPELINE_CACHE_LOG=1
run $tmp/nomergeX.out PC_SALT=20000 PC_HOLD_S=12 &
xpid=$!
i=0; while ! grep -q 'PC verify' $tmp/nomergeX.out 2>/dev/null && [ $i -lt 600 ]; do sleep 0.5; i=$((i + 1)); done
run $tmp/nomergeY.out PC_SALT=40000 PC_THREADS=3
wait $xpid
sleep 1
kill_server
grep -q ': saved [0-9]* KB in' $log || fail "no-merge: Y's new pipelines were not saved"
grep -q 'merged with a newer file' $log && fail "no-merge: X rewrote the file although it added nothing"
start_server $c4
run $tmp/nomergeY2.out PC_SALT=40000
run $tmp/nomergeA3.out PC_SALT=20000
kill_server
check_warm $tmp/nomergeY.out $tmp/nomergeY2.out
check_warm $tmp/mergeA.out $tmp/nomergeA3.out

# 11: the application's own cache (PC_CACHE_FILE), over 1 MB: vkGetPipelineCacheData's reply is
# larger than the 256 KB reply ring (vortek-0013/winlator-0021); saved, loaded again, warm
own=$tmp/own.bin
start_server $c1
run $tmp/own-cold.out PC_CACHE_FILE=$own
run $tmp/own-warm.out PC_CACHE_FILE=$own
kill_server
saved=$(awk '/^PC app cache: [0-9]+ bytes saved/ { print $4; exit }' $tmp/own-cold.out)
[ "${saved:-0}" -gt 262144 ] || fail "own: the application's cache was ${saved:-not} saved (want over 256 KB)"
grep -q "^PC app cache: $saved bytes loaded" $tmp/own-warm.out || fail "own: the saved cache was not loaded back"
c=$(median $tmp/cold.out full) w=$(median $tmp/own-warm.out full)
awk -v c="$c" -v w="$w" 'BEGIN { exit !(c != "" && w != "" && w * 3 < c) }' || fail "own: full median $w ms with the application's cache (cold $c ms)"
grep -q 'buffer overflow' $tmp/own-cold.out $tmp/own-warm.out && fail "own: ring overflow"

for f in cold warm periodic-cold periodic-warm mergeA mergeA2 damaged off2 own-warm; do
  printf '%-14s full %6s ms  fslib %6s ms  vslib %6s ms  link %6s ms (medians)\n' $f "$(median $tmp/$f.out full)" "$(median $tmp/$f.out fslib)" \
    "$(median $tmp/$f.out vslib)" "$(median $tmp/$f.out link)"
done
grep -h 'pipeline cache pcache_test: [0-9]* pipelines' $tmp/server0.log $tmp/server1.log
rm -f $dir/$id-srv
if [ $fails -eq 0 ]; then rm -rf $tmp; echo PCACHE-PASS; exit 0; fi
echo "logs in $tmp"
echo PCACHE-FAIL
exit 1
