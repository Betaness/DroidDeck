#!/usr/bin/env python3
"""GPU idle gaps and what the game's threads do in them (no root).

usage: gpu-gaps.py SECONDS OUT.pftrace [thread names...]
Records Mali counters at 1 ms (vendor gpu_counter_producer) with sched_switch/waking over adb,
finds the 1 ms samples where the GPU was <30% busy, and prints, for each named thread (default:
the Unity/DXVK/Vortek chain), how much of the idle time it spent running. A long gap filled by
CPU threads means the frame's critical path is on the CPU side.
"""
import bisect, statistics, subprocess, sys
from perfetto.trace_processor import TraceProcessor

secs = float(sys.argv[1]); out = sys.argv[2]
names = sys.argv[3:] or ["UnityGfxDeviceW", "dxvk-cs", "dxvk-submit", "libvortekserver", "wineserver"]
ids = " ".join(f"counter_ids: {i}" for i in range(0, 460))
cfg = f"""buffers {{ size_kb: 262144 fill_policy: RING_BUFFER }}
data_sources {{ config {{ name: "linux.ftrace" ftrace_config {{ ftrace_events: "sched/sched_switch"
  ftrace_events: "sched/sched_waking" ftrace_events: "power/gpu_frequency" }} }} }}
data_sources {{ config {{ name: "gpu.counters" gpu_counter_config {{ counter_period_ns: 1000000 {ids} }} }} }}
data_sources {{ config {{ name: "linux.process_stats" process_stats_config {{ scan_all_processes_on_start: true }} }} }}
duration_ms: {int(secs * 1000)}
"""
subprocess.run(["adb", "shell", "pidof gpu_counter_producer >/dev/null || gpu_counter_producer; sleep 1"])
subprocess.run(["adb", "shell", "perfetto -c - --txt -o /data/misc/perfetto-traces/gaps.pftrace"], input=cfg.encode(), capture_output=True)
subprocess.run(["adb", "pull", "/data/misc/perfetto-traces/gaps.pftrace", out], capture_output=True)
tp = TraceProcessor(trace=out)
q = lambda s: list(tp.query(s))
g = q("select c.ts ts, c.value v from counter c join gpu_counter_track t on c.track_id=t.id where t.name='GPU active cycles' order by c.ts")
f = q("select c.ts ts, c.value v from counter c join counter_track t on c.track_id=t.id where t.name='gpufreq' order by c.ts")
if not g or not f:
    sys.exit("no Mali counters in the trace (the producer sometimes delivers none: run again)")
fts = [x.ts for x in f]
def freq(ts):
    return f[max(bisect.bisect_right(fts, ts) - 1, 0)].v
util = [(g[i - 1].ts, g[i].ts, g[i].v / (freq(g[i].ts) * (g[i].ts - g[i - 1].ts) / 1e9)) for i in range(1, len(g)) if g[i].ts > g[i - 1].ts]
gaps, cur = [], None
for a, b, u in util:
    if u < 0.3:
        cur = [a, b] if cur is None else [cur[0], b]
    elif cur:
        gaps.append(cur); cur = None
lengths = [(b - a) / 1e6 for a, b in gaps] or [0]
span = (util[-1][1] - util[0][0]) / 1e9
print(f"GPU idle: {len(gaps)} gaps in {span:.1f} s, median {statistics.median(lengths):.1f} ms, {100 * sum(lengths) / 1000 / span:.0f}% of the time")
for name in names:
    rows = q(f"select s.ts ts, s.dur dur, t.tid tid from sched s join thread t using(utid) where t.name='{name}'")
    if not rows:
        continue
    by = {}
    for r in rows:
        by.setdefault(r.tid, []).append((r.ts, r.ts + r.dur))
    tid = max(by, key=lambda k: sum(e - s for s, e in by[k]))
    inside = sum(min(e, gb) - max(s, ga) for ga, gb in gaps for s, e in by[tid] if e > ga and s < gb)
    print(f"  {name:16s} running {inside / 1e6:5.0f} ms of {sum(lengths):.0f} ms idle")
