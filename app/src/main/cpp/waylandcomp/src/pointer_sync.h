#ifndef POINTER_SYNC_H
#define POINTER_SYNC_H

/* [mali-g2] When the compositor sends a client its absolute pointer position, and where the pointer is
 * on the output. No Wayland: tests/pointer_sync_test.c.
 *
 * Relative motion that a client's relative pointer consumed goes out without wl_pointer.motion, so
 * Xwayland posts raw motion only (POINTER_RAWONLY) and its X pointer stays where it was. X button and
 * wheel events carry that X pointer position, and winex11 moves the Windows cursor to it, so before
 * one of them such a client gets its position again. */

#define PTRSYNC_MOTION 1 /* send wl_pointer.motion */
#define PTRSYNC_BEHIND 2 /* send none: the client's absolute position falls behind */

/* What pointer_input does with wl_pointer.motion after an event. relative: a delta, not a position;
 * rel_consumed: a relative pointer of the client took that motion; moved: the event moved the pointer;
 * desktop: the client is the desktop session. 0: nothing changes. */
int ptrsync_motion(int locked, int relative, int rel_consumed, int moved, int desktop);

/* 1 when a button or wheel event must first bring a client whose position is behind up to date. */
int ptrsync_catch_up(int locked, int behind);

/* A scene position as fractions of the output size: x = map[0] * scene_x + map[1],
 * y = map[2] * scene_y + map[3]. o0 and o1 are the output pixels of scene points (0,0) and (1,1)
 * when shown (shown != 0); otherwise the scene fills the ow x oh output. 0 when there is no scene. */
int ptrsync_scene_map(int shown, double o0x, double o0y, double o1x, double o1y, int ow, int oh,
                      int scene_w, int scene_h, double map[4]);

#endif
