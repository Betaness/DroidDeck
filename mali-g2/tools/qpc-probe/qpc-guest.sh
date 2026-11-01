# qpc-guest.sh (runs in the DroidDeck guest: droiddeckctl guest, or a test harness's g.sh) <command> [args]
# The QueryPerformanceCounter measurement of research item 2: how often the game's main thread calls it
# (libqpcprobe.so, LD_PRELOADed into the game) and what one call costs there (qpcbench.exe, run with the
# game's own Wine and environment). See README.md.
#   install          copy libqpcprobe.so, qpcbench.exe and qpc-report.py from $INBOX (the app's agent-inbox:
#                    adb push them there first) to $Q, and put the probe into games[$APPID] of
#                    game-environment.json: LD_PRELOAD = $BASE_PRELOAD:<probe> (the entry replaces the inherited
#                    LD_PRELOAD; droiddeck-game-env still puts libhybris-common first), QPCPROBE_DIR,
#                    QPCPROBE_MATCH, QPCPROBE_INTERVAL_MS. Before the game starts; the app rewrites the file at
#                    every session start, so nothing outlives the session. The old logs are deleted.
#   check            the game process (comm $GAME_COMM, the one with the most threads): whether it maps the probe,
#                    its LD_PRELOAD, the snapshots in its log. Exit 1 when the probe is not in it, 3 without a game.
#   bench [N] [R]    qpcbench.exe N R (default 1000000 7) with the game's Wine (WINELOADER, else wine beside the
#                    process's executable) and its environment, without the probe and with WINEDEBUG=-all, on a
#                    core of the main thread's cluster other than its own (the game keeps running); prints the
#                    output and that core's clock before and after, and keeps it in $Q/bench-<time>.txt.
#   report [S] [..]  qpc-report.py over the game's log (the running game's, else the busiest matching one), last S
#                    seconds (default 120), with every bench output and any further arguments (--fps F,
#                    --main-ghz G ...); --json works too.
#   dump             the probe log of the running game (else the newest one), gzip + base64, for the host's own
#                    qpc-report.py
#   restore          take the probe out of game-environment.json (a running game keeps it until it exits) and
#                    delete $Q
# env: APPID (3167020), GAME_COMM (Duckov.exe), Q (~/.cache/droiddeck-qpcprobe), INBOX, BASE_PRELOAD (default: what
# Duckov's processes inherit in G2 sessions: libblsession, libfakeinput, Steam's three overlay entries).
A=${APPID:-3167020}
G=${GAME_COMM:-Duckov.exe}
Q=${Q:-$HOME/.cache/droiddeck-qpcprobe}
INBOX=${INBOX:-/storage/emulated/0/Android/data/com.droiddeck.launcher.g2/files/agent-inbox}
F=$HOME/.config/droiddeck/game-environment.json
S=$HOME/.local/share/Steam
BASE_PRELOAD=${BASE_PRELOAD:-/usr/local/lib/libblsession.so:/usr/local/lib/libfakeinput.so:$S/steamrtarm64/gameoverlayrenderer.so:$HOME/.steam/bin32/gameoverlayrenderer.so:$S/steamrtarm64/../steamrtarm64/gameoverlayrenderer.so}

game() {
  p= ; n=0
  for c in /proc/[0-9]*/comm; do
    { read -r x < "$c"; } 2>/dev/null || continue
    [ "$x" = "$G" ] || continue
    q=${c#/proc/}; q=${q%/comm}
    k=$(ls /proc/$q/task 2>/dev/null | wc -l); [ "$k" -gt "$n" ] && { p=$q; n=$k; }
  done
  [ -n "$p" ] || { [ "${1:-}" = optional ] && return 0; echo "GAME none"; exit 3; }
}

envjson() { # set | unset
  [ -f "$F" ] || { echo "no $F"; exit 1; }
  python3 - "$F" "$A" "$1" "$BASE_PRELOAD:$Q/libqpcprobe.so" "$Q/logs" "$G" <<'PY'
import json, os, sys
f, appid, what, preload, logs, match = sys.argv[1:]
c = json.load(open(f))
g = c.setdefault("games", {}).setdefault(appid, {})
ours = {"LD_PRELOAD": preload, "QPCPROBE_DIR": logs, "QPCPROBE_MATCH": match, "QPCPROBE_INTERVAL_MS": "5000"}
if what == "set":
    g.update(ours)
else:
    for k, v in ours.items():
        if g.get(k) == v:
            del g[k]
json.dump(c, open(f + ".qpc-tmp", "w"), indent=1)
os.replace(f + ".qpc-tmp", f)
print("ok games[%s]: %s" % (appid, ", ".join("%s=%s" % (k, g[k]) for k in ours if k in g) or "no probe entries"))
PY
}

case ${1:-} in
  install)
    mkdir -p "$Q/logs" || exit 1
    rm -f "$Q"/logs/qpc-*.log "$Q"/bench-*.txt
    for f in libqpcprobe.so qpcbench.exe qpc-report.py; do
      [ -r "$INBOX/$f" ] || { echo "no $INBOX/$f (adb push it there first)"; exit 1; }
      cp "$INBOX/$f" "$Q/$f.new" && mv "$Q/$f.new" "$Q/$f" || exit 1
    done
    chmod 755 "$Q/libqpcprobe.so"
    sha256sum "$Q/libqpcprobe.so" "$Q/qpcbench.exe" "$Q/qpc-report.py" | sed 's/^/SHA /'
    envjson set ;;
  check)
    game
    m=$(grep -c libqpcprobe "/proc/$p/maps" 2>/dev/null)
    l=$(tr '\0' '\n' < "/proc/$p/environ" | sed -n 's/^LD_PRELOAD=//p')
    s=$(grep -c '^T ' "$Q/logs/qpc-$p.log" 2>/dev/null)
    echo "GAME $p maps probe ${m:-0}, snapshots ${s:-0}"
    echo "LD_PRELOAD $l"
    [ "${m:-0}" -gt 0 ] || exit 1 ;;
  bench)
    game
    python3 - "$p" "$Q" "${2:-1000000}" "${3:-7}" <<'PY'
import os, subprocess, sys, time
pid, q, n, r = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
env = dict(kv.split("=", 1) for kv in open("/proc/%s/environ" % pid).read().split("\0") if "=" in kv)
env = {k: v for k, v in env.items() if not k.startswith("QPCPROBE_")}
env["LD_PRELOAD"] = ":".join(x for x in env.get("LD_PRELOAD", "").split(":") if x and "libqpcprobe" not in x)
env["WINEDEBUG"] = "-all"
exe = os.path.realpath("/proc/%s/exe" % pid)
wine = next((w for w in (env.get("WINELOADER"), os.path.join(os.path.dirname(exe), "wine"), os.path.join(os.path.dirname(exe), "wine64"))
             if w and os.access(w, os.X_OK)), None)
if not wine:
    print("BENCH no wine (exe %s)" % exe); sys.exit(1)
cpu = int(open("/proc/%s/task/%s/stat" % (pid, pid)).read().rsplit(")", 1)[1].split()[36])
def cpus(path):
    try:
        text = open(path).read().split()
    except OSError:
        return []
    out = []
    for part in ",".join(text).split(","):
        if "-" in part:
            a, b = part.split("-"); out += range(int(a), int(b) + 1)
        elif part:
            out.append(int(part))
    return out
cluster = cpus("/sys/devices/system/cpu/cpu%d/cpufreq/related_cpus" % cpu) or [cpu]
allowed = sorted(os.sched_getaffinity(0))
pick = next((c for c in cluster if c != cpu and c in allowed), cpu if cpu in allowed else allowed[0])
def khz(c):
    try:
        return int(open("/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq" % c).read())
    except (OSError, ValueError):
        return 0
before = khz(pick)
t0 = time.time()
res = subprocess.run([wine, os.path.join(q, "qpcbench.exe"), n, r], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                     timeout=180, preexec_fn=lambda: os.sched_setaffinity(0, {pick}))
after = khz(pick)
out = res.stdout.decode("utf-8", "replace")
head = "BENCH wine %s, main thread on cpu %d (cluster %s), bench on cpu %d at %.2f -> %.2f GHz, %.1f s, rc %d\n" % (
    wine, cpu, ",".join(map(str, cluster)), pick, before / 1e6, after / 1e6, time.time() - t0, res.returncode)
path = os.path.join(q, "bench-%d.txt" % int(t0))
open(path, "w").write(head + out)
sys.stdout.write(head + out)
if res.returncode or "qpcbench qpc " not in out:
    sys.stdout.write("BENCH stderr: " + res.stderr.decode("utf-8", "replace")[-2000:] + "\n"); sys.exit(1)
PY
    ;;
  report)
    game optional
    last=${2:-120}; [ $# -ge 2 ] && shift 2 || shift 1
    set -- --last "$last" --match "$G" "$@"
    [ -n "$p" ] && set -- --pid "$p" "$@"
    for b in "$Q"/bench-*.txt; do [ -e "$b" ] && set -- "$@" --bench "$b"; done
    python3 "$Q/qpc-report.py" "$@" "$Q/logs" ;;
  dump)
    game optional
    f=$Q/logs/qpc-$p.log
    [ -n "$p" ] || f=$(ls -t "$Q"/logs/qpc-*.log 2>/dev/null | head -1)
    [ -r "$f" ] || { echo "no probe log ($Q/logs)" >&2; exit 1; }
    gzip -c "$f" | base64 -w0; echo ;;
  restore)
    [ -f "$F" ] && envjson unset
    rm -rf "$Q"; echo "ok removed $Q" ;;
  *) sed -n '2,30p' "$0"; exit 2 ;;
esac
