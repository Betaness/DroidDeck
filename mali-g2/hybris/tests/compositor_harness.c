/*
 * DroidDeck's compositor-side zero-copy code (app/src/main/cpp/waylandcomp/src/vortek_present.c)
 * built for a glibc host, with SurfaceControl and the upscaler stubbed out: each Vortek layer "shows"
 * the last buffer presented on it and hands the one before back, as a transaction completing would.
 * Buffers arrive through libhybris (src/ahb.c). Serves $XDG_RUNTIME_DIR/vortek-present until argv[1]
 * presenters (default 1) have disconnected, then prints per presenter what it showed and
 * "HARNESS-RESULT ok|fail".
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include "sc_layer.h"
#include "vortek_present.h"
#include "vortek_upscale.h"

static int disconnected, refused;
static unsigned frames;
static void* shown[SC_VORTEK_LAYERS];
static unsigned perPresenter[8];

void droiddeck_log(const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("[%s] ", tag);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (strstr(fmt, "presenter %d disconnected")) disconnected++;
    if (strstr(fmt, "zero-copy refused") && !strstr(fmt, "id too large")) refused++; /* that one: presenter_probe */
}

void droiddeck_count_game_frame(void) { frames++; }

int sc_layer_available(void) { return 1; }
struct ASurfaceTransaction* sc_layer_tx_begin(void) { return (struct ASurfaceTransaction*)1; }
void sc_layer_tx_apply(struct ASurfaceTransaction* batch) { (void)batch; }
void sc_layer_vortek_z(int slot, int z, struct ASurfaceTransaction* batch) { (void)slot; (void)z; (void)batch; }

int sc_layer_present_vortek(int slot, int z, AHardwareBuffer* ahb, int buf_w, int buf_h, int acquire_fd, void* token, int x, int y,
                            int w, int h, int scene_w, int scene_h, struct ASurfaceTransaction* batch) {
    if (acquire_fd >= 0) {
        struct pollfd p = {acquire_fd, POLLIN, 0};
        poll(&p, 1, 1000);
        close(acquire_fd);
    }
    if (shown[slot] && shown[slot] != token) vortek_present_released(shown[slot], -1);
    shown[slot] = token;
    uint64_t t = (uint64_t)(uintptr_t)token;
    unsigned presenter = (unsigned)(t >> (8 + 29)) & 7;
    perPresenter[presenter]++;
    return 0;
}

void sc_layer_hide_vortek(int slot, struct ASurfaceTransaction* batch) {
    if (shown[slot]) vortek_present_released(shown[slot], -1);
    shown[slot] = NULL;
}

void sc_layer_hide_vortek_unused(unsigned in_use) {
    for (int i = 0; i < SC_VORTEK_LAYERS; i++)
        if (!(in_use & (1u << i))) sc_layer_hide_vortek(i, NULL);
}

void sc_layer_hide_all_vortek(void) { sc_layer_hide_vortek_unused(0); }

int vku_present(const struct vku_frame* f, int acquire_fd, struct ASurfaceTransaction* batch, const char** why) { return VKU_DIRECT; }
void vku_init(struct wl_event_loop* loop) { (void)loop; }
void vku_layer_hidden(int slot) { (void)slot; }
void vku_chain_gone(uint32_t chain) { (void)chain; }

int main(int argc, char** argv) {
    int expected = argc > 1 ? atoi(argv[1]) : 1;
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct wl_event_loop* loop = wl_event_loop_create();
    vortek_present_scene_size(1280, 720);
    vortek_present_init(loop);
    while (disconnected < expected) wl_event_loop_dispatch(loop, 100);
    for (int i = 1; i < 8; i++)
        if (perPresenter[i]) printf("HARNESS presenter %d: %u frames shown\n", i, perPresenter[i]);
    printf("HARNESS %u frames counted, %d refusals\n", frames, refused);
    printf("HARNESS-RESULT %s\n", refused ? "fail" : "ok");
    return refused ? 1 : 0;
}
