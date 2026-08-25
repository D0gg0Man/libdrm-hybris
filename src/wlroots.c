/*
 * wlroots.c -- pieces that exist for wlroots-based compositors (phoc/phosh).
 *
 * libseat: phoc expects to acquire devices through libseat, but on FuriOS it
 * runs outside seatd and the session has already opened everything it needs.
 * This shim is installed as libseat.so.1 so those calls resolve here and are
 * answered locally instead of failing. Nothing else in the system uses it --
 * mutter and KWin take their own paths.
 */

#define _GNU_SOURCE

#include "common.h"
#include "wlroots.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <EGL/egl.h>

/* ==========================================================================
 * 1. LIBSEAT -- fake API for phosh/phoc running outside seatd
 * ========================================================================== */

#define MAX_DEVICES 32
static struct { int device_id; int fd; } devices[MAX_DEVICES] = {
    [0 ... MAX_DEVICES-1] = { .device_id = 0, .fd = -1 }
};
static int next_device_id = 1;

static void track_device(int device_id, int fd) {
    for (int i = 0; i < MAX_DEVICES; i++)
        if (devices[i].fd == -1) {
            devices[i].device_id = device_id; devices[i].fd = fd; return;
        }
    LOG_WARN("device table full, closing fd %d", fd);
    close(fd);
}
static int get_fd_for_device(int device_id) {
    for (int i = 0; i < MAX_DEVICES; i++)
        if (devices[i].fd != -1 && devices[i].device_id == device_id)
            return devices[i].fd;
    return -1;
}
static void untrack_device(int device_id) {
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
    (void)s;   /* the fake seat is a singleton; the handle carries no state */
    /* O_NONBLOCK is critical -- without it the GLib main loop blocks in
     * evdev_read and the wlroots frame timer callbacks never fire. */
    int f = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (f < 0) return -1;
    *fd = f;
    if (next_device_id <= 0) next_device_id = 1;
    int id = next_device_id++;
    track_device(id, f);
    return id;
}
int libseat_close_device(struct libseat *s, int id) {
    (void)s;
    int fd = get_fd_for_device(id);
    if (fd >= 0) { close(fd); untrack_device(id); }
    return 0;
}
int         libseat_get_fd(struct libseat *s)                { return ((struct fake_seat *)s)->pipe_r; }
int         libseat_dispatch(struct libseat *s, int t)       { (void)s; (void)t; return 0; }
const char *libseat_seat_name(struct libseat *s)             { (void)s; return "seat0"; }
int         libseat_close_seat(struct libseat *s)            { (void)s; return 0; }
int         libseat_switch_session(struct libseat *s, int n) { (void)s; (void)n; return 0; }
int         libseat_disable_seat(struct libseat *s)          { (void)s; return 0; }
void        libseat_set_log_handler(void *handler, void *data) { (void)handler; (void)data; }
void        libseat_set_log_level(int level)                 { (void)level; }


/* ---- EGL visual id -------------------------------------------------------
 * wlroots needs a non-zero EGL_NATIVE_VISUAL_ID to pick a config, and the
 * hybris EGL reports 0. Fill one in for 8888 configs.
 *
 * Deliberately NOT applied elsewhere: ordinary clients (Qt camera apps) need
 * the unmodified value, and under mutter the drmadapter platform does the
 * proper fourcc mapping itself -- forcing 1 there breaks its GBM format match
 * ("No EGL config matching supported GBM format found"). */

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config,
                               EGLint attribute, EGLint *value) {
    static EGLBoolean (*real_fn)(EGLDisplay, EGLConfig, EGLint, EGLint *) = NULL;
    if (!real_fn) real_fn = hybris_resolve_next("eglGetConfigAttrib", (void *)eglGetConfigAttrib);
    if (!real_fn) return EGL_FALSE;
    EGLBoolean r = real_fn(dpy, config, attribute, value);
    /* Visual-id fix is ONLY for wlroots/phoc (phosh), which needs a non-zero
     * EGL_NATIVE_VISUAL_ID to select a config. It must NOT run for:
     *  - clients (Qt camera apps) -- they need the unmodified value
     *  - gnome/mutter -- the drmadapter EGL platform does the proper fourcc
     *    mapping itself; our forcing it to 1 breaks mutter's GBM format match
     *    ("No EGL config matching supported GBM format found"). */
    if (r && hybris_is_compositor() && !hybris_is_gnome() &&
        attribute == EGL_NATIVE_VISUAL_ID && *value == 0) {
        EGLint red=0, green=0, blue=0, alpha=0;
        real_fn(dpy,config,EGL_RED_SIZE,&red);   real_fn(dpy,config,EGL_GREEN_SIZE,&green);
        real_fn(dpy,config,EGL_BLUE_SIZE,&blue); real_fn(dpy,config,EGL_ALPHA_SIZE,&alpha);
        if (red==8 && green==8 && blue==8 && alpha==8) *value = 1;
    }
    return r;
}

