#!/usr/bin/env python3
"""Capture Mali hardware counters (no root) through perfetto and print per-frame figures.

usage: mali-counters.py SECONDS FPS OUT.pftrace   (adb connected; needs the perfetto python package)
Starts the vendor gpu_counter_producer if needed, records all counters plus gpufreq, prints
frequency, GPU active cycles per frame and bandwidth. GPU active cycles / frequency ~ 90% only
means the governor picked a clock; compare cycles per frame, not utilisation."""
import subprocess, sys
secs=float(sys.argv[1]); fps=float(sys.argv[2]); out=sys.argv[3]
ids=" ".join(f"counter_ids: {i}" for i in range(0,460))
cfg=f'''buffers {{ size_kb: 262144 fill_policy: RING_BUFFER }}
data_sources {{ config {{ name: "gpu.counters" gpu_counter_config {{ counter_period_ns: 10000000 {ids} }} }} }}
data_sources {{ config {{ name: "linux.ftrace" ftrace_config {{ ftrace_events: "power/gpu_frequency" ftrace_events: "devfreq/devfreq_frequency" }} }} }}
duration_ms: {int(secs*1000)}
'''
subprocess.run(["adb","shell","pidof gpu_counter_producer >/dev/null || gpu_counter_producer; sleep 1"])
subprocess.run(["adb","shell",f"perfetto -c - --txt -o /data/misc/perfetto-traces/gc.pftrace"],input=cfg.encode(),capture_output=True)
subprocess.run(["adb","pull","/data/misc/perfetto-traces/gc.pftrace",out],capture_output=True)
from perfetto.trace_processor import TraceProcessor
tp=TraceProcessor(trace=out)
rows={r.n:(r.v,r.f) for r in tp.query("select t.name n, sum(c.value) v, avg(c.value) f from counter c join gpu_counter_track t on c.track_id=t.id group by t.name")}
fr=secs*fps
def pf(n): return rows.get(n,(0,0))[0]/fr
fq=list(tp.query("select t.name n, c.ts ts, c.value v from counter c join counter_track t on c.track_id=t.id where t.name like '%freq%' order by t.name, c.ts"))
import collections
byn=collections.defaultdict(list)
for r in fq: byn[r.n].append((r.ts,r.v))
for n,pts in byn.items():
    if len(pts)<2: print("freq",n,pts[:1]); continue
    tot=sum((pts[i+1][0]-pts[i][0])*pts[i][1] for i in range(len(pts)-1)); dur=pts[-1][0]-pts[0][0]
    print(f"freq {n}: time-avg {tot/dur/1e6:.0f} MHz, min {min(v for _,v in pts)/1e6:.0f} max {max(v for _,v in pts)/1e6:.0f}")
print(f"frames={fr:.0f}")
for n in ["GPU active cycles","Output external read beats","Output external write beats","Output external write transactions","Output external ReadNoSnoop transactions","Tile unit write beats to L2 memory system","Tiles","Killed unchanged tiles","Rasterized fine quads","Fragment warps","Main phase jobs","Compute jobs","Texture unit read beats from external memory","MMU level 3 lookup TLB misses","MMU level 2 lookup TLB misses","MMU lookup requests","Input internal snoop requests","Fragment main pass stall cycles","Processing unit starvation cycles","Execution core active cycles"]:
    print(f"  {n:45s} {pf(n):14.4g} /frame")
ga=pf("GPU active cycles"); print(f"GPU active ms/frame @1248MHz {ga/1.248e6:.2f}; frame interval {1000/fps:.1f} ms")
