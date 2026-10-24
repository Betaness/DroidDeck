#ifndef FRAME_SCHED_FEED_H
#define FRAME_SCHED_FEED_H

/* [mali-g2] The game's frames reported to HyperOS's frame governor (SchedGenius) on the Xring O3.
 *
 * libmigui reports an app's BufferQueue frames through /dev/xr_fbt (libxfbt is a thin ioctl wrapper),
 * and SchedGenius sizes CPU and GPU clocks per reported layer against its expected frame time. The
 * game's frames never go through a BufferQueue here: the compositor shows them on a SurfaceControl
 * layer (droiddeck_vortek_game), so this reports them for that layer, the same three ioctls libmigui
 * issues: the frame rate the layer should keep, and a queue (with the frame's acquire fence) then the
 * next frame's dequeue for each frame shown. Off unless the app turns it on; event-loop thread only,
 * except the two setters. */

enum { FSF_OFF = 0, FSF_COMPOSITOR = 1, FSF_GAME_THREAD = 2 };

/* FSF_COMPOSITOR names the presenting thread; FSF_GAME_THREAD the thread set below, if the kernel takes it. */
void fsf_set_mode(int mode);
/* The game's busiest thread (from the app's thread scan), for FSF_GAME_THREAD. */
void fsf_set_game_thread(int pid, int tid);
int fsf_enabled(void);
/* A frame of the game's window shown on the Vortek layer whose SurfaceControl is `surface_control`
 * (an ASurfaceControl, which points at an android::SurfaceControl). fence_fd: its acquire fence, not
 * consumed (-1: none). images: the swapchain's buffers. target_fps <= 0: 60. */
void fsf_game_frame(const void *surface_control, const char *layer_name, int fence_fd, int images, float target_fps);

#endif
