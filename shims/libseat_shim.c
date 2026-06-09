/*
 * libseat_shim.c -- LD_PRELOAD shim for running phoc/phosh on FuriOS
 * with a Mali GPU via the Android HWComposer2 (hwcomposer) wlroots backend.
 *
 * 1. LIBSEAT -- fake API opening devices with O_NONBLOCK (critical: without
 *    it the GLib main loop blocks in evdev_read and frames never fire).
 * 2. DRM CAPS -- patch drmGetDevice2 to advertise a render node; patch
 *    drmGetCap for PRIME/vblank/timestamp; stub drmModeCreateLease.
 * 3. EGL VISUAL-ID -- return HAL_PIXEL_FORMAT_RGBA_8888 (1) for RGBA8888
 *    configs where Android EGL returns 0, so wlroots can select a config.
 * 4. HWC2 VSYNC -- always return HWC2_ERROR_NONE from set_vsync_enabled so
 *    on_vsync_timer_elapsed calls schedule_frame() and phosh gets frames.
 * 5. BUFFER SHIMS -- force 2 HWC buffers; discard redundant acquire fences.
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <dlfcn.h>
#include <xf86drm.h>
#include <EGL/egl.h>

/* -- 1. LIBSEAT -- */
#define MAX_DEVICES 32
static struct { int device_id; int fd; } devices[MAX_DEVICES];
static int next_device_id = 1;

static void devices_init(void) {
    static int done = 0;
    if (done) return;
    for (int i = 0; i < MAX_DEVICES; i++) devices[i].fd = -1;
    done = 1;
}
static void track_device(int device_id, int fd) {
    devices_init();
    for (int i = 0; i < MAX_DEVICES; i++)
        if (devices[i].fd == -1) {
            devices[i].device_id = device_id; devices[i].fd = fd; return;
        }
    fprintf(stderr, "libseat_shim: device table full, closing fd %d\n", fd);
    close(fd);
}
static int get_fd_for_device(int device_id) {
    devices_init();
    for (int i = 0; i < MAX_DEVICES; i++)
        if (devices[i].fd != -1 && devices[i].device_id == device_id)
            return devices[i].fd;
    return -1;
}
static void untrack_device(int device_id) {
    devices_init();
    for (int i = 0; i < MAX_DEVICES; i++)
        if (devices[i].fd != -1 && devices[i].device_id == device_id) {
            devices[i].device_id = 0; devices[i].fd = -1; return;
        }
}

struct libseat;
struct libseat_seat_listener {
    void (*enable_seat)(struct libseat *, void *);
    void (*disable_seat)(struct libseat *, void *);
};
struct fake_seat {
    const struct libseat_seat_listener *listener;
    void *userdata;
    int pipe_r, pipe_w;
};
static struct fake_seat _fake_seat;

struct libseat *libseat_open_seat(const struct libseat_seat_listener *l, void *u) {
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC | O_NONBLOCK) < 0) return NULL;
    _fake_seat.listener = l; _fake_seat.userdata = u;
    _fake_seat.pipe_r = pfd[0]; _fake_seat.pipe_w = pfd[1];
    if (l && l->enable_seat) l->enable_seat((struct libseat *)&_fake_seat, u);
    return (struct libseat *)&_fake_seat;
}
int libseat_open_device(struct libseat *s, const char *path, int *fd) {
    int f = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (f < 0) return -1;
    *fd = f;
    if (next_device_id <= 0) next_device_id = 1;
    int id = next_device_id++;
    track_device(id, f);
    return id;
}
int libseat_close_device(struct libseat *s, int id) {
    int fd = get_fd_for_device(id);
    if (fd >= 0) { close(fd); untrack_device(id); }
    return 0;
}
int         libseat_get_fd(struct libseat *s)            { return ((struct fake_seat *)s)->pipe_r; }
int         libseat_dispatch(struct libseat *s, int t)   { return 0; }
const char *libseat_seat_name(struct libseat *s)         { return "seat0"; }
int         libseat_close_seat(struct libseat *s)        { return 0; }
int         libseat_switch_session(struct libseat *s, int n) { return 0; }
int         libseat_disable_seat(struct libseat *s)      { return 0; }

/* -- 2. DRM CAPS -- */
char *drmGetRenderDeviceNameFromFd(int fd) { return strdup("/dev/dri/card0"); }
int   drmGetNodeTypeFromFd(int fd)         { return DRM_NODE_PRIMARY; }
int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device) {
    static int (*real_fn)(int, uint32_t, drmDevicePtr *) = NULL;
    if (!real_fn) real_fn = dlsym(RTLD_NEXT, "drmGetDevice2");
    int r = real_fn(fd, flags, device);
    if (r == 0 && *device) {
        (*device)->available_nodes |= (1 << DRM_NODE_RENDER);
        (*device)->nodes[DRM_NODE_RENDER] = strdup((*device)->nodes[DRM_NODE_PRIMARY]);
    }
    return r;
}
int drmGetCap(int fd, uint64_t cap, uint64_t *value) {
    static int (*real_fn)(int, uint64_t, uint64_t *) = NULL;
    if (!real_fn) real_fn = dlsym(RTLD_NEXT, "drmGetCap");
    switch (cap) {
        case DRM_CAP_PRIME:                 *value = DRM_PRIME_CAP_IMPORT|DRM_PRIME_CAP_EXPORT; return 0;
        case DRM_CAP_CRTC_IN_VBLANK_EVENT: *value = 1; return 0;
        case DRM_CAP_TIMESTAMP_MONOTONIC:  *value = 1; return 0;
        default: return real_fn(fd, cap, value);
    }
}
int drmSetClientCap(int fd, uint64_t cap, uint64_t value) {
    static int (*real_fn)(int, uint64_t, uint64_t) = NULL;
    if (!real_fn) real_fn = dlsym(RTLD_NEXT, "drmSetClientCap");
    return real_fn(fd, cap, value);
}
int drmIsKMS(int fd) { return 1; }
int drmModeCreateLease(int fd, const uint32_t *o, int n, int f, uint32_t *id) { return -EINVAL; }

/* -- 3. EGL VISUAL-ID -- */
EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value) {
    static EGLBoolean (*real_fn)(EGLDisplay, EGLConfig, EGLint, EGLint *) = NULL;
    if (!real_fn) real_fn = dlsym(RTLD_NEXT, "eglGetConfigAttrib");
    EGLBoolean r = real_fn(dpy, config, attribute, value);
    if (r && attribute == EGL_NATIVE_VISUAL_ID && *value == 0) {
        EGLint red=0, green=0, blue=0, alpha=0;
        real_fn(dpy,config,EGL_RED_SIZE,&red); real_fn(dpy,config,EGL_GREEN_SIZE,&green);
        real_fn(dpy,config,EGL_BLUE_SIZE,&blue); real_fn(dpy,config,EGL_ALPHA_SIZE,&alpha);
        if (red==8 && green==8 && blue==8 && alpha==8) *value=1;
    }
    return r;
}

/* -- 4. HWC2 VSYNC -- */
typedef void hwc2_compat_display_t;
typedef int32_t hwc2_error_t;
#define HWC2_ERROR_NONE 0
hwc2_error_t hwc2_compat_display_set_vsync_enabled(hwc2_compat_display_t *display, int32_t enabled) {
    static hwc2_error_t (*real_fn)(hwc2_compat_display_t *, int32_t) = NULL;
    if (!real_fn) real_fn = dlsym(RTLD_NEXT, "hwc2_compat_display_set_vsync_enabled");
    if (real_fn) real_fn(display, enabled);
    return HWC2_ERROR_NONE;
}

/* -- 5. BUFFER SHIMS -- */
typedef void HWCNativeWindow;
void HWCNativeWindowSetBufferCount(HWCNativeWindow *win, int count) {
    static void (*real_fn)(HWCNativeWindow *, int) = NULL;
    if (!real_fn) real_fn = dlsym(RTLD_NEXT, "HWCNativeWindowSetBufferCount");
    if (real_fn) real_fn(win, 2);
}
typedef void ANativeWindowBuffer;
void HWCNativeBufferSetFence(ANativeWindowBuffer *buffer, int fd) {
    static void (*real_fn)(ANativeWindowBuffer *, int) = NULL;
    if (!real_fn) real_fn = dlsym(RTLD_NEXT, "HWCNativeBufferSetFence");
    if (real_fn) real_fn(buffer, -1);
    if (fd >= 0) close(fd);
}
