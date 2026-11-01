/*
 * qpcbench.exe (x86_64 Windows, mingw): what one QueryPerformanceCounter costs x86_64 code under the
 * Wine / FEX that runs the game, beside what a direct counter read and the cheapest kernel32 call cost.
 * Run with the game's own Wine and environment (qpc-guest.sh bench). Per kind, REPEATS loops of N calls,
 * timed with QueryPerformanceCounter around each loop; prints the fastest and the median ns per call:
 *   empty      the loop alone (volatile store)
 *   rdtsc      RDTSC from x86_64 code: FEX reads the CPU's counter (cntvct_el0) for it, about what a
 *              QPC that reads the counter in user space would cost beyond the call itself
 *   tick64     GetTickCount64: a kernel32 call that only reads KUSER_SHARED_DATA (the x86_64 -> ARM64EC
 *              call into Wine's PE code, without a syscall)
 *   qpc        QueryPerformanceCounter: kernel32 -> ntdll RtlQueryPerformanceCounter -> the
 *              NtQueryPerformanceCounter syscall into the unix side (clock_gettime)
 *   ntqpc      NtQueryPerformanceCounter called directly
 * Output lines: "qpcbench <kind> min <ns> median <ns> calls <N>", then "qpcbench freq <QPC frequency>"
 * and "qpcbench cpu <processor number at the start> <at the end>".
 *   qpcbench.exe [N (default 1000000)] [REPEATS (default 7)]
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <x86intrin.h>

typedef LONG(WINAPI* NtQpc)(LARGE_INTEGER*, LARGE_INTEGER*);

static volatile LONGLONG sink;
static LARGE_INTEGER freq;

static double nsPer(LONGLONG ticks, long n) { return (double)ticks * 1e9 / (double)freq.QuadPart / (double)n; }

static int cmp(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

enum { EMPTY, RDTSC, TICK64, QPC, NTQPC, KINDS };
static const char* const names[KINDS] = {"empty", "rdtsc", "tick64", "qpc", "ntqpc"};

static NtQpc ntQpc;

static double loop(int kind, long n) {
    LARGE_INTEGER a, b, x;
    QueryPerformanceCounter(&a);
    switch (kind) {
    case EMPTY:
        for (long i = 0; i < n; i++) sink = i;
        break;
    case RDTSC:
        for (long i = 0; i < n; i++) sink = (LONGLONG)__rdtsc();
        break;
    case TICK64:
        for (long i = 0; i < n; i++) sink = (LONGLONG)GetTickCount64();
        break;
    case QPC:
        for (long i = 0; i < n; i++) {
            QueryPerformanceCounter(&x);
            sink = x.QuadPart;
        }
        break;
    case NTQPC:
        for (long i = 0; i < n; i++) {
            ntQpc(&x, NULL);
            sink = x.QuadPart;
        }
        break;
    }
    QueryPerformanceCounter(&b);
    return nsPer(b.QuadPart - a.QuadPart, n);
}

int main(int argc, char** argv) {
    long n = argc > 1 ? atol(argv[1]) : 1000000;
    int repeats = argc > 2 ? atoi(argv[2]) : 7;
    if (n < 1000) n = 1000;
    if (repeats < 1 || repeats > 99) repeats = 7;
    QueryPerformanceFrequency(&freq);
    ntQpc = (NtQpc)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryPerformanceCounter");
    DWORD cpu0 = GetCurrentProcessorNumber();
    double results[KINDS][99];
    /* warm up (FEX translates each loop the first time) */
    for (int k = 0; k < KINDS; k++)
        if (k != NTQPC || ntQpc) loop(k, n / 10);
    /* interleaved, so a clock change hits every kind alike */
    for (int r = 0; r < repeats; r++)
        for (int k = 0; k < KINDS; k++) results[k][r] = (k == NTQPC && !ntQpc) ? 0 : loop(k, n);
    for (int k = 0; k < KINDS; k++) {
        if (k == NTQPC && !ntQpc) continue;
        qsort(results[k], repeats, sizeof(double), cmp);
        printf("qpcbench %s min %.1f median %.1f calls %ld\n", names[k], results[k][0], results[k][repeats / 2], n);
    }
    printf("qpcbench freq %lld\n", (long long)freq.QuadPart);
    printf("qpcbench cpu %lu %lu\n", (unsigned long)cpu0, (unsigned long)GetCurrentProcessorNumber());
    fflush(stdout);
    return 0;
}
