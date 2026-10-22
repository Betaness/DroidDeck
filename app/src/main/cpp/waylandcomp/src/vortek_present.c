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

#define VP_MAX_CHAINS 8
#define VP_MAX_IMAGES 8

struct vp_chain {
    uint32_t id;
    uint32_t n, w, h;
    AHardwareBuffer *ahb[VP_MAX_IMAGES];
};

static struct vp_chain g_chains[VP_MAX_CHAINS];
static struct wl_event_loop *g_loop;
static struct wl_event_source *g_listen_src, *g_conn_src;
static int g_listen_fd = -1;
static pthread_mutex_t g_conn_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_conn_fd = -1;
static int g_scene_w, g_scene_h;
static unsigned g_stat_frames;
static uint32_t g_shown_chain;
static int32_t g_shown_rect[4];
static int g_shown;
static int g_first_logged;
static int g_accept = 1;
static struct wl_event_source *g_stall_timer;

/* A game that stops presenting (blocked on a dialog, hung) must not keep covering the X windows. */
#define VP_STALL_MS 2000

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
    do r = recvmsg(fd, &h, MSG_CMSG_CLOEXEC); while (r < 0 && errno == EINTR);
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

static void hide(void) {
    if (!g_shown) return;
    sc_layer_hide_vortek();
    g_shown = 0;
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
        if (g_shown && g_shown_chain == m->chain) hide();
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

static int rects_overlap(const int32_t a[4], const struct vp_msg *m) {
    return a[0] < m->x + m->w && m->x < a[0] + a[2] && a[1] < m->y + m->h && m->y < a[1] + a[3];
}

static void on_present(const struct vp_msg *m, int acquire_fd) {
    struct vp_chain *c = chain_of(m->chain);
    if (!c || m->index >= c->n || !c->ahb[m->index] || g_scene_w <= 0 || g_scene_h <= 0 || !g_accept) {
        if (acquire_fd >= 0) close(acquire_fd);
        send_release(m->chain, m->index, -1);
        if (!c) refuse(m->chain, "unknown swapchain");
        return;
    }
    /* One layer: a swapchain recreated for the same window takes it over, another window does not. */
    if (g_shown && g_shown_chain != m->chain && !rects_overlap(g_shown_rect, m)) {
        if (acquire_fd >= 0) close(acquire_fd);
        send_release(m->chain, m->index, -1);
        refuse(m->chain, "another window already uses the layer");
        return;
    }
    int x = m->x, y = m->y, w = m->w, h = m->h;
    if (m->root_w > 0 && m->root_h > 0 && ((int)m->root_w != g_scene_w || (int)m->root_h != g_scene_h)) {
        const double sx = (double)g_scene_w / m->root_w, sy = (double)g_scene_h / m->root_h;
        x = (int)(m->x * sx + 0.5); y = (int)(m->y * sy + 0.5);
        w = (int)((m->x + m->w) * sx + 0.5) - x; h = (int)((m->y + m->h) * sy + 0.5) - y;
    }
    int r = sc_layer_present_vortek(c->ahb[m->index], (int)c->w, (int)c->h, acquire_fd, token_of(m->chain, m->index),
                                    x, y, w, h, g_scene_w, g_scene_h);
    if (r != 0) {
        send_release(m->chain, m->index, -1);
        return;
    }
    g_shown = 1;
    g_shown_chain = m->chain;
    g_shown_rect[0] = m->x; g_shown_rect[1] = m->y; g_shown_rect[2] = m->w; g_shown_rect[3] = m->h;
    if (g_stall_timer) wl_event_source_timer_update(g_stall_timer, VP_STALL_MS);
    g_stat_frames++;
    if (!g_first_logged) {
        g_first_logged = 1;
        droiddeck_log("vortek", "game frames shown zero-copy: %ux%u buffer over the window at %d,%d %dx%d (X root %ux%u, scene %dx%d)",
                      c->w, c->h, m->x, m->y, m->w, m->h, m->root_w, m->root_h, g_scene_w, g_scene_h);
    }
}

static int on_stall(void *data) {
    (void)data;
    if (g_shown) {
        droiddeck_log("vortek", "no zero-copy frame for %d ms: showing the X windows again", VP_STALL_MS);
        hide();
    }
    return 0;
}

void vortek_present_set_accept(int accept) {
    accept = accept ? 1 : 0;
    if (accept == g_accept) return;
    g_accept = accept;
    if (!accept) hide();
    send_simple(VP_MSG_MODE, 0, (uint32_t)accept);
    droiddeck_log("vortek", "zero-copy %s", accept ? "resumed" : "paused: screen effects, frame generation or cardboard need the composited scene");
}

static int on_conn(int fd, uint32_t mask, void *data) {
    (void)data;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) { drop_connection(); return 0; }
    struct vp_msg m;
    int got_fd;
    if (recv_msg(fd, &m, &got_fd) != 0) { drop_connection(); return 0; }
    switch (m.type) {
        case VP_MSG_CHAIN_CREATE:
            if (got_fd >= 0) close(got_fd);
            on_create(fd, &m);
            break;
        case VP_MSG_PRESENT:
            on_present(&m, got_fd);
            break;
        case VP_MSG_HIDE:
            if (got_fd >= 0) close(got_fd);
            if (g_shown && g_shown_chain == m.chain) hide();
            break;
        case VP_MSG_CHAIN_DESTROY: {
            if (got_fd >= 0) close(got_fd);
            if (g_shown && g_shown_chain == m.chain) hide();
            struct vp_chain *c = chain_of(m.chain);
            if (c) chain_free(c);
            break;
        }
        default:
            if (got_fd >= 0) close(got_fd);
            break;
    }
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
    g_stall_timer = wl_event_loop_add_timer(loop, on_stall, NULL);
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
