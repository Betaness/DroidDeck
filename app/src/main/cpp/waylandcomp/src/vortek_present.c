#include "vortek_present.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <android/hardware_buffer.h>

#include "sc_layer.h"
#include "droiddeck_ext.h"

#define VP_MAX_CHAINS 32
#define VP_MAX_IMAGES 8

struct vp_chain {
    uint32_t id;
    uint32_t n, w, h;
    AHardwareBuffer *ahb[VP_MAX_IMAGES];
    int spatial_said;
};

static struct vp_chain g_chains[VP_MAX_CHAINS];
static struct wl_event_loop *g_loop;
static struct wl_event_source *g_listen_src, *g_conn_src;
static int g_listen_fd = -1;
static pthread_mutex_t g_conn_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_conn_fd = -1;
static int g_scene_w, g_scene_h;
void droiddeck_count_game_frame(void);
static unsigned g_stat_frames;
/* One SurfaceControl layer per window shown zero-copy (Steam's main view and its side menu, a game),
 * stacked like the windows: by the client's X stacking key, then by arrival. */
#define VP_LAYERS SC_VORTEK_LAYERS
struct vp_shown {
    uint32_t chain;           /* 0: free */
    int32_t rect[4];          /* X root coordinates */
    uint64_t key;
    uint32_t seq;
    uint32_t toplevel;        /* the window's top-level X window, 0 if the client did not say */
    uint32_t report;          /* g_report_seq of the last PRESENT or RESTACK with its key */
    int z;
};
static struct vp_shown g_show[VP_LAYERS];
static uint32_t g_show_seq, g_report_seq;
static int g_first_logged;
static int g_accept = 1;
static int g_spatial;
/* While on_conn drains the socket: the messages' layer changes go into one transaction (created on
 * first use), so a menu's HIDE and the view below's next frame reach the screen together. */
static int g_batching;
static struct ASurfaceTransaction *g_batch;
static unsigned g_batch_buffers;  /* layers that got a buffer in g_batch */
static int g_trace;               /* VORTEK_PRESENT_TRACE=1: a timestamped line per message */

/* sc_layer only compares tokens, and its completion callback may run after the chain is gone,
 * so a token encodes chain and image instead of pointing at them. */
static void *token_of(uint32_t chain, uint32_t index) {
    return (void *)(uintptr_t)(((uint64_t)chain << 8) | (index & 0xff) | (1ull << 40));
}

static struct vp_chain *chain_of(uint32_t id) {
    for (int i = 0; i < VP_MAX_CHAINS; i++)
        if (g_chains[i].id == id) return &g_chains[i];
    return NULL;
}

static void chain_free(struct vp_chain *c) {
    for (uint32_t i = 0; i < c->n && i < VP_MAX_IMAGES; i++)
        if (c->ahb[i]) AHardwareBuffer_release(c->ahb[i]);
    memset(c, 0, sizeof(*c));
}

static int send_msg(int fd, const struct vp_msg *m, int pass_fd) {
    struct iovec iov = {(void *)m, sizeof(*m)};
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr h = {0};
    h.msg_iov = &iov;
    h.msg_iovlen = 1;
    if (pass_fd >= 0) {
        memset(cbuf, 0, sizeof(cbuf));
        h.msg_control = cbuf;
        h.msg_controllen = sizeof(cbuf);
        struct cmsghdr *c = CMSG_FIRSTHDR(&h);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &pass_fd, sizeof(int));
    }
    ssize_t r;
    do r = sendmsg(fd, &h, MSG_NOSIGNAL); while (r < 0 && errno == EINTR);
    return r == (ssize_t)sizeof(*m) ? 0 : -1;
}

/* 0: a message; 1: none waiting; -1: the connection is broken. */
static int recv_msg(int fd, struct vp_msg *m, int *got_fd) {
    struct iovec iov = {m, sizeof(*m)};
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr h = {0};
    h.msg_iov = &iov;
    h.msg_iovlen = 1;
    h.msg_control = cbuf;
    h.msg_controllen = sizeof(cbuf);
    *got_fd = -1;
    ssize_t r;
    do r = recvmsg(fd, &h, MSG_CMSG_CLOEXEC | MSG_DONTWAIT); while (r < 0 && errno == EINTR);
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 1;
    if (r <= 0) return -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&h); c; c = CMSG_NXTHDR(&h, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(got_fd, CMSG_DATA(c), sizeof(int));
    if (r != (ssize_t)sizeof(*m)) {
        if (*got_fd >= 0) close(*got_fd);
        *got_fd = -1;
        return -1;
    }
    return 0;
}

static void send_release(uint32_t chain, uint32_t index, int fence_fd) {
    struct vp_msg m = {.type = VP_MSG_RELEASE, .chain = chain, .index = index};
    pthread_mutex_lock(&g_conn_lock);
    if (g_conn_fd >= 0) send_msg(g_conn_fd, &m, fence_fd);
    pthread_mutex_unlock(&g_conn_lock);
    if (fence_fd >= 0) close(fence_fd);
}

void vortek_present_released(void *token, int release_fd) {
    uint64_t t = (uint64_t)(uintptr_t)token;
    send_release((uint32_t)(t >> 8) & 0xffffffffu, (uint32_t)(t & 0xff), release_fd);
}

static void send_simple(uint32_t type, uint32_t chain, uint32_t index) {
    struct vp_msg m = {.type = type, .chain = chain, .index = index};
    pthread_mutex_lock(&g_conn_lock);
    if (g_conn_fd >= 0) send_msg(g_conn_fd, &m, -1);
    pthread_mutex_unlock(&g_conn_lock);
}

static void refuse(uint32_t chain, const char *why) {
    send_simple(VP_MSG_REFUSED, chain, 0);
    droiddeck_log("vortek", "zero-copy refused for swapchain %u: %s", chain, why);
}

static struct ASurfaceTransaction *batch(void) {
    if (g_batching && !g_batch) g_batch = sc_layer_tx_begin();
    return g_batch;
}

static void batch_apply(void) {
    sc_layer_tx_apply(g_batch);
    g_batch = NULL;
    g_batch_buffers = 0;
}

/* A layer gets one buffer per transaction: a second one would replace the first unseen, and the
 * first's token would come back with a release fence that is not its own. */
static void batch_touch(int slot) {
    if (g_batch_buffers & (1u << slot)) batch_apply();
}

static void hide_slot(int i) {
    if (!g_show[i].chain) return;
    batch_touch(i);
    sc_layer_hide_vortek(i, batch());
    memset(&g_show[i], 0, sizeof(g_show[i]));
}

static void hide_chain(uint32_t chain) {
    for (int i = 0; i < VP_LAYERS; i++)
        if (g_show[i].chain == chain) hide_slot(i);
}

static void hide(void) {
    for (int i = 0; i < VP_LAYERS; i++) hide_slot(i);
}

/* Whether layer a goes below layer b. Two top-level windows cannot hold the same place among the
 * root's children: when their keys agree there (the top 16 bits), one key is stale (an idle window
 * raised over or lowered since it last reported), and the top-level window that reported last goes
 * above. Otherwise by key, then by arrival. `recent`: per layer, its top-level window's last report. */
static int below(int a, int b, const uint32_t *recent) {
    const struct vp_shown *x = &g_show[a], *y = &g_show[b];
    if (x->toplevel && y->toplevel && x->toplevel != y->toplevel && (x->key >> 48) == (y->key >> 48) &&
        recent[a] != recent[b])
        return recent[a] < recent[b];
    if (x->key != y->key) return x->key < y->key;
    return x->seq < y->seq;
}

/* Layer order from the stacking keys; returns the z of `slot` and moves the other layers whose
 * place changed (slot -1: moves them all). The z values sit between sc_layer's GAME and OVERLAY. */
static int restack(int slot) {
    int order[VP_LAYERS], n = 0;
    uint32_t recent[VP_LAYERS] = {0};
    for (int i = 0; i < VP_LAYERS; i++) {
        if (!g_show[i].chain) continue;
        order[n++] = i;
        /* All windows of a top-level window share its recency, so the order stays transitive. */
        for (int j = 0; j < VP_LAYERS; j++)
            if (g_show[j].chain && (j == i || (g_show[i].toplevel && g_show[j].toplevel == g_show[i].toplevel)) &&
                g_show[j].report > recent[i])
                recent[i] = g_show[j].report;
    }
    for (int a = 1; a < n; a++) {
        int cur = order[a], b = a - 1;
        while (b >= 0 && below(cur, order[b], recent)) {
            order[b + 1] = order[b];
            b--;
        }
        order[b + 1] = cur;
    }
    int z = SC_VORTEK_Z_BASE;
    for (int r = 0; r < n; r++) {
        int i = order[r], want = SC_VORTEK_Z_BASE + r;
        if (i == slot) z = want;
        else if (g_show[i].z != want) sc_layer_vortek_z(i, want, batch());
        g_show[i].z = want;
    }
    return z;
}

static void drop_connection(void) {
    hide();
    if (g_conn_src) wl_event_source_remove(g_conn_src);
    g_conn_src = NULL;
    pthread_mutex_lock(&g_conn_lock);
    if (g_conn_fd >= 0) close(g_conn_fd);
    g_conn_fd = -1;
    pthread_mutex_unlock(&g_conn_lock);
    for (int i = 0; i < VP_MAX_CHAINS; i++)
        if (g_chains[i].id) chain_free(&g_chains[i]);
    droiddeck_log("vortek", "zero-copy presenter disconnected");
}

static void on_create(int fd, const struct vp_msg *m) {
    struct vp_chain *c = chain_of(m->chain);
    if (c) {
        hide_chain(m->chain);
        chain_free(c);
    }
    c = chain_of(0);
    uint32_t n = m->image_count;
    AHardwareBuffer *ahb[VP_MAX_IMAGES] = {0};
    for (uint32_t i = 0; i < n; i++) {
        AHardwareBuffer *b = NULL;
        if (AHardwareBuffer_recvHandleFromUnixSocket(fd, &b) != 0) b = NULL;
        if (i < VP_MAX_IMAGES) ahb[i] = b;
        else if (b) AHardwareBuffer_release(b);
    }
    int complete = c && n > 0 && n <= VP_MAX_IMAGES && m->chain;
    for (uint32_t i = 0; complete && i < n; i++) complete = ahb[i] != NULL;
    if (!complete) {
        for (uint32_t i = 0; i < n && i < VP_MAX_IMAGES; i++) if (ahb[i]) AHardwareBuffer_release(ahb[i]);
        refuse(m->chain, !c ? "no free swapchain slot" : "its buffers did not arrive");
        return;
    }
    c->id = m->chain;
    c->n = n;
    c->w = m->width;
    c->h = m->height;
    memcpy(c->ahb, ahb, sizeof(ahb));
    droiddeck_log("vortek", "zero-copy swapchain %u: %ux%u, %u AHardwareBuffers", c->id, c->w, c->h, c->n);
}

static void on_present(const struct vp_msg *m, int acquire_fd) {
    struct vp_chain *c = chain_of(m->chain);
    if (!c || m->index >= c->n || !c->ahb[m->index] || g_scene_w <= 0 || g_scene_h <= 0 || !g_accept) {
        if (acquire_fd >= 0) close(acquire_fd);
        send_release(m->chain, m->index, -1);
        if (!c) refuse(m->chain, "unknown swapchain");
        return;
    }
    int slot = -1, fresh = 0;
    for (int i = 0; i < VP_LAYERS && slot < 0; i++) if (g_show[i].chain == m->chain) slot = i;
    if (slot < 0) {
        fresh = 1;
        for (int i = 0; i < VP_LAYERS && slot < 0; i++) if (!g_show[i].chain) slot = i;
        /* Every layer taken: a swapchain recreated for a window (same place, same stacking key)
         * takes over that window's layer. */
        uint64_t key = ((uint64_t)m->key_hi << 32) | m->key_lo;
        for (int i = 0; i < VP_LAYERS && slot < 0; i++)
            if (g_show[i].rect[0] == m->x && g_show[i].rect[1] == m->y && g_show[i].rect[2] == m->w &&
                g_show[i].rect[3] == m->h && g_show[i].key == key) slot = i;
        if (slot < 0) {
            if (acquire_fd >= 0) close(acquire_fd);
            send_release(m->chain, m->index, -1);
            refuse(m->chain, "every zero-copy layer is in use");
            return;
        }
    }
    batch_touch(slot);
    struct vp_shown before = g_show[slot];
    if (fresh) g_show[slot].seq = ++g_show_seq;
    g_show[slot].chain = m->chain;
    g_show[slot].rect[0] = m->x; g_show[slot].rect[1] = m->y; g_show[slot].rect[2] = m->w; g_show[slot].rect[3] = m->h;
    g_show[slot].key = ((uint64_t)m->key_hi << 32) | m->key_lo;
    g_show[slot].toplevel = m->toplevel;
    g_show[slot].report = ++g_report_seq;
    int z = restack(slot);
    int x = m->x, y = m->y, w = m->w, h = m->h;
    if (m->root_w > 0 && m->root_h > 0 && ((int)m->root_w != g_scene_w || (int)m->root_h != g_scene_h)) {
        const double sx = (double)g_scene_w / m->root_w, sy = (double)g_scene_h / m->root_h;
        x = (int)(m->x * sx + 0.5); y = (int)(m->y * sy + 0.5);
        w = (int)((m->x + m->w) * sx + 0.5) - x; h = (int)((m->y + m->h) * sy + 0.5) - y;
    }
    int r = sc_layer_present_vortek(slot, z, c->ahb[m->index], (int)c->w, (int)c->h, acquire_fd,
                                    token_of(m->chain, m->index), x, y, w, h, g_scene_w, g_scene_h, batch());
    if (r == 0 && g_batch) g_batch_buffers |= 1u << slot;
    if (r != 0) {
        send_release(m->chain, m->index, -1);
        if (fresh) {
            /* Not on the screen: the slot stays free, or keeps the window it had. */
            if (before.chain) g_show[slot] = before;
            else memset(&g_show[slot], 0, sizeof(g_show[slot]));
        }
        return;
    }
    g_stat_frames++;
    if (g_spatial && !c->spatial_said) {
        c->spatial_said = 1;
        droiddeck_log("effects", "zero-copy layer: spatial filter skipped for swapchain %u (%ux%u shown as is; "
                      "scene effects would switch zero-copy off)", c->id, c->w, c->h);
    }
    /* The frame-rate counters follow the biggest window on a layer (the game, or Steam's main view). */
    int64_t area = (int64_t)m->w * m->h, biggest = 0;
    for (int i = 0; i < VP_LAYERS; i++)
        if (g_show[i].chain && (int64_t)g_show[i].rect[2] * g_show[i].rect[3] > biggest)
            biggest = (int64_t)g_show[i].rect[2] * g_show[i].rect[3];
    if (area >= biggest) droiddeck_count_game_frame();
    if (!g_first_logged) {
        g_first_logged = 1;
        droiddeck_log("vortek", "game frames shown zero-copy: %ux%u buffer over the window at %d,%d %dx%d (X root %ux%u, scene %dx%d)",
                      c->w, c->h, m->x, m->y, m->w, m->h, m->root_w, m->root_h, g_scene_w, g_scene_h);
    }
}

void vortek_present_set_accept(int accept) {
    accept = accept ? 1 : 0;
    if (accept == g_accept) return;
    g_accept = accept;
    if (!accept) hide();
    send_simple(VP_MSG_MODE, 0, (uint32_t)accept);
    droiddeck_log("vortek", "zero-copy %s", accept ? "resumed" : "paused: scene effects, frame generation or cardboard need the composited scene");
}

void vortek_present_set_spatial(int on) { g_spatial = on ? 1 : 0; }

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* CLOCK_MONOTONIC, the clock of the client's "(t=...)" log lines (the guest shares the kernel's). */
static void trace_msg(const struct vp_msg *m) {
    static const char *const names[] = {"?", "CREATE", "PRESENT", "HIDE", "DESTROY", "?", "?", "?", "RESTACK"};
    droiddeck_log("vortek", "trace t=%.4f %s chain %u index %u key %08x%08x top 0x%x", now_s(),
                  m->type < sizeof(names) / sizeof(names[0]) ? names[m->type] : "?", m->chain, m->index,
                  m->key_hi, m->key_lo, m->toplevel);
}

static void on_msg(int fd, const struct vp_msg *m, int got_fd) {
    if (g_trace) trace_msg(m);
    switch (m->type) {
        case VP_MSG_CHAIN_CREATE:
            if (got_fd >= 0) close(got_fd);
            on_create(fd, m);
            break;
        case VP_MSG_PRESENT:
            on_present(m, got_fd);
            break;
        case VP_MSG_HIDE:
            if (got_fd >= 0) close(got_fd);
            hide_chain(m->chain);
            break;
        case VP_MSG_RESTACK:
            if (got_fd >= 0) close(got_fd);
            for (int i = 0; i < VP_LAYERS; i++) {
                if (g_show[i].chain != m->chain) continue;
                g_show[i].key = ((uint64_t)m->key_hi << 32) | m->key_lo;
                g_show[i].toplevel = m->toplevel;
                g_show[i].report = ++g_report_seq;
                restack(-1);
            }
            break;
        case VP_MSG_CHAIN_DESTROY: {
            if (got_fd >= 0) close(got_fd);
            hide_chain(m->chain);
            struct vp_chain *c = chain_of(m->chain);
            if (c) chain_free(c);
            break;
        }
        default:
            if (got_fd >= 0) close(got_fd);
            break;
    }
}

/* Everything the server sent so far is handled in one go and its layer changes applied in one
 * transaction; at most VP_BATCH_MAX messages, so a flood cannot hold up the event loop. */
#define VP_BATCH_MAX 64
static int on_conn(int fd, uint32_t mask, void *data) {
    (void)data;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) { drop_connection(); return 0; }
    g_batching = 1;
    int n = 0;
    for (; n < VP_BATCH_MAX; n++) {
        struct vp_msg m;
        int got_fd;
        int r = recv_msg(fd, &m, &got_fd);
        if (r > 0) break;
        if (r < 0) { drop_connection(); break; }
        on_msg(fd, &m, got_fd);
    }
    g_batching = 0;
    if (g_trace && g_batch) droiddeck_log("vortek", "trace t=%.4f applied %d message%s", now_s(), n, n == 1 ? "" : "s");
    batch_apply();
    return 0;
}

static int on_listen(int fd, uint32_t mask, void *data) {
    (void)mask; (void)data;
    int c = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
    if (c < 0) return 0;
    if (g_conn_fd >= 0) drop_connection();
    pthread_mutex_lock(&g_conn_lock);
    g_conn_fd = c;
    pthread_mutex_unlock(&g_conn_lock);
    g_conn_src = wl_event_loop_add_fd(g_loop, c, WL_EVENT_READABLE, on_conn, NULL);
    const char *trace = getenv("VORTEK_PRESENT_TRACE");
    g_trace = trace && atoi(trace) > 0;
    if (!g_accept) send_simple(VP_MSG_MODE, 0, 0);
    droiddeck_log("vortek", "zero-copy presenter connected");
    return 0;
}

void vortek_present_init(struct wl_event_loop *loop) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt || !sc_layer_available()) return;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if ((size_t)snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/%s", rt, VORTEK_PRESENT_SOCKET_NAME) >= sizeof(addr.sun_path))
        return;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return;
    unlink(addr.sun_path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 2) != 0) {
        droiddeck_log("vortek", "zero-copy socket %s unavailable: %s", addr.sun_path, strerror(errno));
        close(fd);
        return;
    }
    g_loop = loop;
    g_listen_fd = fd;
    g_listen_src = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE, on_listen, NULL);
}

void vortek_present_scene_size(int w, int h) {
    g_scene_w = w;
    g_scene_h = h;
}

unsigned vortek_present_stats_take(void) {
    unsigned n = g_stat_frames;
    g_stat_frames = 0;
    return n;
}
