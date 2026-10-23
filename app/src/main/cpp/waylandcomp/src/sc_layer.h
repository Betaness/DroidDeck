#ifndef SC_LAYER_H
#define SC_LAYER_H
#include <stddef.h>
#include <stdint.h>
#include <android/hardware_buffer.h>

struct vkp_image;
struct vkp_draw;
struct droiddeck_color;

/* [mali-g2] SC_VORTEK_LAYERS layers from SC_LAYER_VORTEK on, one per window shown zero-copy. */
enum sc_layer_id { SC_LAYER_GAME = 0, SC_LAYER_OVERLAY = 1, SC_LAYER_VORTEK = 2, SC_VORTEK_LAYERS = 4,
                   SC_LAYER_COUNT = SC_LAYER_VORTEK + SC_VORTEK_LAYERS };
/* z of the lowest Vortek layer: above GAME (1), the others follow; OVERLAY stays above them all. */
#define SC_VORTEK_Z_BASE 2
#define SC_OVERLAY_Z 16

int sc_layer_available(void);

void sc_layer_probe_dmabuf_fd(int fd);

int sc_layer_can_tag_hdr(void);
void sc_layer_hdr_symbols(char *out, size_t size);


int sc_layer_present(struct vkp_image *src, int scene_w, int scene_h, const struct droiddeck_color *color);

int sc_layer_present_pass(const struct vkp_draw *draws, int n, int scene_w, int scene_h);

struct vkp_hdr_frame;
int sc_layer_present_hdr_scene(const struct vkp_draw *draws, int n, const struct vkp_hdr_frame *hf,
                               int scene_w, int scene_h, const struct droiddeck_color *color);

int sc_layer_present_ahb(AHardwareBuffer *ahb, int w, int h, int acquire_fd, void *token,
                         int scene_w, int scene_h, const struct droiddeck_color *color, uint32_t ahb_format);

void sc_layer_set_frame_rate(float fps);

int sc_layer_overlay_affordable(void);

int sc_layer_present_overlay(struct vkp_image *src, const int geo[8], int translucent);

void sc_layer_hide(void);
/* [mali-g2] Vortek zero-copy: an AHB shown over the X window at x,y,w,h of the scene, on Vortek
 * layer `slot` (0 .. SC_VORTEK_LAYERS - 1) at z `z`. `batch`: a transaction from sc_layer_tx_begin
 * that the caller applies with sc_layer_tx_apply, so changes to several layers reach the screen in
 * the same frame (one buffer per layer and batch); NULL applies a transaction of its own. */
struct ASurfaceTransaction;
struct ASurfaceTransaction *sc_layer_tx_begin(void);
void sc_layer_tx_apply(struct ASurfaceTransaction *batch);
int sc_layer_present_vortek(int slot, int z, AHardwareBuffer *ahb, int buf_w, int buf_h, int acquire_fd, void *token,
                            int x, int y, int w, int h, int scene_w, int scene_h, struct ASurfaceTransaction *batch);
void sc_layer_hide_vortek(int slot, struct ASurfaceTransaction *batch);
void sc_layer_vortek_z(int slot, int z, struct ASurfaceTransaction *batch);
int sc_layer_base_black(int width, int height);
void sc_layer_hide_base(void);
void sc_layer_hide_overlay(void);

void sc_layer_window_gone(void);

unsigned sc_layer_frames_take(void);
unsigned sc_layer_drops_take(void);

#endif
