#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <time.h>
#include <sys/syscall.h>
#include "droiddeck_ext.h"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#define F_SEAL_WRITE 0x0008
#endif

/* [mali-g2] Committed text (Android IME, Steam's keyboard) for X programs of the rootful Xwayland,
 * which has no text-input. As gamescope's ime.cpp: characters keymap.xkb cannot type get Unicode
 * keysyms (U4E2D) on character keys of a temporary keymap sent only to Xwayland, which passes it to
 * X clients, then each key is pressed and released. keymap.xkb comes back RESTORE_MS after the last
 * commit: clients that look keysyms up late (Steam) would read keys already sent through it. */

#define RESTORE_MS 300
#define MAX_KBS 8

#ifndef KEY_BACKSPACE
#define KEY_BACKSPACE 14
#endif
#ifndef KEY_TAB
#define KEY_TAB 15
#endif
#ifndef KEY_ENTER
#define KEY_ENTER 28
#endif
#define KEY_LEFTSHIFT 42
#define KEY_SPACE 57

/* gamescope's allow_keycodes: some clients (Wine) read evdev meaning into keycodes, so only keys
 * that type characters anyway carry the temporary ones. Names as in keymap.xkb's xkb_keycodes. */
static const struct { uint8_t evdev; char name[5]; } slot_keys[] = {
    { 2, "AE01" }, { 3, "AE02" }, { 4, "AE03" }, { 5, "AE04" }, { 6, "AE05" }, { 7, "AE06" },
    { 8, "AE07" }, { 9, "AE08" }, { 10, "AE09" }, { 11, "AE10" }, { 12, "AE11" }, { 13, "AE12" },
    { 16, "AD01" }, { 17, "AD02" }, { 18, "AD03" }, { 19, "AD04" }, { 20, "AD05" }, { 21, "AD06" },
    { 22, "AD07" }, { 23, "AD08" }, { 24, "AD09" }, { 25, "AD10" }, { 26, "AD11" }, { 27, "AD12" },
    { 30, "AC01" }, { 31, "AC02" }, { 32, "AC03" }, { 33, "AC04" }, { 34, "AC05" }, { 35, "AC06" },
    { 36, "AC07" }, { 37, "AC08" }, { 38, "AC09" }, { 39, "AC10" }, { 40, "AC11" }, { 41, "TLDE" },
    { 43, "BKSL" }, { 44, "AB01" }, { 45, "AB02" }, { 46, "AB03" }, { 47, "AB04" }, { 48, "AB05" },
    { 49, "AB06" }, { 50, "AB07" }, { 51, "AB08" }, { 52, "AB09" }, { 53, "AB10" },
};
#define NSLOTS ((int)(sizeof(slot_keys) / sizeof(slot_keys[0])))

/* keymap.xkb is pc+us: the character of evdev keys 0..57 without and with Shift. */
static const char us_plain[] = "\0\0" "1234567890-=" "\0\0" "qwertyuiop[]" "\0\0" "asdfghjkl;'`" "\0"
                               "\\zxcvbnm,./" "\0\0\0" " ";
static const char us_shift[] = "\0\0" "!@#$%^&*()_+" "\0\0" "QWERTYUIOP{}" "\0\0" "ASDFGHJKL:\"~" "\0"
                               "|ZXCVBNM<>?" "\0\0\0" " ";

static struct wl_display *g_disp;
static struct wl_event_source *g_timer;
static char *g_map;
static size_t g_map_len, g_map_split;
static uint32_t g_key_cp[NSLOTS];
static uint32_t g_key_real_ms[NSLOTS];
static int g_nslots;
static int g_temp;
static struct wl_client *g_client;
static struct wl_listener g_client_gone;
static uint32_t *g_pending;
static size_t g_npending, g_pending_cap;

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int is_xwayland(struct wl_client *c) { return !strncmp(droiddeck_client_name(c), "Xwayland", 8); }

static int ascii_key(uint32_t cp, uint32_t *evdev, int *shift) {
    if (cp < 0x20 || cp > 0x7e) return 0;
    for (uint32_t k = 1; k < sizeof(us_plain) - 1; k++) {
        if ((unsigned char)us_plain[k] != cp && (unsigned char)us_shift[k] != cp) continue;
        *evdev = k;
        *shift = (unsigned char)us_plain[k] != cp;
        if ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z') *shift ^= (droiddeck_locked_mods() & 0x2) != 0;
        return 1;
    }
    return 0;
}

/* Characters typed by keys no temporary keymap reassigns, so they never need one. */
static int direct_key(uint32_t cp, uint32_t *evdev) {
    switch (cp) {
    case '\n': *evdev = KEY_ENTER; return 1;
    case '\t': *evdev = KEY_TAB; return 1;
    case '\b': *evdev = KEY_BACKSPACE; return 1;
    case ' ': *evdev = KEY_SPACE; return 1;
    }
    return 0;
}

static int slot_of(uint32_t cp) {
    for (int i = 0; i < NSLOTS; i++) if (g_key_cp[i] == cp) return i;
    return -1;
}

/* Not a key pressed for real within RESTORE_MS: clients may not have read that press yet. */
static int free_slot(void) {
    uint32_t now = now_ms();
    for (int i = 0; i < NSLOTS; i++)
        if (!g_key_cp[i] && now - g_key_real_ms[i] >= RESTORE_MS) return i;
    return -1;
}

static void clear_slots(void) {
    memset(g_key_cp, 0, sizeof(g_key_cp));
    g_nslots = 0;
}

/* Code points of utf8: invalid sequences and controls without a key dropped, CR LF as one Enter. */
static size_t decode(const char *utf8, size_t len, uint32_t *out) {
    const unsigned char *s = (const unsigned char *)utf8;
    size_t n = 0;
    int after_cr = 0;
    for (size_t i = 0; i < len;) {
        unsigned char b = s[i];
        size_t size = b < 0x80 ? 1 : (b & 0xe0) == 0xc0 ? 2 : (b & 0xf0) == 0xe0 ? 3 : (b & 0xf8) == 0xf0 ? 4 : 0;
        if (!size || i + size > len) { i++; continue; }
        uint32_t cp = size == 1 ? b : b & (0x7fu >> size), ev;
        size_t j;
        for (j = 1; j < size && (s[i + j] & 0xc0) == 0x80; j++) cp = cp << 6 | (s[i + j] & 0x3f);
        i += j;
        if (j < size || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) continue;
        int lf_after_cr = cp == '\n' && after_cr;
        after_cr = cp == '\r';
        if (lf_after_cr) continue;
        if (cp == '\r') cp = '\n';
        if ((cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) && !direct_key(cp, &ev)) continue;
        out[n++] = cp;
    }
    return n;
}

static int load_map(void) {
    if (g_map) return 1;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char path[512];
    snprintf(path, sizeof(path), "%s/keymap.xkb", rt ? rt : ".");
    FILE *f = fopen(path, "re");
    if (!f) { droiddeck_log("text-input", "cannot type for X programs: %s: %s", path, strerror(errno)); return 0; }
    char *buf = NULL;
    size_t cap = 0, len = 0, got;
    do {
        if (cap - len < 65536) {
            char *nb = realloc(buf, cap += 65536);
            if (!nb) { free(buf); fclose(f); return 0; }
            buf = nb;
        }
        got = fread(buf + len, 1, cap - len - 1, f);
        len += got;
    } while (got > 0);
    fclose(f);
    buf[len] = 0;
    /* The temporary keys go at the end of xkb_symbols: "replace key" drops the key's keymap.xkb
     * definition, so every other key (Enter, Backspace, Shift...) types as usual meanwhile. */
    const char *sym = strstr(buf, "xkb_symbols");
    const char *end = sym ? strstr(sym, "\n};") : NULL;
    if (!end) { droiddeck_log("text-input", "cannot type for X programs: %s has no xkb_symbols", path); free(buf); return 0; }
    g_map = buf;
    g_map_len = len;
    g_map_split = (size_t)(end + 1 - buf);
    return 1;
}

static int send_keymap(struct wl_client *c, const char *text, size_t len) {
    int fd = (int)syscall(__NR_memfd_create, "droiddeck-x11-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) { droiddeck_log("text-input", "keymap for Xwayland: memfd_create failed (%s)", strerror(errno)); return 0; }
    for (size_t off = 0; off < len + 1;) {
        ssize_t w = write(fd, text + off, len + 1 - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { droiddeck_log("text-input", "keymap for Xwayland: write failed (%s)", strerror(errno)); close(fd); return 0; }
        off += (size_t)w;
    }
    fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE);
    struct wl_resource *kbs[MAX_KBS];
    int n = droiddeck_client_keyboards(c, kbs, MAX_KBS);
    for (int i = 0; i < n; i++)
        wl_keyboard_send_keymap(kbs[i], WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)(len + 1));
    close(fd);
    return 1;
}

static int send_temp_keymap(struct wl_client *c) {
    size_t cap = g_map_len + (size_t)g_nslots * 96 + 1, off = g_map_split;
    char *text = malloc(cap);
    if (!text) return 0;
    memcpy(text, g_map, g_map_split);
    for (int i = 0; i < NSLOTS; i++) {
        uint32_t cp = g_key_cp[i];
        if (!cp) continue;
        off += (size_t)snprintf(text + off, cap - off, "\treplace key <%s> { type= \"ONE_LEVEL\", symbols[1]= [ U%04X ] };\n",
                                slot_keys[i].name, cp);
    }
    memcpy(text + off, g_map + g_map_split, g_map_len - g_map_split + 1);
    int ok = send_keymap(c, text, off + g_map_len - g_map_split);
    free(text);
    return ok;
}

static void on_client_gone(struct wl_listener *l, void *data) {
    wl_list_remove(&l->link);
    g_client = NULL;
    g_temp = 0;
    clear_slots();
    g_npending = 0;
}

static void restore(void) {
    if (g_temp && g_client && send_keymap(g_client, g_map, g_map_len))
        droiddeck_log("text-input", "keymap.xkb back on %s (%d temporary keys)", droiddeck_client_name(g_client), g_nslots);
    g_temp = 0;
    clear_slots();
    if (g_client) { wl_list_remove(&g_client_gone.link); g_client = NULL; }
}

static void pump(void) {
    struct wl_client *c = droiddeck_key_target_client(1);
    if (!c || !is_xwayland(c)) {
        droiddeck_log("text-input", "%zu typed chars dropped: Xwayland no longer has the keyboard", g_npending);
        g_npending = 0;
        restore();
        return;
    }
    if (c != g_client) {
        restore();
        g_client = c;
        g_client_gone.notify = on_client_gone;
        wl_client_add_destroy_listener(c, &g_client_gone);
    }
    droiddeck_release_keys();
    size_t n;
    int added = 0;
    uint32_t ev;
    for (n = 0; n < g_npending; n++) {
        uint32_t cp = g_pending[n];
        if (direct_key(cp, &ev) || slot_of(cp) >= 0) continue;
        /* Out of keys: the rest waits for a fresh map after RESTORE_MS, as reusing a key now would
         * change what the keys just sent mean to clients that have not read them yet. */
        int k = free_slot();
        if (k < 0) break;
        g_key_cp[k] = cp;
        g_nslots++;
        added = 1;
    }
    if (added) {
        if (!send_temp_keymap(c)) { g_npending = 0; restore(); return; }
        g_temp = 1;
    }
    struct wl_resource *kbs[MAX_KBS];
    int nk = droiddeck_client_keyboards(c, kbs, MAX_KBS);
    uint32_t t = now_ms();
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = g_pending[i];
        if (direct_key(cp, &ev)) { x11_key_action(ev); continue; }
        uint32_t key = slot_keys[slot_of(cp)].evdev;
        for (int k = 0; k < nk; k++) {
            wl_keyboard_send_key(kbs[k], wl_display_next_serial(g_disp), t, key, WL_KEYBOARD_KEY_STATE_PRESSED);
            wl_keyboard_send_key(kbs[k], wl_display_next_serial(g_disp), t, key, WL_KEYBOARD_KEY_STATE_RELEASED);
        }
    }
    if (n) droiddeck_log("text-input", "typed %zu chars into %s through a temporary keymap (%d keys)%s", n,
                         droiddeck_client_name(c), g_nslots, n < g_npending ? ", the rest after a pause" : "");
    memmove(g_pending, g_pending + n, (g_npending - n) * sizeof(*g_pending));
    g_npending -= n;
    wl_event_source_timer_update(g_timer, RESTORE_MS);
}

static int on_timer(void *data) {
    if (g_npending) {
        clear_slots();
        pump();
    } else {
        restore();
    }
    wl_display_flush_clients(g_disp);
    return 0;
}

void x11_key_action(uint32_t evdev) {
    droiddeck_inject_key(evdev, 1);
    droiddeck_inject_key(evdev, 0);
}

int x11_type_text(const char *utf8, size_t len) {
    struct wl_client *c = droiddeck_key_target_client(0);
    if (!c) return 0;
    int xw = is_xwayland(c) && g_timer;
    uint32_t *cps = malloc((len + 1) * sizeof(*cps)), ev;
    if (!cps) return 1;
    size_t n = decode(utf8, len, cps), typed = 0;
    if (!n) { free(cps); return 1; }
    int plain = !g_temp && !g_npending, keys = 0;
    int shift;
    for (size_t i = 0; i < n; i++)
        if (direct_key(cps[i], &ev) || ascii_key(cps[i], &ev, &shift)) keys = 1; else plain = 0;
    if (!xw && !keys) { free(cps); return 0; }
    /* Not Xwayland (gamescope's own X, a Wayland program without text-input): what keymap.xkb types
     * goes as key presses and the rest is dropped, as KeyboardHost did before it sent such text here. */
    if (plain || !xw) {
        droiddeck_release_keys();
        for (size_t i = 0; i < n; i++) {
            if (direct_key(cps[i], &ev)) { x11_key_action(ev); typed++; continue; }
            if (!ascii_key(cps[i], &ev, &shift)) continue;
            if (shift) droiddeck_inject_key(KEY_LEFTSHIFT, 1);
            x11_key_action(ev);
            if (shift) droiddeck_inject_key(KEY_LEFTSHIFT, 0);
            typed++;
        }
        free(cps);
        if (typed < n)
            droiddeck_log("text-input", "typed %zu of %zu chars into %s as key presses, the rest dropped: it has no "
                          "text input", typed, n, droiddeck_client_name(c));
        else
            droiddeck_log("text-input", "typed %zu chars into %s as key presses", n, droiddeck_client_name(c));
        return 1;
    }
    if (!load_map()) { free(cps); return 1; }
    if (g_npending + n > g_pending_cap) {
        size_t cap = g_npending + n + 64;
        uint32_t *np = realloc(g_pending, cap * sizeof(*np));
        if (!np) { free(cps); return 1; }
        g_pending = np;
        g_pending_cap = cap;
    }
    memcpy(g_pending + g_npending, cps, n * sizeof(*cps));
    g_npending += n;
    free(cps);
    /* Behind text still waiting for keys: the timer pump() armed types it, not re-armed here so
     * steady typing cannot hold it back. */
    if (g_npending == n) pump();
    return 1;
}

/* A real key press: one the temporary keymap reassigned would type the wrong text. */
void x11_type_key_pressed(uint32_t evdev) {
    for (int i = 0; i < NSLOTS; i++) {
        if (slot_keys[i].evdev != evdev) continue;
        g_key_real_ms[i] = now_ms();
        if (g_key_cp[i]) restore();
        return;
    }
}

void x11_type_init(struct wl_display *display) {
    g_disp = display;
    g_timer = wl_event_loop_add_timer(wl_display_get_event_loop(display), on_timer, NULL);
}
