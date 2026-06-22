/*
 * libdrm-hybris.c -- unified LD_PRELOAD shim for FuriOS Mali/HWC2
 *
 * Covers both the phosh/hwcomposer session and the gnome-mali session.
 * Session-specific paths are gated on XDG_SESSION_DESKTOP at runtime.
 *
 * Sections:
 *   1. LIBSEAT       -- fake libseat API (O_NONBLOCK, critical for phosh)
 *   2. DRM CAPS      -- render node, PRIME, vblank, timestamp patches
 *   3. EGL           -- visual-id fix; gnome EGL platform intercepts
 *   4. WAYLAND       -- gnome: inject android_wlegl into wl_display
 *   5. HWC2 VSYNC    -- always return HWC2_ERROR_NONE
 *   6. BUFFER SHIMS  -- force 2 HWC buffers; discard redundant fences
 *   7. DRM IOCTLS    -- ADDFB2/ATOMIC/PAGE_FLIP intercepts + dumb buffer
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
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <stdarg.h>
#include <android/android-config.h>
#include <hybris/gralloc/gralloc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <wayland-server.h>

/* Process-scope gate. This library is loaded system-wide via ld.so.preload
 * and as libseat.so.1, so it lands inside ordinary EGL clients (Qt camera
 * apps, etc) too. Those must pass straight through -- our EGL/DRM/HWC2
 * intercepts are only correct inside the compositor process. We detect the
 * compositor by executable name; everything else gets pass-through behavior.
 *
 * libseat interception is the exception: it must always be active because
 * the whole point is that phoc (the compositor) calls libseat, and phoc IS
 * a compositor. Non-compositors that call libseat (rare) still get our fake,
 * which is harmless -- they would have used seatd otherwise. */
static int is_compositor(void) {
    static int cached = -1;
    if (cached != -1) return cached;
    char buf[256] = {0};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0) { cached = 0; return 0; }
    buf[n] = '\0';
    /* Match on the basename of known compositors that drive HWC2 directly */
    const char *base = strrchr(buf, '/');
    base = base ? base + 1 : buf;
    cached =
        strcmp(base, "phoc") == 0        ||
        strcmp(base, "gnome-shell") == 0 ||
        strcmp(base, "mutter") == 0      ||
        strcmp(base, "weston") == 0      ||
        strcmp(base, "wlroots") == 0     ||
        strstr(base, "kwin") != NULL     ||
        strcmp(base, "sway") == 0;
    return cached;
}

/* Runtime session detection */
static int is_gnome(void) {
    const char *d = getenv("XDG_SESSION_DESKTOP");
    if (d && strcmp(d, "gnome") == 0) return 1;
    /* gnome-mali unsets XDG_SESSION_DESKTOP before import-environment,
     * so fall back to XDG_CURRENT_DESKTOP. Exact match "GNOME" only --
     * phosh uses "Phosh:GNOME" which must NOT match. */
    d = getenv("XDG_CURRENT_DESKTOP");
    return d && strcmp(d, "GNOME") == 0;
}

/* BUG 4 fix: only inject wlegl into gnome-shell itself, not every
 * wayland server that happens to run in a gnome session */
static int is_gnome_shell(void) {
    char buf[256] = {0};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0) return 0;
    buf[n] = '\0';
    return strstr(buf, "gnome-shell") != NULL;
}

/* BUG 2 fix: resolve the real symbol safely. When this library is
 * installed both as libseat.so.1 AND via ld.so.preload, RTLD_NEXT from
 * the preloaded copy resolves to the libseat.so.1 copy of the SAME
 * function (at a different address), causing infinite recursion.
 * Detect duplicates by comparing the build of the resolved symbol's
 * library against our own using a marker symbol unique to this shim. */
static void *resolve_next(const char *name, void *self_addr) {
    void *fn = dlsym(RTLD_NEXT, name);
    if (!fn || fn == self_addr) return NULL;
    /* If the resolved copy lives in a library that also exports our
     * unique marker, it is another copy of this shim -- skip it. */
    Dl_info info;
    if (dladdr(fn, &info) && info.dli_fname) {
        void *h = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
        if (h) {
            int is_dup = dlsym(h, "libdrm_hybris_shim_marker") != NULL;
            dlclose(h);
            if (is_dup) return NULL;
        }
    }
    return fn;
}

/* Unique marker exported so resolve_next can identify copies of this shim */
int libdrm_hybris_shim_marker = 1;


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
    fprintf(stderr, "libdrm-hybris: device table full, closing fd %d\n", fd);
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
    int fd = get_fd_for_device(id);
    if (fd >= 0) { close(fd); untrack_device(id); }
    return 0;
}
int         libseat_get_fd(struct libseat *s)                { return ((struct fake_seat *)s)->pipe_r; }
int         libseat_dispatch(struct libseat *s, int t)       { return 0; }
const char *libseat_seat_name(struct libseat *s)             { return "seat0"; }
int         libseat_close_seat(struct libseat *s)            { return 0; }
int         libseat_switch_session(struct libseat *s, int n) { return 0; }
int         libseat_disable_seat(struct libseat *s)          { return 0; }
void        libseat_set_log_handler(void *handler, void *data) { (void)handler; (void)data; }
void        libseat_set_log_level(int level)                 { (void)level; }


/* ==========================================================================
 * 2. DRM CAPS -- render node advertisement, capability patches
 * ========================================================================== */

char *drmGetRenderDeviceNameFromFd(int fd) {
    if (!is_compositor()) {
        typedef char *(*fn_t)(int);
        fn_t real=(fn_t)resolve_next("drmGetRenderDeviceNameFromFd",
                                     (void*)drmGetRenderDeviceNameFromFd);
        return real ? real(fd) : NULL;
    }
    return strdup("/dev/dri/card0");
}
int drmGetNodeTypeFromFd(int fd) {
    if (!is_compositor()) {
        typedef int (*fn_t)(int);
        fn_t real=(fn_t)resolve_next("drmGetNodeTypeFromFd",(void*)drmGetNodeTypeFromFd);
        return real ? real(fd) : -1;
    }
    return DRM_NODE_PRIMARY;
}

int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device) {
    static int (*real_fn)(int, uint32_t, drmDevicePtr *) = NULL;
    if (!real_fn) real_fn = resolve_next("drmGetDevice2", (void *)drmGetDevice2);
    if (!real_fn) return -ENOSYS;
    int r = real_fn(fd, flags, device);
    if (r == 0 && *device && is_compositor()) {
        (*device)->available_nodes |= (1 << DRM_NODE_RENDER);
        (*device)->nodes[DRM_NODE_RENDER] = strdup((*device)->nodes[DRM_NODE_PRIMARY]);
    }
    return r;
}
int drmGetCap(int fd, uint64_t cap, uint64_t *value) {
    static int (*real_fn)(int, uint64_t, uint64_t *) = NULL;
    if (!real_fn) real_fn = resolve_next("drmGetCap", (void *)drmGetCap);
    if (is_compositor()) {
        switch (cap) {
            case DRM_CAP_PRIME:                 *value = DRM_PRIME_CAP_IMPORT|DRM_PRIME_CAP_EXPORT; return 0;
            case DRM_CAP_CRTC_IN_VBLANK_EVENT: *value = 1; return 0;
            case DRM_CAP_TIMESTAMP_MONOTONIC:  *value = 1; return 0;
            default: break;
        }
    }
    return real_fn ? real_fn(fd, cap, value) : -ENOSYS;
}
int drmSetClientCap(int fd, uint64_t cap, uint64_t value) {
    static int (*real_fn)(int, uint64_t, uint64_t) = NULL;
    if (!real_fn) real_fn = resolve_next("drmSetClientCap", (void *)drmSetClientCap);
    return real_fn ? real_fn(fd, cap, value) : -ENOSYS;
}
int drmIsKMS(int fd) {
    if (!is_compositor()) {
        typedef int (*fn_t)(int);
        fn_t real=(fn_t)resolve_next("drmIsKMS",(void*)drmIsKMS);
        return real ? real(fd) : 0;
    }
    return 1;
}
int drmModeCreateLease(int fd, const uint32_t *o, int n, int f, uint32_t *id) {
    if (!is_compositor()) {
        typedef int (*fn_t)(int,const uint32_t*,int,int,uint32_t*);
        fn_t real=(fn_t)resolve_next("drmModeCreateLease",(void*)drmModeCreateLease);
        return real ? real(fd,o,n,f,id) : -EINVAL;
    }
    return -EINVAL;
}


/* ==========================================================================
 * 3. EGL -- visual-id fix (phosh) + platform display intercepts (gnome)
 * ========================================================================== */

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config,
                               EGLint attribute, EGLint *value) {
    static EGLBoolean (*real_fn)(EGLDisplay, EGLConfig, EGLint, EGLint *) = NULL;
    if (!real_fn) real_fn = resolve_next("eglGetConfigAttrib", (void *)eglGetConfigAttrib);
    if (!real_fn) return EGL_FALSE;
    EGLBoolean r = real_fn(dpy, config, attribute, value);
    /* Visual-id fix is ONLY for wlroots/phoc (phosh), which needs a non-zero
     * EGL_NATIVE_VISUAL_ID to select a config. It must NOT run for:
     *  - clients (Qt camera apps) -- they need the unmodified value
     *  - gnome/mutter -- the drmadapter EGL platform does the proper fourcc
     *    mapping itself; our forcing it to 1 breaks mutter's GBM format match
     *    ("No EGL config matching supported GBM format found"). */
    if (r && is_compositor() && !is_gnome() &&
        attribute == EGL_NATIVE_VISUAL_ID && *value == 0) {
        EGLint red=0, green=0, blue=0, alpha=0;
        real_fn(dpy,config,EGL_RED_SIZE,&red);   real_fn(dpy,config,EGL_GREEN_SIZE,&green);
        real_fn(dpy,config,EGL_BLUE_SIZE,&blue); real_fn(dpy,config,EGL_ALPHA_SIZE,&alpha);
        if (red==8 && green==8 && blue==8 && alpha==8) *value = 1;
    }
    return r;
}

/* ==========================================================================
 * 4. WAYLAND -- gnome: inject android_wlegl into wl_display on creation
 * ========================================================================== */

typedef void *(*server_wlegl_create_t)(struct wl_display *);

struct wl_display *wl_display_create(void) {
    typedef struct wl_display *(*fn_t)(void);
    fn_t real = resolve_next("wl_display_create", (void *)wl_display_create);
    if (!real) return NULL;
    struct wl_display *dpy = real();
    if (dpy && is_gnome() && is_gnome_shell()) {
        void *lib = dlopen("libhybris-platformcommon.so", RTLD_NOW | RTLD_NOLOAD);
        if (!lib) lib = dlopen("libhybris-platformcommon.so", RTLD_NOW);
        if (lib) {
            server_wlegl_create_t create =
                dlsym(lib, "_Z19server_wlegl_createP10wl_display");
            if (create) create(dpy);
        }
    }
    return dpy;
}


/* ==========================================================================
 * 5. HWC2 VSYNC -- always succeed so schedule_frame() keeps running
 * ========================================================================== */

typedef void    hwc2_compat_display_t;
typedef int32_t hwc2_error_t;
#define HWC2_ERROR_NONE 0

hwc2_error_t hwc2_compat_display_set_vsync_enabled(hwc2_compat_display_t *display,
                                                    int32_t enabled) {
    static hwc2_error_t (*real_fn)(hwc2_compat_display_t *, int32_t) = NULL;
    if (!real_fn) real_fn = resolve_next("hwc2_compat_display_set_vsync_enabled",
                                         (void *)hwc2_compat_display_set_vsync_enabled);
    if (real_fn) real_fn(display, enabled);
    return HWC2_ERROR_NONE;
}


/* ==========================================================================
 * 6. BUFFER SHIMS -- double-buffering; discard redundant acquire fences
 * ========================================================================== */

typedef void HWCNativeWindow;
void HWCNativeWindowSetBufferCount(HWCNativeWindow *win, int count) {
    static void (*real_fn)(HWCNativeWindow *, int) = NULL;
    if (!real_fn) real_fn = resolve_next("HWCNativeWindowSetBufferCount",
                                         (void *)HWCNativeWindowSetBufferCount);
    /* Only force double-buffering in the compositor. Clients keep their count. */
    if (real_fn) real_fn(win, is_compositor() ? 2 : count);
}

void HWCNativeBufferSetFence(ANativeWindowBuffer *buffer, int fd) {
    static void (*real_fn)(ANativeWindowBuffer *, int) = NULL;
    if (!real_fn) real_fn = resolve_next("HWCNativeBufferSetFence",
                                         (void *)HWCNativeBufferSetFence);
    /* Only discard fences in the compositor. Clients need their real fence
     * preserved or buffer sync breaks (camera preview texture corruption). */
    if (!is_compositor()) {
        if (real_fn) real_fn(buffer, fd);
        return;
    }
    if (real_fn) real_fn(buffer, -1);
    if (fd >= 0) close(fd);
}


/* ==========================================================================
 * 7. DRM IOCTLS -- intercept ADDFB2/ATOMIC/PAGE_FLIP; maintain dumb buffer
 * ========================================================================== */

#define MAX 64

static uint32_t frame_w = 0, frame_h = 0;

static struct { uint32_t prime_fd; buffer_handle_t gralloc; } gmap[MAX];
static int gmap_n = 0;

static struct { uint32_t gem, fb_id; } fmap[MAX];
static int fmap_n = 0;

static uint32_t  dumb_handle = 0, dumb_fb_id = 0, dumb_pitch = 0;
static void     *dumb_map    = NULL;
static size_t    dumb_size   = 0;
static uint32_t  next_fake   = 0x80000000u;
static __thread int in_hook  = 0;

typedef int (*ioctl_t)(int, unsigned long, ...);
static ioctl_t real_ioctl = NULL;
int ioctl(int fd, unsigned long request, ...);
static void ensure_real(void) {
    if (!real_ioctl) real_ioctl = (ioctl_t)resolve_next("ioctl", (void *)ioctl);
}

static int gmap_evict = 0;
void drm_shim_register_bo(uint32_t prime_fd, buffer_handle_t gralloc) {
    for (int i = 0; i < gmap_n; i++)
        if (gmap[i].prime_fd == prime_fd) { gmap[i].gralloc = gralloc; return; }
    int slot;
    if (gmap_n < MAX) slot = gmap_n++;
    else { slot = gmap_evict; gmap_evict = (gmap_evict + 1) % MAX; }
    gmap[slot].prime_fd = prime_fd; gmap[slot].gralloc = gralloc;
}
static buffer_handle_t find_gralloc(uint32_t gem) {
    for (int i = 0; i < gmap_n; i++)
        if (gmap[i].prime_fd == gem) return gmap[i].gralloc;
    return NULL;
}
static buffer_handle_t find_by_fb(uint32_t fb_id) {
    for (int i = 0; i < fmap_n; i++)
        if (fmap[i].fb_id == fb_id) return find_gralloc(fmap[i].gem);
    return NULL;
}
static int fmap_evict = 0;
static void fmap_insert(uint32_t gem, uint32_t fb_id) {
    for (int i = 0; i < fmap_n; i++)
        if (fmap[i].gem == gem) { fmap[i].fb_id = fb_id; return; }
    int slot;
    if (fmap_n < MAX) slot = fmap_n++;
    else { slot = fmap_evict; fmap_evict = (fmap_evict + 1) % MAX; }
    fmap[slot].gem = gem; fmap[slot].fb_id = fb_id;
}
/* NB: the dumb buffer is never actually scanned out (the HWC2 composer owns the
 * CRTC; real pixels reach the panel via the drmadapter EGL path). HOWEVER the
 * per-frame gralloc lock/unlock in copy_to_dumb() is load-bearing -- removing it
 * makes gnome-session stop the shell after ~43s (verified on-device). The lock/
 * unlock is evidently a GPU/cache sync the HWC2 presentation relies on, so keep
 * it. (The dumb FB also gives mutter a real FB id to page-flip to.) */
static void copy_to_dumb(buffer_handle_t h) {
    if (!dumb_map || !h || !frame_w || !frame_h) return;
    void *src = NULL;
    if (hybris_gralloc_lock(h, 0x3|0x30, 0, 0, frame_w, frame_h, &src) || !src) return;
    uint8_t *d = dumb_map, *s = src;
    for (uint32_t y = 0; y < frame_h; y++)
        memcpy(d + y*dumb_pitch, s + y*dumb_pitch, frame_w*4);
    hybris_gralloc_unlock(h);
}
static int init_dumb(int fd) {
    if (dumb_map) return 0;
    if (!frame_w || !frame_h) return -1;
    int saved = in_hook; in_hook = 1;
    struct drm_mode_create_dumb cd = { .height=frame_h, .width=frame_w, .bpp=32 };
    if (real_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { in_hook=saved; return -1; }
    dumb_handle=cd.handle; dumb_pitch=cd.pitch; dumb_size=cd.size;
    struct drm_mode_fb_cmd fb = {
        .width=frame_w, .height=frame_h,
        .pitch=dumb_pitch, .bpp=32, .depth=24, .handle=dumb_handle
    };
    if (real_ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb) == 0) {
        dumb_fb_id = fb.fb_id;
    } else {
        struct drm_mode_fb_cmd2 fb2 = {
            .width=frame_w, .height=frame_h, .pixel_format=0x34325258
        };
        fb2.handles[0]=dumb_handle; fb2.pitches[0]=dumb_pitch;
        if (real_ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb2)) { in_hook=saved; return -1; }
        dumb_fb_id = fb2.fb_id;
    }
    struct drm_mode_map_dumb md = { .handle=dumb_handle };
    if (real_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) { in_hook=saved; return -1; }
    dumb_map = mmap(NULL, dumb_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, md.offset);
    if (dumb_map == MAP_FAILED) { dumb_map=NULL; in_hook=saved; return -1; }
    in_hook=saved; return 0;
}

int drmModeAddFB2WithModifiers(int fd, uint32_t w, uint32_t h, uint32_t fmt,
    const uint32_t handles[4], const uint32_t pitches[4], const uint32_t offsets[4],
    const uint64_t mod[4], uint32_t *buf_id, uint32_t flags) {
    if (!is_compositor()) {
        typedef int (*fn_t)(int,uint32_t,uint32_t,uint32_t,const uint32_t*,
                            const uint32_t*,const uint32_t*,const uint64_t*,uint32_t*,uint32_t);
        fn_t real=(fn_t)resolve_next("drmModeAddFB2WithModifiers",
                                     (void*)drmModeAddFB2WithModifiers);
        return real ? real(fd,w,h,fmt,handles,pitches,offsets,mod,buf_id,flags) : -ENOSYS;
    }
    if (!frame_w) { frame_w=w; frame_h=h; }
    if (!dumb_map) init_dumb(fd);
    uint32_t id=next_fake++; *buf_id=id; fmap_insert(handles[0],id); return 0;
}
int drmModeAddFB2(int fd, uint32_t w, uint32_t h, uint32_t fmt,
    const uint32_t handles[4], const uint32_t pitches[4], const uint32_t offsets[4],
    uint32_t *buf_id, uint32_t flags) {
    if (!is_compositor()) {
        typedef int (*fn_t)(int,uint32_t,uint32_t,uint32_t,const uint32_t*,
                            const uint32_t*,const uint32_t*,uint32_t*,uint32_t);
        fn_t real=(fn_t)resolve_next("drmModeAddFB2",(void*)drmModeAddFB2);
        return real ? real(fd,w,h,fmt,handles,pitches,offsets,buf_id,flags) : -ENOSYS;
    }
    if (!frame_w) { frame_w=w; frame_h=h; }
    if (!dumb_map) init_dumb(fd);
    uint32_t id=next_fake++; *buf_id=id; fmap_insert(handles[0],id); return 0;
}
int drmModeRmFB(int fd, uint32_t id) {
    if (!is_compositor()) {
        typedef int (*fn_t)(int,uint32_t);
        fn_t real=(fn_t)resolve_next("drmModeRmFB",(void*)drmModeRmFB);
        return real ? real(fd,id) : 0;
    }
    return 0;
}
int drmModeSetCrtc(int fd, uint32_t crtcId, uint32_t bufferId, uint32_t x, uint32_t y,
    uint32_t *connectors, int count, drmModeModeInfoPtr mode) {
    if (!is_compositor()) {
        typedef int (*fn_t)(int,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t*,int,drmModeModeInfoPtr);
        fn_t real=(fn_t)resolve_next("drmModeSetCrtc",(void*)drmModeSetCrtc);
        return real ? real(fd,crtcId,bufferId,x,y,connectors,count,mode) : -ENOSYS;
    }
    if (!dumb_map) init_dumb(fd); return 0;
}
int drmModePageFlip(int fd, uint32_t crtc_id, uint32_t fb_id, uint32_t flags, void *ud) {
    typedef int (*fn_t)(int,uint32_t,uint32_t,uint32_t,void*);
    fn_t real=(fn_t)resolve_next("drmModePageFlip",(void*)drmModePageFlip);
    if (!is_compositor())
        return real ? real(fd,crtc_id,fb_id,flags,ud) : -ENOSYS;
    buffer_handle_t h=find_by_fb(fb_id); copy_to_dumb(h);
    return real ? real(fd,crtc_id,dumb_fb_id?dumb_fb_id:fb_id,flags,ud) : 0;
}
int drmModeAtomicCommit(int fd, drmModeAtomicReqPtr req, uint32_t flags, void *ud) {
    if (!is_compositor()) {
        typedef int (*fn_t)(int,drmModeAtomicReqPtr,uint32_t,void*);
        fn_t real=(fn_t)resolve_next("drmModeAtomicCommit",(void*)drmModeAtomicCommit);
        return real ? real(fd,req,flags,ud) : -ENOSYS;
    }
    for (int i=fmap_n-1; i>=0; i--) {
        buffer_handle_t h=find_gralloc(fmap[i].gem);
        if (h) { copy_to_dumb(h); break; }
    }
    return 0;
}

int ioctl(int fd, unsigned long request, ...) {
    ensure_real();
    va_list args; va_start(args,request); void *arg=va_arg(args,void*); va_end(args);
    uint32_t magic=(request>>8)&0xff;
    if (magic != 0x64) return real_ioctl(fd,request,arg);
    /* Only the compositor's DRM ioctls drive the fake KMS framebuffer.
     * Client processes (camera etc) must reach the real DRM driver intact. */
    if (!is_compositor()) return real_ioctl(fd,request,arg);
    if (in_hook) return real_ioctl(fd,request,arg);
    uint32_t nr=request&0xff;
    in_hook=1; int ret;
    if (nr==0xb8) {
        uint32_t *fb=arg, w=fb[0], h=fb[1], gem=fb[5];
        if (!frame_w) { frame_w=w; frame_h=h; }
        if (!dumb_map) init_dumb(fd);
        uint32_t id=next_fake++; fb[6]=id; fmap_insert(gem,id); ret=0;
    } else if (nr==0xaf) { ret=real_ioctl(fd,request,arg);
    } else if (nr==0xa2) {
        if (!dumb_map) init_dumb(fd);
        real_ioctl(fd,request,arg); ret=0;
    } else if (nr==0xb0||nr==0xb6) {
        struct drm_mode_crtc_page_flip *flip=arg;
        buffer_handle_t h=find_by_fb(flip->fb_id); copy_to_dumb(h);
        if (dumb_fb_id) flip->fb_id=dumb_fb_id;
        ret=real_ioctl(fd,request,arg);
    } else if (nr==0xbc) {
        for (int i=fmap_n-1; i>=0; i--) {
            buffer_handle_t h=find_gralloc(fmap[i].gem);
            if (h) { copy_to_dumb(h); break; }
        }
        ret=0;
    } else if (nr==0x11) { ret=0;
    } else { ret=real_ioctl(fd,request,arg); }
    in_hook=0; return ret;
}
