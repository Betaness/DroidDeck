#define _GNU_SOURCE
#include "frame_sched_feed.h"

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void droiddeck_log(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* libxfbt's setDequeueBuffer / setQueueBuffer / setFpsInfo arguments, as libmigui's
 * FrameSchedPolicy::notifyFrameSched fills them (HyperOS 3, Android 17 build of 2026-10). */
struct fbt_frame {
    int32_t pid, tid, layer_id, reserved0;
    uint64_t frame_id;
    int32_t fence_fd;     /* a fence fd of the calling process, -1: none */
    int32_t buffer_count;
    int32_t expected_us;  /* the frame time the layer should keep */
    int32_t frame_insert; /* libmigui: 1 for an inserted (generated) frame */
    uint64_t name_hash;   /* FNV-1a 64 of the layer name before its '#' */
    int64_t time_ns;      /* CLOCK_MONOTONIC */
};
struct fbt_fps {
    int32_t pid, layer_id;
    int32_t fps_x10;      /* SchedGenius logs it as scaledFps: 600 for 60 fps */
    int32_t reserved0;
    int64_t time_ns;
    int64_t reserved1;
};
_Static_assert(sizeof(struct fbt_frame) == 0x38, "xr_fbt frame layout");
_Static_assert(sizeof(struct fbt_fps) == 0x20, "xr_fbt fps layout");
#define FBT_DEQUEUE _IOW('x', 0, struct fbt_frame)
#define FBT_QUEUE _IOW('x', 1, struct fbt_frame)
#define FBT_FPS _IOW('x', 6, struct fbt_fps)
#define FBT_DEVICE "/dev/xr_fbt"

#define LOG_EVERY_NS 30000000000LL
#define LOG_CHANGE_NS 5000000000LL  /* a change of layer or thread is logged at most this often */
#define CHECK_THREAD_NS 1000000000LL /* how often the game's thread is looked for in /proc */
#define FPS_AGAIN_NS 1000000000LL /* SchedGenius drops a layer that went quiet; its rate is sent again */

static _Atomic int g_mode;
static _Atomic uint64_t g_game; /* pid << 32 | tid */

/* Event-loop thread only. */
static int g_mode_seen;
static int g_fd = -1;
static int g_broken;
static int g_layer_off = -2; /* -2: not looked up, -1: unavailable */
static int g_refused;        /* the kernel turned down the game's thread */
static uint64_t g_frame_id;
static int32_t g_fps_layer, g_fps_pid, g_fps_x10;
static int64_t g_last_ns, g_log_ns;
static unsigned g_reported, g_errors;
static int g_last_errno;
static const char *g_hash_name;
static uint64_t g_hash;
static int32_t g_logged_layer, g_logged_pid, g_logged_tid;
static int64_t g_change_ns, g_check_ns;
static unsigned g_changes; /* changes not logged since the last line */

void fsf_set_mode(int mode) {
    if (mode < FSF_OFF || mode > FSF_GAME_THREAD) mode = FSF_OFF;
    int was = atomic_exchange(&g_mode, mode);
    if (was == mode) return;
    static const char *const names[] = {"off", "as the compositor thread", "as the game's busiest thread"};
    droiddeck_log("framesched", "game layer frames to SchedGenius: %s", names[mode]);
}

void fsf_set_game_thread(int pid, int tid) {
    atomic_store(&g_game, pid > 0 && tid > 0 ? (uint64_t)(uint32_t)pid << 32 | (uint32_t)tid : 0);
}

int fsf_enabled(void) { return atomic_load(&g_mode) != FSF_OFF; }

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

struct lib_ref { const char *suffix; const char *path; };

static int find_lib(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    struct lib_ref *lib = data;
    const char *name = info->dlpi_name;
    size_t n = name ? strlen(name) : 0, s = strlen(lib->suffix);
    if (n < s || strcmp(name + n - s, lib->suffix) != 0) return 0;
    lib->path = name;
    return 1;
}

/* libgui is outside the app's linker namespace (no dlsym), so its exported symbol is read from the file,
 * and so are its first two instructions (`code`): the mapped text may one day be execute-only. */
static uintptr_t elf_symbol(const char *path, const char *symbol, uint32_t code[2]) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    uintptr_t value = 0;
    int have_code = 0;
    if (fstat(fd, &st) == 0 && st.st_size > (off_t)sizeof(Elf64_Ehdr)) {
        const uint8_t *base = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (base != MAP_FAILED) {
            const Elf64_Ehdr *eh = (const Elf64_Ehdr *)base;
            size_t size = st.st_size;
            if (memcmp(eh->e_ident, ELFMAG, SELFMAG) == 0 && eh->e_ident[EI_CLASS] == ELFCLASS64 &&
                eh->e_shoff + (size_t)eh->e_shnum * sizeof(Elf64_Shdr) <= size) {
                const Elf64_Shdr *sh = (const Elf64_Shdr *)(base + eh->e_shoff);
                for (int i = 0; i < eh->e_shnum && !value; i++) {
                    if (sh[i].sh_type != SHT_DYNSYM || sh[i].sh_link >= eh->e_shnum) continue;
                    const Elf64_Shdr *strs = &sh[sh[i].sh_link];
                    if (sh[i].sh_offset + sh[i].sh_size > size || strs->sh_offset + strs->sh_size > size) continue;
                    const Elf64_Sym *syms = (const Elf64_Sym *)(base + sh[i].sh_offset);
                    const char *names = (const char *)(base + strs->sh_offset);
                    size_t count = sh[i].sh_size / sizeof(Elf64_Sym);
                    for (size_t k = 0; k < count; k++) {
                        if (syms[k].st_name >= strs->sh_size || syms[k].st_shndx == SHN_UNDEF) continue;
                        if (strcmp(names + syms[k].st_name, symbol) == 0) {
                            value = syms[k].st_value;
                            break;
                        }
                    }
                }
                /* The symbol's address to a file offset, through the PT_LOAD segment holding it. */
                const Elf64_Phdr *ph = (const Elf64_Phdr *)(base + eh->e_phoff);
                for (int i = 0; value && eh->e_phoff + (size_t)eh->e_phnum * sizeof(Elf64_Phdr) <= size &&
                                i < eh->e_phnum && !have_code; i++) {
                    if (ph[i].p_type != PT_LOAD || value < ph[i].p_vaddr || value + 8 > ph[i].p_vaddr + ph[i].p_filesz)
                        continue;
                    const uint64_t at = value - ph[i].p_vaddr + ph[i].p_offset;
                    if (at + 8 > size) break;
                    memcpy(code, base + at, 8);
                    have_code = 1;
                }
            }
            munmap((void *)base, st.st_size);
        }
    }
    close(fd);
    return have_code ? value : 0;
}

/* Where android::SurfaceControl keeps its SurfaceFlinger layer id: read from getLayerId(), which is
 * a single field load (ldr w0, [x0, #off]; ret) on this build; anything else is left alone. */
static int layer_id_offset(void) {
    if (g_layer_off != -2) return g_layer_off;
    g_layer_off = -1;
    struct lib_ref gui = {.suffix = "/libgui.so"};
    dl_iterate_phdr(find_lib, &gui);
    uint32_t code[2] = {0, 0};
    uintptr_t sym = gui.path ? elf_symbol(gui.path, "_ZNK7android14SurfaceControl10getLayerIdEv", code) : 0;
    if (!sym) {
        droiddeck_log("framesched", "not reporting: no SurfaceControl::getLayerId in %s", gui.path ? gui.path : "(libgui not loaded)");
        return -1;
    }
    if ((code[0] & 0xffc003ffu) != 0xb9400000u || code[1] != 0xd65f03c0u) {
        droiddeck_log("framesched", "not reporting: SurfaceControl::getLayerId is not a field load (%08x %08x)", code[0], code[1]);
        return -1;
    }
    g_layer_off = (int)((code[0] >> 10) & 0xfff) * 4;
    return g_layer_off;
}

static uint64_t name_hash(const char *name) {
    if (name == g_hash_name) return g_hash;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (const char *p = name; p && *p && *p != '#'; p++) h = (h ^ (uint8_t)*p) * 0x100000001b3ULL;
    g_hash_name = name;
    g_hash = h;
    return h;
}

static void reset(void) {
    if (g_fd >= 0) close(g_fd);
    g_fd = -1;
    g_broken = 0;
    g_refused = 0;
    g_fps_layer = g_fps_pid = g_fps_x10 = 0;
    g_last_ns = g_log_ns = 0;
    g_reported = g_errors = 0;
    g_logged_layer = g_logged_pid = g_logged_tid = 0;
    g_change_ns = g_check_ns = 0;
    g_changes = 0;
}

static int submit(unsigned long request, void *arg) {
    if (ioctl(g_fd, request, arg) >= 0) return 0;
    int e = errno;
    if (!g_errors++ || e != g_last_errno)
        droiddeck_log("framesched", "%s ioctl failed: %s (%u failure%s so far)", request == FBT_FPS ? "frame-rate" :
                      request == FBT_QUEUE ? "queue" : "dequeue", strerror(e), g_errors, g_errors == 1 ? "" : "s");
    g_last_errno = e;
    return -e;
}

static int thread_exists(int pid, int tid) {
    char path[48];
    snprintf(path, sizeof(path), "/proc/%d/task/%d", pid, tid);
    return access(path, F_OK) == 0;
}

/* The game's thread has exited: forget it (unless the app has named another meanwhile), so its tid
 * is not reported for a thread that reuses it, and the compositor's thread is reported until the
 * app's next scan names one. */
static void game_thread_gone(uint64_t game) {
    if (atomic_compare_exchange_strong(&g_game, &game, 0))
        droiddeck_log("framesched", "the game's thread %d/%d is gone: reporting the compositor thread until another is named",
                      (int)(game >> 32), (int)(uint32_t)game);
}

/* Queue for this frame, then the next frame's dequeue at the same time: as seen from SchedGenius,
 * a render thread that starts the next frame as it hands over this one. That is not what it measures
 * for an app: libmigui dequeues when the app starts drawing, so dequeue-to-queue is the render time.
 * The compositor only sees the game's frames arrive, so here it is the time between two frames shown,
 * and a game held at its frame-rate limit or the panel's rate looks like it uses the whole budget;
 * any jitter reads as a missed frame and may keep the clocks up for nothing (more power, no gain).
 * Both ioctls run on the compositor's event-loop thread: if xr_fbt waits on the fence or a lock,
 * presenting waits with it. */
static int report(int pid, int tid, int layer, int fence_fd, int images, int expected_us, uint64_t hash, int64_t t) {
    struct fbt_frame f = {.pid = pid, .tid = tid, .layer_id = layer, .frame_id = g_frame_id, .fence_fd = fence_fd,
                          .buffer_count = images, .expected_us = expected_us, .name_hash = hash, .time_ns = t};
    int r = submit(FBT_QUEUE, &f);
    if (r) return r;
    f.frame_id = ++g_frame_id;
    f.fence_fd = -1;
    return submit(FBT_DEQUEUE, &f);
}

void fsf_game_frame(const void *surface_control, const char *layer_name, int fence_fd, int images, float target_fps) {
    int mode = atomic_load(&g_mode);
    if (mode != g_mode_seen) {
        g_mode_seen = mode;
        reset();
    }
    if (mode == FSF_OFF || g_broken || !surface_control) return;
    int off = layer_id_offset();
    if (off < 0) { g_broken = 1; return; }
    int32_t layer = *(const int32_t *)((const uint8_t *)surface_control + off);
    if (layer <= 0 || layer >= (1 << 30)) {
        droiddeck_log("framesched", "not reporting: %s has no plausible layer id (%d)", layer_name, layer);
        g_broken = 1;
        return;
    }
    if (g_fd < 0) {
        g_fd = open(FBT_DEVICE, O_RDWR | O_CLOEXEC);
        if (g_fd < 0) {
            droiddeck_log("framesched", "not reporting: %s: %s", FBT_DEVICE, strerror(errno));
            g_broken = 1;
            return;
        }
    }
    int pid = getpid(), tid = gettid();
    uint64_t game = atomic_load(&g_game);
    int named = mode == FSF_GAME_THREAD && game && !g_refused;
    const int64_t t = now_ns();
    if (named && t - g_check_ns >= CHECK_THREAD_NS) {
        g_check_ns = t;
        if (!thread_exists((int)(game >> 32), (int)(uint32_t)game)) {
            game_thread_gone(game);
            named = 0;
        }
    }
    if (named) { pid = (int)(game >> 32); tid = (int)(uint32_t)game; }
    if (!(target_fps >= 10.0f)) target_fps = 60.0f;
    if (target_fps > 240.0f) target_fps = 240.0f;
    const int fps_x10 = (int)(target_fps * 10.0f + 0.5f), expected_us = (int)(1e6f / target_fps + 0.5f);
    const uint64_t hash = name_hash(layer_name);
    if (layer != g_fps_layer || pid != g_fps_pid || fps_x10 != g_fps_x10 || t - g_last_ns > FPS_AGAIN_NS) {
        struct fbt_fps fps = {.pid = pid, .layer_id = layer, .fps_x10 = fps_x10, .time_ns = t};
        if (submit(FBT_FPS, &fps) == 0) { g_fps_layer = layer; g_fps_pid = pid; g_fps_x10 = fps_x10; }
    }
    int r = report(pid, tid, layer, fence_fd, images, expected_us, hash, t);
    if (r && named && r != -EBADF) {
        if (thread_exists(pid, tid)) {
            g_refused = 1;
            droiddeck_log("framesched", "the kernel turned down the game's thread %d/%d (%s): reporting the compositor thread instead",
                          pid, tid, strerror(-r));
        } else {
            game_thread_gone(game);
        }
        named = 0;
        pid = getpid(); tid = gettid();
        g_fps_pid = 0;
        r = report(pid, tid, layer, fence_fd, images, expected_us, hash, t);
    }
    g_last_ns = t;
    if (r) return;
    g_reported++;
    const int changed = layer != g_logged_layer || pid != g_logged_pid || tid != g_logged_tid;
    if (changed && (!g_change_ns || t - g_change_ns >= LOG_CHANGE_NS)) {
        droiddeck_log("framesched", "reporting %s (layer %d, hash %016llx) to SchedGenius at %.1f fps (%d us a frame) "
                      "as pid %d tid %d%s%s", layer_name, layer, (unsigned long long)hash, fps_x10 / 10.0, expected_us,
                      pid, tid, named ? " (the game's busiest thread)" : " (the compositor thread)",
                      g_changes ? "; it changed more often than logged" : "");
        g_logged_layer = layer; g_logged_pid = pid; g_logged_tid = tid;
        g_change_ns = g_log_ns = t;
        g_changes = 0;
    } else if (changed) {
        g_changes++;
    } else if (t - g_log_ns >= LOG_EVERY_NS) {
        droiddeck_log("framesched", "%u frames reported in %.0f s (layer %d, %.1f fps target, tid %d)%s", g_reported,
                      (t - g_log_ns) / 1e9, layer, fps_x10 / 10.0, tid, g_errors ? "; some ioctls failed, see above" : "");
        g_reported = 0;
        g_log_ns = t;
    }
}
