#!/usr/bin/env python3
"""qpc-report.py: QueryPerformanceCounter calls of a game's main thread, from libqpcprobe.so's logs, and
what they cost, from qpcbench.exe's output.

usage: qpc-report.py [--match Duckov.exe] [--pid PID] [--last S] [--bench FILE]... [--fps F]
                     [--bench-ghz G --main-ghz G] [--no-symbols] [--json] LOG_OR_DIR...

LOG_OR_DIR: qpc-<pid>.log files (or directories of them). The process is --pid, else the one whose command line
contains --match with the most calls. Rates are over its last --last seconds (default 120) of snapshots.
The main thread is the one whose tid is the pid (Wine runs the Windows main thread on the process's first thread).
Its QPC calls are its clock_gettime(CLOCK_MONOTONIC_RAW) calls from Wine's unix ntdll.so (monotonic_counter() in
NtQueryPerformanceCounter is the only such call there). Symbols come from the mapped files themselves when they are
readable here (run it in the guest; a host path of proot's is tried by its tails), else file+offset is shown.

--bench: qpcbench.exe output(s); the median of each kind's medians is used. Per call t = qpc; the floor a QPC that
reads the CPU counter in user space would still pay is tick64 (the call into Wine's PE code) + rdtsc - 2 x empty.
Main-thread share = N x t; a saving of N x (t - floor). --bench-ghz/--main-ghz scale the bench's times to the main
thread's clock (the bench ran on another core). Gate (research item 2): the share must reach 1 % of the main
thread (40-100 k calls/s) before the wine change is worth making.
"""
import argparse
import glob
import json
import os
import struct
import sys

GATE = 0.01
CLOCK_NAMES = {0: "REALTIME", 1: "MONOTONIC", 4: "MONOTONIC_RAW", 5: "REALTIME_COARSE", 6: "MONOTONIC_COARSE", 7: "BOOTTIME"}


def parse_log(path):
    """{pid, cmd, snaps: [(t_ns, {(tid, clock, addr): count}, {tid: comm})], maps: [(lo, hi, offset, path)]}"""
    proc = {"pid": None, "cmd": "", "snaps": [], "maps": [], "file": path, "dropped": 0}
    cur = None
    with open(path, errors="replace") as f:
        for line in f:
            parts = line.rstrip("\n").split(" ")
            kind = parts[0]
            try:
                if kind == "P" and len(parts) >= 2:
                    proc["pid"] = int(parts[1])
                    proc["cmd"] = " ".join(parts[2:]).strip()
                elif kind == "T" and len(parts) >= 5:
                    cur = (int(parts[1]), {}, {})
                    proc["snaps"].append(cur)
                    proc["dropped"] = max(proc["dropped"], int(parts[4]))
                elif kind == "S" and len(parts) >= 6 and cur is not None:
                    tid, comm, clock, addr, count = int(parts[1]), parts[2], int(parts[3]), int(parts[4], 16), int(parts[5])
                    cur[1][(tid, clock, addr)] = count
                    cur[2][tid] = comm
                elif kind == "M" and len(parts) >= 2:
                    fields = line[2:].split()
                    lo, hi = (int(x, 16) for x in fields[0].split("-"))
                    offset = int(fields[2], 16) if len(fields) > 2 else 0
                    name = fields[5] if len(fields) > 5 else ""
                    proc["maps"].append((lo, hi, offset, name))
            except ValueError:
                continue
    if proc["pid"] is None:
        base = os.path.basename(path)
        if base.startswith("qpc-") and base.endswith(".log") and base[4:-4].isdigit():
            proc["pid"] = int(base[4:-4])
    return proc


class Elf:
    """Function symbols of an ELF64 little-endian file, by virtual address."""

    def __init__(self, path):
        self.loads, self.syms = [], []
        with open(path, "rb") as f:
            d = f.read()
        if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
            raise ValueError("not ELF64 LE")
        phoff, shoff = struct.unpack_from("<QQ", d, 0x20)
        phentsize, phnum, shentsize, shnum = struct.unpack_from("<HHHH", d, 0x36)
        for i in range(phnum):
            ptype, _, poff, pvaddr, _, pfilesz = struct.unpack_from("<IIQQQQ", d, phoff + i * phentsize)
            if ptype == 1:
                self.loads.append((poff, pvaddr, pfilesz))
        sections = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize) for i in range(shnum)]
        for s in sections:
            if s[1] not in (2, 11):  # SYMTAB, DYNSYM
                continue
            strtab = sections[s[6]]
            for off in range(s[4], s[4] + s[5], 24):
                name, info, _, _, value, size = struct.unpack_from("<IBBHQQ", d, off)
                if info & 0xF != 2 or not value:  # FUNC
                    continue
                end = d.index(b"\0", strtab[4] + name)
                self.syms.append((value, size, d[strtab[4] + name:end].decode("ascii", "replace")))
        self.syms.sort()

    def vaddr(self, file_offset):
        for poff, pvaddr, size in self.loads:
            if poff <= file_offset < poff + size:
                return file_offset - poff + pvaddr
        return None

    def symbol(self, file_offset):
        va = self.vaddr(file_offset)
        if va is None:
            return None
        best = None
        for value, size, name in self.syms:
            if value > va:
                break
            if va < value + max(size, 1):
                best = "%s+0x%x" % (name, va - value)
        return best


def readable(path):
    """The path, then its tails (/a/b/rootfs/usr/x -> /b/rootfs/usr/x ... /x): the maps of a process under proot
    name files by their host paths."""
    parts = path.strip("/").split("/")
    for i in range(len(parts)):
        candidate = "/" + "/".join(parts[i:])
        if os.path.isfile(candidate):
            yield candidate


class Where:
    def __init__(self, maps, symbols):
        self.maps, self.symbols, self.elves = maps, symbols, {}

    def __call__(self, addr):
        """(file basename, 'file+0xoff [symbol]')"""
        for lo, hi, offset, path in self.maps:
            if lo <= addr < hi:
                fo = addr - lo + offset
                text = "%s+0x%x" % (os.path.basename(path) or "?", fo)
                if self.symbols and path.startswith("/"):
                    if path not in self.elves:
                        self.elves[path] = None
                        for local in readable(path):
                            try:
                                self.elves[path] = Elf(local)
                                break
                            except (OSError, ValueError, struct.error, IndexError):
                                continue
                    sym = self.elves[path] and self.elves[path].symbol(fo)
                    if sym:
                        text += " " + sym
                return os.path.basename(path), text
        return "?", "0x%x" % addr


def window(proc, last_s):
    snaps = proc["snaps"]
    if len(snaps) < 2:
        return None
    end = snaps[-1]
    start = snaps[0]
    for s in snaps:
        if end[0] - s[0] <= last_s * 1e9:
            start = s
            break
    if start is end:
        start = snaps[-2]
    return start, end


def rates(proc, last_s):
    w = window(proc, last_s)
    if not w:
        return None, {}, {}
    (t0, c0, _), (t1, c1, comms) = w
    dt = (t1 - t0) / 1e9
    out = {}
    for key, n in c1.items():
        delta = n - c0.get(key, 0)
        if delta > 0:
            out[key] = delta / dt
    return dt, out, comms


def bench_numbers(paths):
    runs = {}
    for p in paths:
        with open(p, errors="replace") as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 6 and parts[0] == "qpcbench" and parts[2] == "min" and parts[4] == "median":
                    runs.setdefault(parts[1], []).append(float(parts[5]))
    return {k: sorted(v)[len(v) // 2] for k, v in runs.items()}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--match", default="Duckov.exe")
    ap.add_argument("--pid", type=int)
    ap.add_argument("--last", type=float, default=120)
    ap.add_argument("--bench", action="append", default=[])
    ap.add_argument("--fps", type=float)
    ap.add_argument("--bench-ghz", type=float)
    ap.add_argument("--main-ghz", type=float)
    ap.add_argument("--no-symbols", action="store_true")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)

    files = []
    for x in a.logs:
        files += sorted(glob.glob(os.path.join(x, "qpc-*.log"))) if os.path.isdir(x) else [x]
    procs = [parse_log(f) for f in files]
    if a.pid is not None:
        procs = [p for p in procs if p["pid"] == a.pid]
    else:
        procs = [p for p in procs if a.match in p["cmd"]] or procs
    procs = [p for p in procs if len(p["snaps"]) >= 2]
    if not procs:
        print("qpc-report: no process with two snapshots or more (match %r)" % a.match, file=sys.stderr)
        return 2
    proc = max(procs, key=lambda p: sum(p["snaps"][-1][1].values()))
    dt, r, comms = rates(proc, a.last)
    where = Where(proc["maps"], not a.no_symbols)
    pid = proc["pid"]

    rows = []
    for (tid, clock, addr), rate in r.items():
        lib, text = where(addr)
        rows.append({"tid": tid, "comm": comms.get(tid, "?"), "clock": CLOCK_NAMES.get(clock, str(clock)), "site": text,
                     "lib": lib, "perS": round(rate, 1), "qpc": lib.startswith("ntdll.so") and clock == 4})
    rows.sort(key=lambda x: -x["perS"])
    main_rows = [x for x in rows if x["tid"] == pid]
    n_main = sum(x["perS"] for x in main_rows if x["qpc"])
    per_thread = {}
    for x in rows:
        if x["qpc"]:
            per_thread[(x["tid"], x["comm"])] = per_thread.get((x["tid"], x["comm"]), 0) + x["perS"]

    result = {"pid": pid, "cmd": proc["cmd"][:200], "windowS": round(dt, 1), "snapshots": len(proc["snaps"]),
              "droppedThreads": proc["dropped"], "mainQpcPerS": round(n_main, 1),
              "mainQpcPerFrame": round(n_main / a.fps, 2) if a.fps else None,
              "mainSites": main_rows, "qpcByThread": [{"tid": t, "comm": c, "perS": round(v, 1)}
                                                      for (t, c), v in sorted(per_thread.items(), key=lambda kv: -kv[1])[:10]]}
    b = bench_numbers(a.bench)
    if b.get("qpc") is not None:
        scale = a.bench_ghz / a.main_ghz if a.bench_ghz and a.main_ghz else 1.0
        t = b["qpc"] * scale
        empty = b.get("empty", 0.0)
        floor = (b.get("tick64", 0.0) - empty + b.get("rdtsc", 0.0) - empty) * scale
        share = n_main * t / 1e9
        result["bench"] = {k: v for k, v in sorted(b.items())}
        result["qpcNs"] = round(t, 1)
        result["floorNs"] = round(floor, 1)
        result["mainShare"] = round(share, 5)
        result["savingShare"] = round(n_main * max(t - floor, 0.0) / 1e9, 5)
        result["clockScale"] = round(scale, 3)
        result["gate"] = "PASS" if share >= GATE else "below"

    if a.json:
        print(json.dumps(result, indent=1))
        return 0
    print("process %d: %s" % (pid, result["cmd"]))
    print("window %.1f s over %d snapshots%s" % (dt, len(proc["snaps"]),
                                                ", %d threads not counted (table full)" % proc["dropped"] if proc["dropped"] else ""))
    print("main thread (tid %d) QueryPerformanceCounter: %.0f calls/s%s" % (
        pid, n_main, ", %.1f per frame at %.1f fps" % (n_main / a.fps, a.fps) if a.fps else ""))
    print("main thread clock_gettime sites:")
    for x in main_rows[:12]:
        print("  %10.1f/s  %-14s %s%s" % (x["perS"], x["clock"], x["site"], "  <- QPC" if x["qpc"] else ""))
    print("QPC by thread (ntdll.so, MONOTONIC_RAW):")
    for x in result["qpcByThread"]:
        print("  %10.1f/s  %d %s" % (x["perS"], x["tid"], x["comm"]))
    if "qpcNs" in result:
        print("bench (ns/call, median of medians): " + ", ".join("%s %.1f" % kv for kv in result["bench"].items()))
        if result["clockScale"] != 1.0:
            print("scaled to the main thread's clock by %.3f" % result["clockScale"])
        print("QPC %.1f ns/call -> main thread share %.3f %% (gate %.0f %%: %s); a counter read in user space would still"
              " cost ~%.1f ns: saving ~%.3f %%" % (result["qpcNs"], 100 * result["mainShare"], 100 * GATE, result["gate"],
                                                    result["floorNs"], 100 * result["savingShare"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
