/*
 * qpcprobe: how often each thread of a Wine process calls clock_gettime, by clock and call site, without
 * a rebuilt Wine. LD_PRELOADed into the game (games[<appid>].LD_PRELOAD of game-environment.json).
 *
 * On ARM64 Proton, QueryPerformanceCounter goes kernel32 -> ntdll RtlQueryPerformanceCounter (PE) ->
 * the NtQueryPerformanceCounter syscall -> Wine's unix ntdll.so, whose monotonic_counter() is
 * clock_gettime(CLOCK_MONOTONIC_RAW): in Valve's wine (experimental_11.0) the only CLOCK_MONOTONIC_RAW
 * call in ntdll.so. So the calls from ntdll.so with clock 4 of the game's main thread (tid == pid)
 * are its QueryPerformanceCounter calls. glibc answers clock_gettime from the vDSO, so no syscall
 * tracing sees them, and FEX's stats count no syscalls for ARM64EC code.
 *
 * Off (a plain pass-through) unless QPCPROBE_DIR is set. Then every call adds one to a counter for
 * (thread, return address, clock) - a lock-free table keyed by pthread_self(), no thread-local storage
 * (static TLS is short when libhybris-common is preloaded too) - and every QPCPROBE_INTERVAL_MS
 * (default 5000) the first thread past the deadline appends a snapshot to
 * $QPCPROBE_DIR/qpc-<pid>.log, if the process's command line contains QPCPROBE_MATCH (default: any):
 *   P <pid> <cmdline, NULs as spaces>                     once
 *   T <CLOCK_MONOTONIC_RAW ns> <threads> <sites> <dropped>   per snapshot (written by a thread that reads
 *                                                       that clock: a process that never does writes nothing)
 *   S <tid> <comm> <clock> <return address hex> <count>  per site, counts since the start
 *   D <tid> <calls>                                       calls of a thread from more than 16 sites
 *   M <maps line>                                         the mappings holding a site (new ones)
 * The analysis (qpc-report.py) turns return addresses into file+offset (and a symbol, from the guest's
 * qpc-guest.sh symbols) and counts into rates. The probe costs a few ns per call on top of the call.
 * Build: build.sh (old glibc symbol versions: the guest is Debian bullseye).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#if defined(__aarch64__)
__asm__(".symver dlsym, dlsym@GLIBC_2.17");
#elif defined(__x86_64__)
__asm__(".symver dlsym, dlsym@GLIBC_2.2.5");
#endif

#define EXPORT __attribute__((visibility("default")))
#define THREADS 512
#define SITES 16

typedef struct {
    uintptr_t address; /* 0: free */
    uint32_t clock;
    uint64_t count;
} Site;

typedef struct {
    uintptr_t key; /* pthread_self(); 0: free */
    int tid;
    uint32_t dropped; /* calls from more than SITES sites */
    Site sites[SITES];
} Thread;

typedef int (*ClockGettime)(clockid_t, struct timespec*);

static ClockGettime real;
static int enabled;
static char dir[256];
static char match[128];
static uint64_t intervalNs = 5000000000ull;
static uint64_t nextDump;      /* atomic */
static int dumping;            /* atomic */
static uint32_t droppedThreads; /* atomic */
static Thread threads[THREADS];
static int headerWritten, matchState; /* matchState 0 unknown, 1 yes, -1 no */
static uintptr_t mapped[64];   /* starts of the mappings already written */
static int mappedCount;

static uint64_t ns(const struct timespec* ts) { return (uint64_t)ts->tv_sec * 1000000000ull + (uint64_t)ts->tv_nsec; }

__attribute__((constructor)) static void init(void) {
    real = (ClockGettime)dlsym(RTLD_NEXT, "clock_gettime");
    const char* d = getenv("QPCPROBE_DIR");
    if (!real || !d || !*d || strlen(d) >= sizeof(dir)) return;
    strcpy(dir, d);
    const char* m = getenv("QPCPROBE_MATCH");
    if (m && strlen(m) < sizeof(match)) strcpy(match, m);
    const char* i = getenv("QPCPROBE_INTERVAL_MS");
    if (i && *i) {
        uint64_t v = 0;
        for (; *i >= '0' && *i <= '9'; i++) v = v * 10 + (uint64_t)(*i - '0');
        if (v >= 100) intervalNs = v * 1000000ull;
    }
    struct timespec now;
    real(CLOCK_MONOTONIC_RAW, &now);
    nextDump = ns(&now) + intervalNs;
    enabled = 1;
}

static Thread* self(void) {
    uintptr_t key = (uintptr_t)pthread_self();
    uint32_t h = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 55) & (THREADS - 1);
    for (uint32_t probe = 0; probe < THREADS; probe++) {
        Thread* t = &threads[(h + probe) & (THREADS - 1)];
        uintptr_t k = __atomic_load_n(&t->key, __ATOMIC_ACQUIRE);
        if (k == key) return t;
        if (k) continue;
        uintptr_t expected = 0;
        if (__atomic_compare_exchange_n(&t->key, &expected, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&t->tid, (int)syscall(SYS_gettid), __ATOMIC_RELEASE);
            return t;
        }
        if (expected == key) return t;
    }
    __atomic_add_fetch(&droppedThreads, 1, __ATOMIC_RELAXED);
    return NULL;
}

/* Only its own thread adds sites to a Thread: no compare-and-swap needed, the dump reads them racily. */
static void count(uintptr_t address, clockid_t clock) {
    Thread* t = self();
    if (!t) return;
    for (int i = 0; i < SITES; i++) {
        Site* s = &t->sites[i];
        uintptr_t a = __atomic_load_n(&s->address, __ATOMIC_RELAXED);
        if (a == address && s->clock == (uint32_t)clock) {
            __atomic_store_n(&s->count, s->count + 1, __ATOMIC_RELAXED);
            return;
        }
        if (!a) {
            s->clock = (uint32_t)clock;
            __atomic_store_n(&s->count, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&s->address, address, __ATOMIC_RELEASE);
            return;
        }
    }
    __atomic_store_n(&t->dropped, t->dropped + 1, __ATOMIC_RELAXED);
}

/* ---- the snapshot: plain write(2), no stdio and no loader calls (the caller may hold any lock) ---- */

typedef struct {
    int fd;
    char buf[8192];
    size_t used;
} Out;

static void flush(Out* o) {
    if (o->used && write(o->fd, o->buf, o->used) < 0) {}
    o->used = 0;
}

static void put(Out* o, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void put(Out* o, const char* fmt, ...) {
    if (o->used > sizeof(o->buf) - 1024) flush(o);
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->used, sizeof(o->buf) - o->used, fmt, ap);
    va_end(ap);
    if (n > 0) o->used += (size_t)n < sizeof(o->buf) - o->used ? (size_t)n : sizeof(o->buf) - o->used - 1;
}

static ssize_t readAll(const char* path, char* buf, size_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t got = 0;
    ssize_t n;
    while (got < size - 1 && (n = read(fd, buf + got, size - 1 - got)) > 0) got += (size_t)n;
    close(fd);
    buf[got] = 0;
    return (ssize_t)got;
}

static int matches(void) {
    if (matchState) return matchState > 0;
    if (!match[0]) return (matchState = 1) > 0;
    char cmd[4096];
    ssize_t n = readAll("/proc/self/cmdline", cmd, sizeof(cmd));
    for (ssize_t i = 0; i < n; i++)
        if (!cmd[i]) cmd[i] = ' ';
    /* Wine rewrites its command line once the process runs: undecided until it matches or 30 s pass */
    if (n > 0 && strstr(cmd, match)) return (matchState = 1) > 0;
    return 0;
}

static uintptr_t hex(const char** p) {
    uintptr_t v = 0;
    for (;; (*p)++) {
        char c = **p;
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) return v;
        v = v * 16 + (uintptr_t)d;
    }
}

static void mapsFor(Out* o, const uintptr_t* addresses, int count) {
    static char maps[1 << 20];
    if (readAll("/proc/self/maps", maps, sizeof(maps)) <= 0) return;
    for (char* line = maps; *line;) {
        char* end = strchr(line, '\n');
        if (end) *end = 0;
        const char* c = line;
        uintptr_t lo = hex(&c), hi = 0;
        if (*c == '-' && (c++, hi = hex(&c)) > lo) {
            for (int i = 0; i < count; i++) {
                if (addresses[i] < lo || addresses[i] >= hi) continue;
                int seen = 0;
                for (int m = 0; m < mappedCount && !seen; m++) seen = mapped[m] == lo;
                if (!seen) {
                    if (mappedCount < (int)(sizeof(mapped) / sizeof(mapped[0]))) mapped[mappedCount++] = lo;
                    put(o, "M %s\n", line);
                }
                break;
            }
        }
        if (!end) break;
        line = end + 1;
    }
}

static void dump(uint64_t now, uint64_t startNs) {
    if (!matches()) {
        if (now - startNs > 30000000000ull + intervalNs) matchState = -1;
        return;
    }
    char path[300];
    snprintf(path, sizeof(path), "%s/qpc-%d.log", dir, (int)getpid());
    static Out o;
    o.fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    o.used = 0;
    if (o.fd < 0) return;
    if (!headerWritten) {
        char cmd[1024];
        ssize_t n = readAll("/proc/self/cmdline", cmd, sizeof(cmd));
        for (ssize_t i = 0; i < n; i++)
            if (!cmd[i] || cmd[i] == '\n') cmd[i] = ' ';
        put(&o, "P %d %s\n", (int)getpid(), n > 0 ? cmd : "?");
        headerWritten = 1;
    }
    int nThreads = 0, nSites = 0;
    static uintptr_t addresses[THREADS * SITES];
    for (int i = 0; i < THREADS; i++) {
        Thread* t = &threads[i];
        if (!__atomic_load_n(&t->key, __ATOMIC_ACQUIRE)) continue;
        nThreads++;
        for (int s = 0; s < SITES; s++)
            if (__atomic_load_n(&t->sites[s].address, __ATOMIC_ACQUIRE)) addresses[nSites++] = t->sites[s].address;
    }
    put(&o, "T %llu %d %d %u\n", (unsigned long long)now, nThreads, nSites, __atomic_load_n(&droppedThreads, __ATOMIC_RELAXED));
    for (int i = 0; i < THREADS; i++) {
        Thread* t = &threads[i];
        if (!__atomic_load_n(&t->key, __ATOMIC_ACQUIRE)) continue;
        int tid = __atomic_load_n(&t->tid, __ATOMIC_ACQUIRE);
        char comm[64] = "?", p[64];
        snprintf(p, sizeof(p), "/proc/self/task/%d/comm", tid);
        if (readAll(p, comm, sizeof(comm)) > 0) {
            for (char* c = comm; *c; c++)
                if (*c == '\n' || *c == ' ') *c = *c == ' ' ? '_' : 0;
        }
        for (int s = 0; s < SITES; s++) {
            Site* site = &t->sites[s];
            uintptr_t a = __atomic_load_n(&site->address, __ATOMIC_ACQUIRE);
            if (!a) continue;
            put(&o, "S %d %s %u %lx %llu\n", tid, comm[0] ? comm : "?", site->clock, (unsigned long)a,
                (unsigned long long)__atomic_load_n(&site->count, __ATOMIC_RELAXED));
        }
        if (t->dropped) put(&o, "D %d %u\n", tid, t->dropped);
    }
    mapsFor(&o, addresses, nSites);
    flush(&o);
    close(o.fd);
}

EXPORT int clock_gettime(clockid_t clock, struct timespec* ts) {
    if (!real) real = (ClockGettime)dlsym(RTLD_NEXT, "clock_gettime");
    int r = real(clock, ts);
    if (!enabled) return r;
    count((uintptr_t)__builtin_return_address(0), clock);
    static uint64_t startNs;
    if (r == 0 && clock == CLOCK_MONOTONIC_RAW) {
        /* the deadline from the reading itself (no extra clock call): only CLOCK_MONOTONIC_RAW ones, the
         * clocks run apart (NTP slews CLOCK_MONOTONIC), so a process that never reads it never writes */
        uint64_t now = ns(ts);
        if (!startNs) startNs = now;
        if (now >= __atomic_load_n(&nextDump, __ATOMIC_RELAXED) && !__atomic_exchange_n(&dumping, 1, __ATOMIC_ACQUIRE)) {
            if (now >= nextDump) {
                nextDump = now + intervalNs;
                if (matchState >= 0) dump(now, startNs);
            }
            __atomic_store_n(&dumping, 0, __ATOMIC_RELEASE);
        }
    }
    return r;
}
