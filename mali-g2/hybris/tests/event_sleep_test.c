/*
 * DROIDDECK_HYBRIS_EVENT_SLEEP_US, _PAUSE and _LOG_S (mali_fixes.c) through this ICD's entry points
 * and a fake driver (tests/fake_icd.c as libfake_mali_icd.so, DROIDDECK_HYBRIS_NEXT_ICD), no GPU and
 * no libhybris. Each setting runs in a process of its own (the variables are read once), which checks
 * the answers, the driver's calls and its own log (stderr, a file). A call of the wrapper that slept
 * asks the driver twice, so the driver's call count tells the sleeps.
 *   sh tests/guards_test.sh        (builds and runs this with the ICD built from src/)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "fake_icd.h"

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* name);

static int failures, checks;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);    \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
        }                                                                      \
    } while (0)

#define R VK_EVENT_RESET
#define S VK_EVENT_SET
#define LOST VK_ERROR_DEVICE_LOST

static FakeIcd* fk;
static PFN_vkVoidFunction (*fakeProc)(const char* name);
static VkInstance instance;
static VkDevice device;
static PFN_vkGetDeviceProcAddr gdpa;
static VkQueue queue;
static PFN_vkGetEventStatus getEventStatus;
static const VkEvent eventA = (VkEvent)(uintptr_t)0x100, eventB = (VkEvent)(uintptr_t)0x200;
static int sleepsSeen; /* answers for which the driver was asked twice */
static __thread uint64_t lastReturnNs; /* when this thread's last call returned */

#define INST(name) ((PFN_vk##name)vk_icdGetInstanceProcAddr(instance, "vk" #name))
#define DEV(name) ((PFN_vk##name)gdpa(device, "vk" #name))

static void script(FakeScript* s, VkResult otherwise, int count, ...) {
    memset(s, 0, sizeof(*s));
    s->otherwise = otherwise;
    va_list results;
    va_start(results, count);
    for (int i = 0; i < count; i++) s->results[s->count++] = va_arg(results, VkResult);
    va_end(results);
}

static uint64_t nowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Whether this thread's last call returned well within the wrapper's 1 ms: where the steps in between
 * (threads, files) could take longer under load, the check of a repeated reset is made only then. */
static bool recent(void) {
    return nowNs() - lastReturnNs < 500000ull;
}

static void sleepMs(long ms) {
    nanosleep(&(struct timespec){ms / 1000, ms % 1000 * 1000000L}, NULL);
}

static void setUp(void) {
    void* lib = dlopen(getenv("DROIDDECK_HYBRIS_NEXT_ICD"), RTLD_NOW | RTLD_LOCAL);
    fk = lib ? dlsym(lib, "fake_icd") : NULL;
    fakeProc = lib ? (PFN_vkVoidFunction(*)(const char*))dlsym(lib, "fake_icd_device_proc") : NULL;
    if (!fk || !fakeProc) {
        fprintf(stderr, "FAIL: no fake driver (DROIDDECK_HYBRIS_NEXT_ICD): %s\n", dlerror());
        exit(1);
    }
    VkInstanceCreateInfo instanceInfo = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    CHECK(createInstance && createInstance(&instanceInfo, NULL, &instance) == VK_SUCCESS, "instance");
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t count = 1;
    CHECK(INST(EnumeratePhysicalDevices)(instance, &count, &physical) == VK_SUCCESS && physical, "physical device");
    float priority = 0;
    VkDeviceQueueCreateInfo queueInfo = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    CHECK(INST(CreateDevice)(physical, &deviceInfo, NULL, &device) == VK_SUCCESS, "device");
    gdpa = (PFN_vkGetDeviceProcAddr)vk_icdGetInstanceProcAddr(instance, "vkGetDeviceProcAddr");
    DEV(GetDeviceQueue)(device, 0, 0, &queue);
    CHECK(queue != VK_NULL_HANDLE, "queue");
}

/* The wrapper or the driver's own function, looked up as the loader does (with the device). */
static bool lookUp(void) {
    getEventStatus = DEV(GetEventStatus);
    CHECK(INST(GetEventStatus) == (PFN_vkGetEventStatus)fakeProc("vkGetEventStatus"), "without a device: the driver's own");
    return getEventStatus != (PFN_vkGetEventStatus)fakeProc("vkGetEventStatus");
}

static void tearDown(void) {
    DEV(DestroyDevice)(device, NULL);
    INST(DestroyInstance)(instance, NULL);
}

static void present(void) {
    VkPresentInfoKHR info = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    CHECK(DEV(QueuePresentKHR)(queue, &info) == VK_SUCCESS, "a present of no swapchain");
}

/* One call: the answer is the driver's last one for the event; `calls` driver calls (-1: 1 or 2). */
static void expect(const char* what, VkEvent event, VkResult want, int calls) {
    int before = fk->events.calls;
    VkResult got = getEventStatus(device, event);
    lastReturnNs = nowNs();
    int made = fk->events.calls - before;
    if (made == 2) sleepsSeen++;
    CHECK(got == fk->lastEventResult && fk->lastEvent == event, "%s: answer %d, the driver's last %d", what, got, fk->lastEventResult);
    CHECK(got == want && (calls < 0 ? made == 1 || made == 2 : made == calls), "%s: answer %d (want %d), %d driver calls (want %d)", what,
          got, want, made, calls);
}

/* ---- the process's own log ---- */

static char logText[65536];

static const char* ownLog(void) {
    ssize_t n = pread(2, logText, sizeof(logText) - 1, 0);
    logText[n > 0 ? n : 0] = 0;
    return logText;
}

static int countLines(const char* log, const char* prefix) {
    int n = 0;
    for (const char* line = log; line && *line; line = strchr(line, '\n'), line = line ? line + 1 : NULL)
        if (!strncmp(line, prefix, strlen(prefix))) n++;
    return n;
}

typedef struct {
    unsigned us;
    int paused;
    unsigned long long calls, set, sleeps, setAfterSleep;
    double sleptMs;
} Counts;

/* The last line "hybris-icd: event sleep: <what>, pid ...". */
static bool countsLine(const char* log, const char* what, Counts* c) {
    char prefix[96];
    snprintf(prefix, sizeof(prefix), "hybris-icd: event sleep: %s, pid ", what);
    const char* found = NULL;
    for (const char* line = log; line && *line; line = strchr(line, '\n'), line = line ? line + 1 : NULL)
        if (!strncmp(line, prefix, strlen(prefix))) found = line;
    if (!found) return false;
    int pid;
    unsigned long long seconds;
    return sscanf(found + strlen(prefix) - 4,
                  "pid %d, seconds %llu, us %u, paused %d, calls %llu, set %llu, sleeps %llu, set-after-sleep %llu, slept %lf ms", &pid,
                  &seconds, &c->us, &c->paused, &c->calls, &c->set, &c->sleeps, &c->setAfterSleep, &c->sleptMs) == 9 &&
           pid == getpid();
}

static void checkCounts(const char* log, const char* what, unsigned us, unsigned long long calls, unsigned long long set,
                        unsigned long long sleeps, unsigned long long setAfterSleep) {
    Counts c;
    bool found = countsLine(log, what, &c);
    CHECK(found, "a line \"event sleep: %s, ...\"", what);
    if (!found) return;
    CHECK(c.us == us && c.calls == calls && c.set == set && c.sleeps == sleeps && c.setAfterSleep == setAfterSleep,
          "%s: us %u calls %llu set %llu sleeps %llu set-after-sleep %llu (want %u %llu %llu %llu %llu)", what, c.us, c.calls, c.set,
          c.sleeps, c.setAfterSleep, us, calls, set, sleeps, setAfterSleep);
    CHECK(c.sleptMs + 0.05 >= (double)sleeps * us / 1000.0, "%s: slept %.1f ms for %llu sleeps of %u us", what, c.sleptMs, sleeps, us);
}

/* ---- the settings ---- */

/* Not wrapped: the driver's function and answers, no line (an "ignored" one for a value that is not one). */
static void runOff(void) {
    setUp();
    CHECK(!lookUp(), "vkGetEventStatus is the driver's");
    script(&fk->events, S, 5, R, R, R, R, R);
    for (int i = 0; i < 5; i++) expect("off: reset", eventA, R, 1);
    expect("off: set", eventA, S, 1);
    present();
    tearDown();
    const char* log = ownLog();
    const char* value = getenv("DROIDDECK_HYBRIS_EVENT_SLEEP_US");
    CHECK(!strstr(log, "event sleep"), "no event sleep line");
    if (value && *value && strcmp(value, "0")) {
        char ignored[128];
        snprintf(ignored, sizeof(ignored), "hybris-icd: DROIDDECK_HYBRIS_EVENT_SLEEP_US=%s ignored (microseconds, 0 to 1000)", value);
        CHECK(countLines(log, ignored) == 1, "one line \"%s\"", ignored);
    } else {
        CHECK(!strstr(log, "EVENT_SLEEP"), "nothing about the variable");
    }
}

/* One reset on eventB: a first one for this thread whatever the caller's last call was. */
static void* otherThreadOnce(void* arg) {
    expect("thread: its first reset", eventB, R, 1);
    return NULL;
}

static void* otherThread(void* arg) {
    expect("thread: its first reset", eventB, R, 1);
    expect("thread: its repeated reset", eventB, R, 2);
    return NULL;
}

static void runOn(void) {
    setUp();
    CHECK(lookUp(), "vkGetEventStatus wrapped");

    /* a poll loop until set: the first reset at once, each later one after a sleep and a second query */
    script(&fk->events, S, 6, R, R, R, R, R, R);
    uint64_t start = nowNs();
    VkResult answers[8];
    int n = 0;
    do {
        answers[n] = getEventStatus(device, eventA);
        CHECK(answers[n] == fk->lastEventResult, "loop: the driver's last answer");
    } while (answers[n++] == R && n < 8);
    uint64_t took = nowNs() - start;
    CHECK(n == 4 && answers[0] == R && answers[1] == R && answers[2] == R && answers[3] == S && fk->events.calls == 7,
          "loop: %d answers, last %d, %d driver calls", n, answers[n - 1], fk->events.calls);
    CHECK(took >= 3 * 800000ull, "loop: 3 sleeps of 800 us took %llu us", (unsigned long long)(took / 1000));
    sleepsSeen = 3;

    /* after an answer other than reset, the next reset is a first one */
    script(&fk->events, R, 0);
    expect("reset after set", eventA, R, 1);
    expect("repeated", eventA, R, 2);
    /* another event in between: no sleep */
    expect("event B after A", eventB, R, 1);
    expect("event A after B", eventA, R, 1);
    expect("event B after A again", eventB, R, 1);
    /* the other thread's calls do not count for this one, nor this one's for it (a second thread checks
     * its own repetition: its sleep would put this thread's next call outside the 1 ms) */
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, otherThreadOnce, NULL) == 0 && pthread_join(thread, NULL) == 0, "thread");
    bool soon = recent();
    expect("this thread's repeated reset after the other's", eventB, R, soon ? 2 : -1);
    CHECK(pthread_create(&thread, NULL, otherThread, NULL) == 0 && pthread_join(thread, NULL) == 0, "second thread");
    /* the same event again more than 1 ms after this thread's last reset of it (a poll each frame, a
     * reused VkEvent): a first reset; right after that, a repeated one */
    sleepMs(3);
    expect("event B 3 ms later", eventB, R, 1);
    expect("event B right after", eventB, R, 2);
    /* errors: returned as they are, never followed by a sleep, and end the repetition */
    script(&fk->events, R, 1, LOST);
    expect("an error", eventB, LOST, 1);
    expect("reset after an error", eventB, R, 1);
    script(&fk->events, R, 2, R, LOST);
    expect("an error after the sleep", eventB, LOST, 2);
    expect("reset after that", eventB, R, 1);
    script(&fk->events, R, 2, R, S);
    expect("set after the sleep", eventB, S, 2);
    script(&fk->events, S, 0);
    expect("set", eventB, S, 1);
    present();
    tearDown();

    const char* log = ownLog();
    CHECK(countLines(log, "hybris-icd: event sleep: on, 800 us after a repeated VK_EVENT_RESET, no pause file, counts every 60 s") == 1,
          "one \"on\" line");
    CHECK(countLines(log, "hybris-icd: event sleep: first sleep, pid ") == 1 && strstr(log, ", 800 us asked, "), "one first-sleep line");
    CHECK(!strstr(log, "event sleep: counts,"), "no counts line before 60 s");
    checkCounts(log, "device destroyed", 800, 21, 3, sleepsSeen, 2);
    CHECK(soon ? sleepsSeen == 9 : sleepsSeen == 8 || sleepsSeen == 9, "%d sleeps", sleepsSeen);
}

static void runMax(void) {
    setUp();
    CHECK(lookUp(), "vkGetEventStatus wrapped");
    script(&fk->events, R, 0);
    expect("first", eventA, R, 1);
    uint64_t start = nowNs();
    expect("repeated", eventA, R, 2);
    uint64_t took = nowNs() - start;
    CHECK(took >= 1000000ull, "a sleep of 1000 us took %llu us", (unsigned long long)(took / 1000));
    tearDown();
    const char* log = ownLog();
    CHECK(countLines(log, "hybris-icd: event sleep: on, 1000 us (DROIDDECK_HYBRIS_EVENT_SLEEP_US=5000 lowered to the maximum) after a "
                          "repeated VK_EVENT_RESET, no pause file, counts every 60 s") == 1,
          "one \"on\" line, lowered");
    checkCounts(log, "device destroyed", 1000, 2, 0, 1, 0);
}

/* The pause file is there at the start, goes, comes back. Its state is looked at every 250 ms at most,
 * on repeated resets only (1 ms apart at most): where a check of that depends on the time, it is made
 * only when this ran fast enough. */
static void runPause(void) {
    const char* path = getenv("DROIDDECK_HYBRIS_EVENT_SLEEP_PAUSE");
    setUp();
    uint64_t looked = nowNs(); /* the first look at the file is in the lookup */
    CHECK(lookUp(), "vkGetEventStatus wrapped");
    script(&fk->events, R, 0);
    sleepsSeen = 0;
    expect("paused: first", eventA, R, 1);
    expect("paused: repeated", eventA, R, 1);
    expect("paused: repeated again", eventA, R, 1);
    unlink(path);
    bool fast = nowNs() - looked < 200000000ull;
    expect("file gone, not looked at yet", eventA, R, fast ? 1 : -1);
    sleepMs(300);
    expect("file gone, 300 ms later: a first reset", eventA, R, 1);
    expect("file gone, looked at", eventA, R, 2);
    looked = nowNs();
    int fd = open(path, O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0, "pause file");
    if (fd >= 0) close(fd);
    fast = nowNs() - looked < 200000000ull && recent();
    expect("file back, not looked at yet", eventA, R, fast ? 2 : -1);
    sleepMs(300);
    expect("file back, 300 ms later: a first reset", eventA, R, 1);
    expect("file back, looked at", eventA, R, 1);
    expect("paused again", eventA, R, 1);
    unlink(path);
    tearDown();

    const char* log = ownLog();
    char on[256];
    snprintf(on, sizeof(on),
             "hybris-icd: event sleep: on, 800 us after a repeated VK_EVENT_RESET, pause file %s (there: paused), counts every 60 s", path);
    CHECK(countLines(log, on) == 1, "one line \"%s\"", on);
    Counts c;
    CHECK(countLines(log, "hybris-icd: event sleep: resumed, pid ") == 1 && countsLine(log, "resumed", &c) && !c.paused,
          "one \"resumed\" line");
    CHECK(countLines(log, "hybris-icd: event sleep: paused, pid ") == 1 && countsLine(log, "paused", &c) && c.paused, "one \"paused\" line");
    checkCounts(log, "device destroyed", 800, 10, 0, sleepsSeen, 0);
    CHECK(sleepsSeen >= 1, "%d sleeps", sleepsSeen);
}

/* DROIDDECK_HYBRIS_EVENT_SLEEP_LOG_S=1: a counts line at the first present a second after the start,
 * none while no call came. */
static void runCounts(void) {
    setUp();
    CHECK(lookUp(), "vkGetEventStatus wrapped");
    script(&fk->events, R, 0);
    expect("first", eventA, R, 1);
    expect("repeated", eventA, R, 2);
    present();
    CHECK(!strstr(ownLog(), "event sleep: counts,"), "no counts line within the second");
    sleepMs(1100);
    present();
    present();
    const char* log = ownLog();
    CHECK(countLines(log, "hybris-icd: event sleep: counts, pid ") == 1, "one counts line");
    checkCounts(log, "counts", 500, 2, 0, 1, 0);
    sleepMs(1100);
    present();
    CHECK(countLines(ownLog(), "hybris-icd: event sleep: counts, pid ") == 1, "no counts line without new calls");
    expect("first after the wait", eventA, R, 1);
    expect("repeated again", eventA, R, 2);
    tearDown();
    log = ownLog();
    CHECK(countLines(log, "hybris-icd: event sleep: on, 500 us after a repeated VK_EVENT_RESET, no pause file, counts every 1 s") == 1,
          "one \"on\" line");
    checkCounts(log, "device destroyed", 500, 4, 0, 2, 0);
}

/* ---- the processes ---- */

typedef struct {
    const char* name;
    void (*run)(void);
    const char* env[3]; /* VAR=value */
    bool pauseFile;     /* DROIDDECK_HYBRIS_EVENT_SLEEP_PAUSE=<a file made before the start> */
} Setting;

static const Setting settings[] = {
    {"unset", runOff, {NULL}},
    {"zero", runOff, {"DROIDDECK_HYBRIS_EVENT_SLEEP_US=0"}},
    {"empty", runOff, {"DROIDDECK_HYBRIS_EVENT_SLEEP_US="}},
    {"garbage", runOff, {"DROIDDECK_HYBRIS_EVENT_SLEEP_US=100us"}},
    {"negative", runOff, {"DROIDDECK_HYBRIS_EVENT_SLEEP_US=-5"}},
    {"on", runOn, {"DROIDDECK_HYBRIS_EVENT_SLEEP_US=800"}},
    {"max", runMax, {"DROIDDECK_HYBRIS_EVENT_SLEEP_US=5000"}},
    {"pause", runPause, {"DROIDDECK_HYBRIS_EVENT_SLEEP_US=800"}, true},
    {"counts", runCounts, {"DROIDDECK_HYBRIS_EVENT_SLEEP_US=500", "DROIDDECK_HYBRIS_EVENT_SLEEP_LOG_S=1"}},
};

static void checkSetting(const Setting* s, const char* self) {
    char path[] = "/tmp/event_sleep_test.XXXXXX", pause[64];
    int fd = mkstemp(path);
    if (fd < 0) {
        CHECK(0, "%s: no log file", s->name);
        return;
    }
    snprintf(pause, sizeof(pause), "/tmp/event_sleep_test.%d.pause", (int)getpid());
    if (s->pauseFile) close(open(pause, O_CREAT | O_WRONLY, 0600));
    fflush(stdout);
    pid_t child = fork();
    if (child == 0) {
        dup2(fd, 2);
        const char* clear[] = {"DROIDDECK_HYBRIS_EVENT_SLEEP_US", "DROIDDECK_HYBRIS_EVENT_SLEEP_PAUSE", "DROIDDECK_HYBRIS_EVENT_SLEEP_LOG_S",
                               "DROIDDECK_HYBRIS_PIPELINE_CACHE_DIR", "DROIDDECK_HYBRIS_CRASH_GUARD"};
        for (unsigned i = 0; i < sizeof(clear) / sizeof(clear[0]); i++) unsetenv(clear[i]);
        for (int i = 0; s->env[i]; i++) putenv((char*)s->env[i]);
        if (s->pauseFile) setenv("DROIDDECK_HYBRIS_EVENT_SLEEP_PAUSE", pause, 1);
        alarm(60);
        execl(self, self, s->name, (char*)NULL);
        _exit(98);
    }
    int status = 0;
    waitpid(child, &status, 0);
    unlink(pause);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "%s: status %#x", s->name, status);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        ssize_t n = pread(fd, logText, sizeof(logText) - 1, 0);
        logText[n > 0 ? n : 0] = 0;
        fprintf(stderr, "---- %s's log:\n%s----\n", s->name, logText);
    }
    close(fd);
    unlink(path);
}

int main(int argc, char** argv) {
    if (argc > 1) {
        for (unsigned i = 0; i < sizeof(settings) / sizeof(settings[0]); i++)
            if (!strcmp(argv[1], settings[i].name)) settings[i].run();
        printf("event_sleep_test (%s): %d checks, %d failed\n", argv[1], checks, failures);
        return failures || !checks ? 1 : 0;
    }
    for (unsigned i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) checkSetting(&settings[i], "/proc/self/exe");
    printf("event_sleep_test: %d settings, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
