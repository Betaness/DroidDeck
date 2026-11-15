#include "pointer_sync.h"

int ptrsync_motion(int locked, int relative, int rel_consumed, int moved, int desktop) {
    if (locked) return 0;
    if (relative && rel_consumed) return desktop ? 0 : PTRSYNC_BEHIND;
    if (!relative || moved) return PTRSYNC_MOTION;
    return 0;
}

int ptrsync_catch_up(int locked, int behind) {
    return !locked && behind;
}

int ptrsync_scene_map(int shown, double o0x, double o0y, double o1x, double o1y, int ow, int oh,
                      int scene_w, int scene_h, double map[4]) {
    if (scene_w <= 0 || scene_h <= 0) return 0;
    if (shown && ow > 0 && oh > 0 && o1x > o0x && o1y > o0y) {
        map[0] = (o1x - o0x) / ow;
        map[1] = o0x / ow;
        map[2] = (o1y - o0y) / oh;
        map[3] = o0y / oh;
    } else {
        map[0] = 1.0 / scene_w;
        map[1] = 0;
        map[2] = 1.0 / scene_h;
        map[3] = 0;
    }
    return 1;
}
