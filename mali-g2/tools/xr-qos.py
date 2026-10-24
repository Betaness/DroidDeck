#!/usr/bin/env python3
"""HyperOS thread QoS and where the game's threads run, on the Xring O3 (no root).

usage: xr-qos.py get TID...
       xr-qos.py set MODE TID...      MODE: vip (6, VIP), rt (9, RT), off, or LEVEL:TYPE[:rtg]
       xr-qos.py pin CPUS TID...      CPUS like 8-9 or 0-9 (sched_setaffinity of single threads)
       xr-qos.py watch SECONDS [--fps N] TID...

Run it as the app's UID, e.g. in the session: droiddeckctl guest -- python3 xr-qos.py ...
/dev/xr_fbt takes QoS requests for threads of the caller's own UID only (EINVAL otherwise), so adb's
shell cannot do this. 'watch' samples every 100 ms: per thread its busy share, CPU time per second
(per frame with --fps), the share of samples on each cluster and its policy; per cluster the clock
(policyN/scaling_cur_freq) while one of the threads was last seen on it, and overall.
"""
import fcntl, os, struct, sys, time

SET, GET = 0x40087807, 0xC0087809
MODES = {"vip": (6, 2), "rt": (9, 1), "off": (0, 0)}
HZ = os.sysconf("SC_CLK_TCK")


def value(mode):
    if mode in MODES:
        level, kind = MODES[mode]
        return level | kind << 4
    parts = mode.split(":")
    return int(parts[0]) | int(parts[1]) << 4 | (1 << 6 if parts[2:] == ["rtg"] else 0)


def stat(tid):
    with open(f"/proc/{tid}/stat") as f:
        s = f.read()
    rest = s[s.rindex(")") + 2:].split(" ")
    return s[s.index("(") + 1:s.rindex(")")], int(rest[11]) + int(rest[12]), int(rest[36]), int(rest[38])


def clusters():
    out = []
    base = "/sys/devices/system/cpu/cpufreq"
    for p in sorted(os.listdir(base), key=lambda n: int(n[6:])):
        with open(f"{base}/{p}/related_cpus") as f:
            out.append((p, [int(c) for c in f.read().split()]))
    return out


def khz(policy):
    with open(f"/sys/devices/system/cpu/cpufreq/{policy}/scaling_cur_freq") as f:
        return int(f.read())


def ioctl(fd, cmd, tid, val=0):
    buf = bytearray(struct.pack("<II", val, tid))
    fcntl.ioctl(fd, cmd, buf, True)
    return struct.unpack("<II", buf)[0]


def main():
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd in ("get", "set"):
        fd = os.open("/dev/xr_fbt", os.O_RDWR)
        tids = [int(t) for t in (args if cmd == "get" else args[1:])]
        for tid in tids:
            try:
                if cmd == "set":
                    ioctl(fd, SET, tid, value(args[0]))
                print(f"{tid} {stat(tid)[0]}: qos 0x{ioctl(fd, GET, tid):x}, policy {stat(tid)[3]}")
            except OSError as e:
                print(f"{tid}: {e}")
    elif cmd == "pin":
        lo, _, hi = args[0].partition("-")
        cpus = set(range(int(lo), int(hi or lo) + 1))
        for tid in map(int, args[1:]):
            os.sched_setaffinity(tid, cpus)
            print(f"{tid} {stat(tid)[0]}: cpus {sorted(os.sched_getaffinity(tid))}")
    elif cmd == "watch":
        secs = float(args[0])
        fps = None
        if args[1:2] == ["--fps"]:
            fps = float(args[2])
            args = args[2:]
        tids = [int(t) for t in args[1:]]
        cl = clusters()
        of = {cpu: p for p, cpus in cl for cpu in cpus}
        first = {t: stat(t) for t in tids}
        where = {t: {} for t in tids}
        clock = {p: [] for p, _ in cl}
        under = {p: [] for p, _ in cl}
        start = time.monotonic()
        n = 0
        while time.monotonic() - start < secs:
            freqs = {p: khz(p) for p, _ in cl}
            for p, f in freqs.items():
                clock[p].append(f)
            for t in tids:
                try:
                    p = of[stat(t)[2]]
                except (OSError, KeyError):
                    continue
                where[t][p] = where[t].get(p, 0) + 1
                under[p].append(freqs[p])
            n += 1
            time.sleep(0.1)
        took = time.monotonic() - start
        for t in tids:
            try:
                name, ticks, _, policy = stat(t)
            except OSError:
                print(f"{t}: gone")
                continue
            ms = (ticks - first[t][1]) * 1000 / HZ / took
            res = ", ".join(f"{p} {100 * c // max(n, 1)}%" for p, c in sorted(where[t].items()))
            print(f"{t} {name}: busy {ms / 10:.0f}%, {ms:.0f} ms/s" + (f" = {ms / fps:.2f} ms/frame" if fps else "") +
                  f", on {res}, policy {policy}")
        for p, cpus in cl:
            mean = lambda xs: sum(xs) / len(xs) / 1e6 if xs else 0
            print(f"{p} (cpus {cpus[0]}-{cpus[-1]}): {mean(clock[p]):.2f} GHz mean, max {max(clock[p]) / 1e6:.2f}; "
                  f"{mean(under[p]):.2f} GHz while a watched thread was on it ({len(under[p])} samples)")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
