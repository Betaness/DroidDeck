#!/bin/sh
# The persistent pipeline cache in the game's process (src/pipeline_cache_hybris.c, the Vortek
# server's ../vortek/src/pipeline_cache.c) with ../vortek/tests/pcache_test through this ICD:
#   cc -O2 -o out/tests/pcache_test ../vortek/tests/pcache_test.c -lvulkan -lpthread
#   sh tests/pcache_test.sh [out/tests/pcache_test [out/tests/pcache_exit_test]]
# (pcache_exit_test: see tests/pcache_exit_test.c; step 12 is skipped without it)
# Each step runs pcache_test processes (tests/run.sh) with DROIDDECK_HYBRIS_PIPELINE_CACHE_DIR set
# to a new directory below a missing parent; PC_N (default 60) pipeline sets per run. Several game
# processes share the directory here, where the server had one process.
#  1 cold      first run (destroys its device): the file is written then; the directory is made
#  2 warm      the same pipelines from a new process: from the file, which is not written again
#  3 killed    new pipelines (4 threads), the process keeps its device and is SIGKILLed 8 s after
#              its last pipeline: the saver thread must have written them
#  4 merge     two processes of the same application add different pipelines at the same time:
#              the later save merges the earlier one's file, which ends up with both
#  5 damaged   bytes of the file overwritten: ignored ("unusable file"), pipelines still right,
#              a good file is written again
#  6 limits    _MAX_MB=1 keeps a cache over 1 MB out; _TOTAL_MB=1 removes the least recently used
#              file of another application
#  7 off       no directory, DROIDDECK_HYBRIS_PIPELINE_CACHE=0, VORTEK_PIPELINE_CACHE=0: nothing
#              is written and the second run is cold
#  8 ident     vertex libraries asked for by shader module identifier do not come from the cache
#  9 marker    a <file>.loading nobody holds (a process died while the driver parsed the file):
#              one death (an empty marker of an older build, too) is read once more: warm, the
#              marker gone; two in a row ("2"): the file is deleted instead of loaded, the run is
#              cold, the next one warm again
# 10 busy      a <file>.loading another live process holds and removes 3 s later: waited for,
#              then loaded (warm, nothing deleted); held for 14 s: not loaded after 10 s, the
#              file stays and the next run is warm
# 11 no-merge  process X only takes pipelines from the file while Y adds new ones and saves: X's
#              save finds nothing of its own missing and writes nothing
# 12 exit      a thread killed Wine's way while it waits in the load for a marker another process
#              holds, then exit(): the process ends within 8 s (it hung on the parse lock)
# 13 steam     SteamGameId (else SteamAppId) goes into the file name: <app>-<id>.<key>.vkpc
# "Warm" means the median monolithic pipeline (and fragment library) took under a third of the
# cold median. Every run checks each pipeline's pixel (PC verify). Prints PCACHE-PASS or PCACHE-FAIL.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
test=${1:-$HERE/../out/tests/pcache_test}
exit_test=${2:-$HERE/../out/tests/pcache_exit_test}
N=${PC_N:-60}
tmp=$(mktemp -d /tmp/hybris-pcache.XXXX)
fails=0
unset DROIDDECK_HYBRIS_PIPELINE_CACHE DROIDDECK_HYBRIS_PIPELINE_CACHE_DIR VORTEK_PIPELINE_CACHE VORTEK_PIPELINE_CACHE_DIR \
  DROIDDECK_HYBRIS_PIPELINE_CACHE_MAX_MB DROIDDECK_HYBRIS_PIPELINE_CACHE_TOTAL_MB VORTEK_PIPELINE_CACHE_MAX_MB VORTEK_PIPELINE_CACHE_TOTAL_MB \
  SteamGameId SteamAppId

fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

# run <cache-dir or ""> <out> [VAR=value...]: one pcache_test process
run() {
  cache=$1 out=$2; shift 2
  if [ -n "$cache" ]; then env PC_N=$N DROIDDECK_HYBRIS_PIPELINE_CACHE_DIR=$cache "$@" timeout 300 sh "$HERE/run.sh" "$test" >$out 2>&1
  else env PC_N=$N "$@" timeout 300 sh "$HERE/run.sh" "$test" >$out 2>&1
  fi
  grep -q '^PC-PASS' $out || fail "$out: $(grep -E 'PC verify|FAIL|wrong' $out | head -3 | tr '\n' ' ')"
}

median() { awk -v k="$2" '$1 == "PC" && $2 == k":" { for (i = 3; i < NF; i++) if ($i == "median") print $(i + 1) }' $1; }

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

wait_verify() { i=0; while ! grep -q 'PC verify' $1 2>/dev/null && [ $i -lt 600 ]; do sleep 0.5; i=$((i + 1)); done; }

files() { ls "$1" 2>/dev/null | grep -c '\.vkpc$'; }

# hold <marker> <seconds>: a live process holds the marker's lock like a parser, removes it, lets go
hold() {
  python3 -c 'import fcntl, os, sys, time
fd = os.open(sys.argv[1], os.O_RDWR | os.O_CREAT, 0o600)
fcntl.flock(fd, fcntl.LOCK_EX)
open(sys.argv[1] + ".held", "w").close()
time.sleep(float(sys.argv[2]))
os.unlink(sys.argv[1])
os.close(fd)' "$1" "$2" &
  i=0; while [ ! -e "$1.held" ] && [ $i -lt 100 ]; do sleep 0.05; i=$((i + 1)); done
  rm -f "$1.held"
}

# 1, 2
c1=$tmp/missing/parent/cache1
run $c1 $tmp/cold.out
[ "$(files $c1)" = 1 ] || fail "cold: $(files $c1) cache files after the device was destroyed"
grep -q 'pipeline cache pcache_test: new' $tmp/cold.out || fail "cold: no 'new' line"
run $c1 $tmp/warm.out
check_warm $tmp/cold.out $tmp/warm.out
grep -q 'pipeline cache pcache_test: loaded' $tmp/warm.out || fail "warm: no 'loaded' line"
grep -q ': saved [0-9]* KB in' $tmp/warm.out && fail "warm: rewrote an unchanged cache"

# 3: new pipelines (PC_SALT), the device stays, the process is SIGKILLed after the saver's idle wait
env PC_N=$N DROIDDECK_HYBRIS_PIPELINE_CACHE_DIR=$c1 PC_SALT=7000 PC_NO_DESTROY=1 PC_HOLD_S=60 PC_THREADS=4 \
  sh "$HERE/run.sh" "$test" >$tmp/killed-cold.out 2>&1 &
kpid=$!
wait_verify $tmp/killed-cold.out
sleep 8
kill -9 $kpid 2>/dev/null
wait $kpid 2>/dev/null
grep -q 'saved [0-9]* KB' $tmp/killed-cold.out || fail "killed: the saver thread did not save before the kill"
ls $c1 | grep -q '\.tmp-' && fail "killed: a temporary file was left"
run $c1 $tmp/killed-warm.out PC_SALT=7000
check_warm $tmp/killed-cold.out $tmp/killed-warm.out
run $c1 $tmp/killed-old.out
check_warm $tmp/cold.out $tmp/killed-old.out

# 4: process A (PC_SALT 20000) holds its device, B (30000) runs and saves, then A saves (merging)
c4=$tmp/cache4
run $c4 $tmp/mergeA.out PC_SALT=20000 PC_HOLD_S=12 &
apid=$!
wait_verify $tmp/mergeA.out
run $c4 $tmp/mergeB.out PC_SALT=30000 PC_THREADS=3
wait $apid
grep -q 'merged with a newer file' $tmp/mergeA.out $tmp/mergeB.out || fail "merge: neither save merged the other's file"
run $c4 $tmp/mergeA2.out PC_SALT=20000
run $c4 $tmp/mergeB2.out PC_SALT=30000
check_warm $tmp/mergeA.out $tmp/mergeA2.out
check_warm $tmp/mergeB.out $tmp/mergeB2.out

# 5: damage the file: 4 KB of zeros in the middle
f=$(ls $c1/*.vkpc | head -1)
size=$(stat -c %s "$f")
dd if=/dev/zero of="$f" bs=4096 count=1 seek=$((size / 8192)) conv=notrunc 2>/dev/null
run $c1 $tmp/damaged.out
grep -q 'unusable file' $tmp/damaged.out || fail "damaged: the damaged file was not refused"
check_cold $tmp/cold.out $tmp/damaged.out
run $c1 $tmp/repaired.out
check_warm $tmp/cold.out $tmp/repaired.out

# 6: limits
c6=$tmp/cache6
run $c6 $tmp/cap.out PC_N=100 DROIDDECK_HYBRIS_PIPELINE_CACHE_MAX_MB=1
grep -q 'over the 1 MB limit' $tmp/cap.out || fail "limits: no 'over the limit' line"
[ "$(files $c6)" = 0 ] || fail "limits: a cache over DROIDDECK_HYBRIS_PIPELINE_CACHE_MAX_MB was written"
run $c6 $tmp/lruA.out PC_N=20 PC_APP=appA.exe VORTEK_PIPELINE_CACHE_TOTAL_MB=1
sleep 1.2
run $c6 $tmp/lruB.out PC_N=20 PC_APP=appB.exe PC_SALT=100 VORTEK_PIPELINE_CACHE_TOTAL_MB=1
sleep 1.2
run $c6 $tmp/lruC.out PC_N=20 PC_APP=appC.exe PC_SALT=200 VORTEK_PIPELINE_CACHE_TOTAL_MB=1
ls $c6 | grep -q '^appA\.exe\.' && fail "limits: the least recently used file stayed: $(ls $c6 | tr '\n' ' ')"
ls $c6 | grep -q '^appC\.exe\.' || fail "limits: the newest file is missing: $(ls $c6 | tr '\n' ' ')"

# 7: off
c7=$tmp/cache7
run "" $tmp/off1.out
run "" $tmp/off2.out
check_cold $tmp/cold.out $tmp/off2.out
run $c7 $tmp/off3.out DROIDDECK_HYBRIS_PIPELINE_CACHE=0
run $c7 $tmp/off4.out VORTEK_PIPELINE_CACHE=0
check_cold $tmp/cold.out $tmp/off4.out
grep -q 'pipeline cache' $tmp/off1.out $tmp/off2.out $tmp/off3.out $tmp/off4.out && fail "off: a pipeline cache was still used"
[ -e $c7 ] && fail "off: $c7 was made"

# 8
run $c1 $tmp/ident.out PC_IDENT=20
grep -q '^PC ident: 0/20 ' $tmp/ident.out || fail "ident: $(grep '^PC ident' $tmp/ident.out): pipelines came from the cache by identifier"

# 9
f=$(ls $c1/*.vkpc | head -1)
: >"$f.loading"
run $c1 $tmp/marker1.out
grep -q 'died while the driver read this file last time, reading it once more' $tmp/marker1.out ||
  fail "marker: one death was not read once more"
grep -q 'deleted it' $tmp/marker1.out && fail "marker: the file was deleted after one death"
[ -e "$f.loading" ] && fail "marker: the .loading marker stayed after a good load"
check_warm $tmp/cold.out $tmp/marker1.out
echo 2 >"$f.loading"
run $c1 $tmp/marker.out
grep -q 'died while the driver read this file the last 2 times, deleted it' $tmp/marker.out ||
  fail "marker: the file was loaded despite two deaths"
[ -e "$f.loading" ] && fail "marker: the .loading marker stayed"
check_cold $tmp/cold.out $tmp/marker.out
run $c1 $tmp/marker-warm.out
check_warm $tmp/cold.out $tmp/marker-warm.out
ls $c1 | grep -q '\.loading' && fail "marker: a .loading marker was left after a good load"

# 10
hold "$f.loading" 3
run $c1 $tmp/busy.out
grep -q 'died while the driver read' $tmp/busy.out && fail "busy: a live process's marker was taken for a crash"
grep -q 'pipeline cache pcache_test: loaded' $tmp/busy.out || fail "busy: not loaded after the other process let go"
check_warm $tmp/cold.out $tmp/busy.out
hold "$f.loading" 14
run $c1 $tmp/busy-long.out
grep -q 'another process has been reading this file' $tmp/busy-long.out || fail "busy-long: no 'another process' line"
[ -e "$f" ] || fail "busy-long: the file of a live reader was deleted"
wait
run $c1 $tmp/busy-after.out
check_warm $tmp/cold.out $tmp/busy-after.out
ls $c1 | grep -q '\.loading' && fail "busy: a .loading marker was left"

# 11: X (PC_SALT 20000, all from the file) holds its device while Y (40000) compiles and saves
run $c4 $tmp/nomergeX.out PC_SALT=20000 PC_HOLD_S=12 DROIDDECK_HYBRIS_PIPELINE_CACHE_LOG=1 &
xpid=$!
wait_verify $tmp/nomergeX.out
run $c4 $tmp/nomergeY.out PC_SALT=40000 PC_THREADS=3 DROIDDECK_HYBRIS_PIPELINE_CACHE_LOG=1
wait $xpid
grep -q ': saved [0-9]* KB in' $tmp/nomergeY.out || fail "no-merge: Y's new pipelines were not saved"
grep -q ': saved [0-9]* KB in' $tmp/nomergeX.out && fail "no-merge: X rewrote the file although it added nothing"
run $c4 $tmp/nomergeY2.out PC_SALT=40000
run $c4 $tmp/nomergeA3.out PC_SALT=20000
check_warm $tmp/nomergeY.out $tmp/nomergeY2.out
check_warm $tmp/mergeA.out $tmp/nomergeA3.out

# 12
if [ -x "$exit_test" ]; then
  c12=$tmp/cache12
  env DROIDDECK_HYBRIS_PIPELINE_CACHE_DIR=$c12 PE_FIRST=1 timeout 60 sh "$HERE/run.sh" "$exit_test" >$tmp/exit-first.out 2>&1
  f12=$(ls $c12/*.vkpc 2>/dev/null | head -1)
  if [ -z "$f12" ]; then fail "exit: no cache file made"
  else
    hold "$f12.loading" 12
    t0=$(date +%s)
    env DROIDDECK_HYBRIS_PIPELINE_CACHE_DIR=$c12 timeout 30 sh "$HERE/run.sh" "$exit_test" >$tmp/exit.out 2>&1
    rc=$? t=$(($(date +%s) - t0))
    grep -q 'PE worker killed inside' $tmp/exit.out || fail "exit: the worker was not inside the call"
    grep -q 'PE-EXIT' $tmp/exit.out || fail "exit: the main thread did not get to exit()"
    [ $rc -eq 0 ] && [ $t -le 8 ] || fail "exit: exit() took $t s (rc $rc)"
    wait
    [ -e "$f12" ] || fail "exit: the cache file was deleted"
  fi
else echo "step 12 skipped: no $exit_test"
fi

# 13
c13=$tmp/cache13
run $c13 $tmp/steam.out SteamGameId=3167020 SteamAppId=1
ls $c13 | grep -q '^pcache_test-3167020\.[0-9a-f]*\.vkpc$' || fail "steam: $(ls $c13 | tr '\n' ' ')not named after SteamGameId"
run $c13 $tmp/steam2.out SteamAppId=0
ls $c13 | grep -q '^pcache_test\.[0-9a-f]*\.vkpc$' || fail "steam: SteamAppId=0 was not left out"

for f in cold warm killed-cold killed-warm mergeA mergeA2 damaged off2 busy; do
  printf '%-12s full %6s ms  fslib %6s ms  vslib %6s ms  link %6s ms (medians)\n' $f "$(median $tmp/$f.out full)" "$(median $tmp/$f.out fslib)" \
    "$(median $tmp/$f.out vslib)" "$(median $tmp/$f.out link)"
done
grep -h 'pipeline cache pcache_test: [0-9]* pipelines' $tmp/cold.out $tmp/warm.out
if [ $fails -eq 0 ]; then rm -rf $tmp; echo PCACHE-PASS; exit 0; fi
echo "logs in $tmp"
echo PCACHE-FAIL
exit 1
