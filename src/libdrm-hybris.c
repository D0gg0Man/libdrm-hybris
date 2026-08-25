/*
 * libdrm-hybris.c -- unified LD_PRELOAD shim for FuriOS hybris/HWC2
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
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <stdarg.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <time.h>
#include <android/android-config.h>
#include <hybris/gralloc/gralloc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "common.h"
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
/* Runtime session detection */
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



/* ==========================================================================
 * 2. DRM CAPS -- render node advertisement, capability patches
 * ========================================================================== */

char *drmGetRenderDeviceNameFromFd(int fd) {
    if (!hybris_is_compositor()) {
        typedef char *(*fn_t)(int);
        fn_t real=(fn_t)hybris_resolve_next("drmGetRenderDeviceNameFromFd",
                                     (void*)drmGetRenderDeviceNameFromFd);
        return real ? real(fd) : NULL;
    }
    return strdup("/dev/dri/card0");
}
int drmGetNodeTypeFromFd(int fd) {
    if (!hybris_is_compositor()) {
        typedef int (*fn_t)(int);
        fn_t real=(fn_t)hybris_resolve_next("drmGetNodeTypeFromFd",(void*)drmGetNodeTypeFromFd);
        return real ? real(fd) : -1;
    }
    return DRM_NODE_PRIMARY;
}

int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device) {
    static int (*real_fn)(int, uint32_t, drmDevicePtr *) = NULL;
    if (!real_fn) real_fn = hybris_resolve_next("drmGetDevice2", (void *)drmGetDevice2);
    if (!real_fn) return -ENOSYS;
    int r = real_fn(fd, flags, device);
    if (r == 0 && *device && hybris_is_compositor()) {
        (*device)->available_nodes |= (1 << DRM_NODE_RENDER);
        (*device)->nodes[DRM_NODE_RENDER] = strdup((*device)->nodes[DRM_NODE_PRIMARY]);
    }
    return r;
}
int drmGetCap(int fd, uint64_t cap, uint64_t *value) {
    static int (*real_fn)(int, uint64_t, uint64_t *) = NULL;
    if (!real_fn) real_fn = hybris_resolve_next("drmGetCap", (void *)drmGetCap);
    if (hybris_is_compositor()) {
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
    if (!real_fn) real_fn = hybris_resolve_next("drmSetClientCap", (void *)drmSetClientCap);
    int r = real_fn ? real_fn(fd, cap, value) : -ENOSYS;
    /* wlroots/phoc: ATOMIC + UNIVERSAL_PLANES must really be set on the fd so
     * the kernel exposes the primary/cursor planes and the atomic uAPI -- pass
     * them through (the MediaTek DRM is a real atomic driver and accepts them).
     * Only if the driver rejects one (e.g. because the HWC2 composer owns the
     * master) do we pretend success, so wlroots still takes the atomic path
     * where our faked drmModeAtomicCommit() works. Not for gnome/mutter. */
    if (r != 0 && hybris_is_compositor() && !hybris_is_gnome() &&
        (cap == DRM_CLIENT_CAP_ATOMIC || cap == DRM_CLIENT_CAP_UNIVERSAL_PLANES))
        return 0;
    return r;
}
int drmIsKMS(int fd) {
    if (!hybris_is_compositor()) {
        typedef int (*fn_t)(int);
        fn_t real=(fn_t)hybris_resolve_next("drmIsKMS",(void*)drmIsKMS);
        return real ? real(fd) : 0;
    }
    return 1;
}
int drmModeCreateLease(int fd, const uint32_t *o, int n, int f, uint32_t *id) {
    if (!hybris_is_compositor()) {
        typedef int (*fn_t)(int,const uint32_t*,int,int,uint32_t*);
        fn_t real=(fn_t)hybris_resolve_next("drmModeCreateLease",(void*)drmModeCreateLease);
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

/* ==========================================================================
 * 4. WAYLAND -- gnome: inject android_wlegl into wl_display on creation
 * ========================================================================== */

typedef void *(*server_wlegl_create_t)(struct wl_display *);

struct wl_display *wl_display_create(void) {
    typedef struct wl_display *(*fn_t)(void);
    fn_t real = hybris_resolve_next("wl_display_create", (void *)wl_display_create);
    if (!real) return NULL;
    struct wl_display *dpy = real();
    if (dpy && hybris_is_gnome() && is_gnome_shell()) {
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


/* HWC2 power modes (hardware/graphics/composer/2.1 IComposerClient::PowerMode).
 * Only OFF and ON are used here: DOZE/DOZE_SUSPEND need AOD enabled or the
 * vendor HAL logs "without aod enabled" and ignores them. */
#define HWC2_POWER_MODE_OFF 0
#define HWC2_POWER_MODE_ON  2

/* Captured in hwc2_compat_display_present(); file-static so the shim does not
 * export an extra symbol into every process it is preloaded into. */
static void *g_hwc_display = NULL;

/* ==========================================================================
 * 5. HWC2 VSYNC -- always succeed so schedule_frame() keeps running
 * ========================================================================== */

typedef void    hwc2_compat_display_t;
typedef int32_t hwc2_error_t;
#define HWC2_ERROR_NONE 0

hwc2_error_t hwc2_compat_display_set_vsync_enabled(hwc2_compat_display_t *display,
                                                    int32_t enabled) {
    static hwc2_error_t (*real_fn)(hwc2_compat_display_t *, int32_t) = NULL;
    if (!real_fn) real_fn = hybris_resolve_next("hwc2_compat_display_set_vsync_enabled",
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
    if (!real_fn) real_fn = hybris_resolve_next("HWCNativeWindowSetBufferCount",
                                         (void *)HWCNativeWindowSetBufferCount);
    /* Only force double-buffering in the compositor. Clients keep their count. */
    if (real_fn) real_fn(win, hybris_is_compositor() ? 2 : count);
}

void HWCNativeBufferSetFence(ANativeWindowBuffer *buffer, int fd) {
    static void (*real_fn)(ANativeWindowBuffer *, int) = NULL;
    if (!real_fn) real_fn = hybris_resolve_next("HWCNativeBufferSetFence",
                                         (void *)HWCNativeBufferSetFence);
    /* Only discard fences in the compositor. Clients need their real fence
     * preserved or buffer sync breaks (camera preview texture corruption). */
    if (!hybris_is_compositor()) {
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
    if (!real_ioctl) real_ioctl = (ioctl_t)hybris_resolve_next("ioctl", (void *)ioctl);
}

/* Env-gated tracing (LIBDRM_HYBRIS_TRACE=1). */
static int trace_on = -1;
static void tracef(const char *fmt, ...) {
    if (trace_on < 0) trace_on = hybris_debug.trace ? 1 : 0;
    if (!trace_on) return;
    int saved = in_hook; in_hook = 1;
    FILE *f = fopen("/tmp/libdrm-hybris-trace.log", "a");
    if (f) { va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fclose(f); }
    in_hook = saved;
}

/* ------------------------------------------------------------------------
 * Synthetic page-flip completion events (decouple mutter's frame clock from
 * card0 DRM master, which the HWC2 composer permanently owns). On an output
 * reconfigure the timing flips start returning EACCES; we then synthesize the
 * DRM_EVENT_FLIP_COMPLETE ourselves, paced at the interval measured from the
 * real flips. The poll/read hooks fast-path out via g_synth_active.
 * ---------------------------------------------------------------------- */
static int      g_synth_active   = 0;
static int      g_drm_fd         = -1;
static uint64_t g_interval_ns    = 0;
static uint64_t g_last_flip_ns   = 0;
static uint32_t g_synth_seq      = 0;

/* Armed-but-undelivered synthetic flips. A QUEUE, not a single slot: flips are
 * armed from several paths (atomic commits, legacy page-flip ioctls during
 * modesets) and wlroots' user_data is a heap object (wlr_drm_page_flip) whose
 * completion MUST be delivered exactly once. With a single slot, a second arm
 * overwrote an undelivered first flip -- that flip's completion was lost,
 * wlroots' conn->pending_page_flip never cleared, and it refused every further
 * commit: the compositor froze (this was the intermittent cold-boot black and
 * the post-blank freeze). */
#define SYNTH_QMAX 16
static struct { uint32_t crtc; uint64_t user; uint64_t deadline; } g_synth_q[SYNTH_QMAX];
static int g_synth_qh = 0;   /* head index */
static int g_synth_qn = 0;   /* queued count; >0 == "pending" */

/* When the compositor nests the DRM fd inside an epoll instance and waits on
 * that epoll fd from an outer poll/ppoll (phoc: wl_event_loop epoll fd polled
 * by the GLib main loop), the outer wait must treat the epoll fd as a synth
 * target too, then the epoll_wait hook injects the inner DRM readiness. */
static int          g_epoll_fd = -1;
static epoll_data_t g_drm_epoll_data;
static int          g_drm_epoll_valid = 0;
/* Private eventfd we add to the compositor's DRM epoll so that arming a synth
 * flip makes that epoll (and thus the outer GLib loop nesting it) OS-readable,
 * waking the compositor to drill in and consume the flip even when it is
 * otherwise idle (post-blank). Without this the outer loop blocks and the flip
 * is only delivered when some real event happens to wake it (sparse) -> the
 * shell freezes visually after an idle blank. Identified via ev.data.ptr. */
static int          g_wake_fd = -1;
/* Companion timerfd in the same epoll: wakes the loop AT the flip deadline
 * (the compositor dispatches the inner epoll with timeout 0, so without a
 * timer the vsync-aligned deadline would only be met on unrelated activity). */
static int          g_timer_fd = -1;
static int is_synth_fd(int fd) {
    return fd == g_drm_fd || (g_drm_epoll_valid && fd == g_epoll_fd);
}

static ssize_t (*real_read)(int, void *, size_t) = NULL;
static int     (*real_poll)(struct pollfd *, nfds_t, int) = NULL;
static int     (*real_ppoll)(struct pollfd *, nfds_t, const struct timespec *, const sigset_t *) = NULL;

static uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
static int g_traceall = -1;
static void TRACEALL(const char *fmt, ...) {
    if (g_traceall < 0) g_traceall = getenv("LIBDRM_HYBRIS_TRACE_ALL") ? 1 : 0;
    if (!g_traceall) return;
    va_list a; va_start(a, fmt);
    fprintf(stderr, "drmtrace: ");
    vfprintf(stderr, fmt, a);
    fprintf(stderr, "\n");
    va_end(a);
}
static int g_stamp = -1;
static void STAMP(const char *tag) {
    if (g_stamp < 0) g_stamp = getenv("LIBDRM_HYBRIS_STAMP") ? 1 : 0;
    if (g_stamp) fprintf(stderr, "STAMP %8.3f %s\n", (double)(now_ns() % 100000000000ull)/1e6, tag);
}
static void synth_note_flip(void) {
    uint64_t n = now_ns();
    if (g_last_flip_ns) {
        uint64_t d = n - g_last_flip_ns;
        if (d > 1000000ull && d < 100000000ull)
            g_interval_ns = g_interval_ns ? (g_interval_ns * 7 + d) / 8 : d;
    }
    g_last_flip_ns = n;
}
/* Pace the synthetic flip completion at the panel's real vsync period (queried
 * from HWC2, ~8.33ms at 120Hz). Delivering it immediately re-enters wlroots
 * before its atomic-commit state settles and the loop stalls after a few
 * frames; one vsync of delay mirrors real hardware and keeps it stable. The
 * period is set once from drmadapter via drm_shim_set_vsync_period(); 8.33ms
 * is only a startup fallback. */
static uint64_t g_vsync_ns = 8333333ull;
void drm_shim_set_vsync_period(uint64_t ns) {
    if (ns > 1000000ull && ns < 100000000ull) g_vsync_ns = ns;
}
/* Real panel vsync timestamps (CLOCK_MONOTONIC ns), stamped by drmadapter's
 * HWC2 vsync callback. Used to phase-align the synthetic flip deadlines to the
 * actual vblank: a free-running flip clock drifts against the panel and the
 * presents periodically straddle the composer's latch point -> mixed old/new
 * frames = flicker under motion (session-random severity = boot phase). */
static uint64_t g_vsync_stamp_ns = 0;
static unsigned long g_vsync_stamps = 0;
void drm_shim_vsync_stamp(int64_t ts) { if (ts > 0) { g_vsync_stamp_ns = (uint64_t)ts; g_vsync_stamps++; } }

/* Recover the panel's vblank PHASE without HWC2 vsync callbacks.
 *
 * Measured on-device: this HAL never fires the vsync callback (stamps stayed 0
 * over minutes of compositing), so drm_shim_vsync_stamp() was never called and
 * BOTH synth_arm()'s flip deadlines and the flip-event presentation timestamps
 * silently fell back to free-running now(). The period was always correct
 * (8333333 ns); only the phase was missing, and a free-running flip clock
 * drifts against the panel exactly as the synth_arm comment warns.
 *
 * hwc2_compat_display_present() returns a fence that signals at the vblank the
 * frame was scanned out; drmadapter closes it immediately. Interpose the call
 * (this shim is in ld.so.preload, so it wins), dup the fd so drmadapter's close
 * stays harmless, and read the PREVIOUS frame's signal time via
 * SYNC_IOC_FILE_INFO -- signalled by then, so nothing blocks.
 *
 * Resolution must NEVER fail closed: returning an error fails every present and
 * blanks the panel (learned the hard way). Try RTLD_NEXT, then dlopen libhwc2,
 * and only then give up -- reporting success with no fence rather than error. */
struct dh_sync_fence_info {
    char obj_name[32]; char driver_name[32];
    int32_t status; uint32_t flags; uint64_t timestamp_ns;
};
struct dh_sync_file_info {
    char name[32]; int32_t status; uint32_t flags;
    uint32_t num_fences; uint32_t pad; uint64_t sync_fence_info;
};
#define DH_SYNC_IOC_FILE_INFO _IOWR('>', 4, struct dh_sync_file_info)

typedef int (*hwc2_present_fn)(void*, int32_t*);
int hwc2_compat_display_present(void *display, int32_t *out_fence);

static hwc2_present_fn hwc2_present_real(void) {
    static hwc2_present_fn f = NULL;
    static int tried = 0;
    if (tried) return f;
    tried = 1;
    f = (hwc2_present_fn)hybris_resolve_next("hwc2_compat_display_present",
                                      (void*)hwc2_compat_display_present);
    if (!f) {
        const char *cands[] = { "libhwc2.so.1", "libhwc2.so", NULL };
        for (int i = 0; cands[i] && !f; i++) {
            void *h = dlopen(cands[i], RTLD_NOW | RTLD_GLOBAL);
            if (!h) continue;
            void *sym = dlsym(h, "hwc2_compat_display_present");
            if (sym && sym != (void*)hwc2_compat_display_present)
                f = (hwc2_present_fn)sym;
        }
    }
    LOG_WARN("hwc2 present real=%p (%s)",
            (void*)f, f ? "resolved" : "NOT RESOLVED - fence phase off");
    return f;
}

static void stamp_phase_from_fence(int fd) {
    if (fd < 0 || !real_ioctl) return;
    struct dh_sync_file_info info; memset(&info, 0, sizeof info);
    int saved = in_hook; in_hook = 1;
    if (real_ioctl(fd, DH_SYNC_IOC_FILE_INFO, &info) == 0 && info.num_fences) {
        struct dh_sync_fence_info *fi = calloc(info.num_fences, sizeof(*fi));
        if (fi) {
            info.sync_fence_info = (uint64_t)(uintptr_t)fi;
            if (real_ioctl(fd, DH_SYNC_IOC_FILE_INFO, &info) == 0) {
                uint64_t latest = 0;
                for (uint32_t i = 0; i < info.num_fences; i++)
                    if (fi[i].status == 1 && fi[i].timestamp_ns > latest)
                        latest = fi[i].timestamp_ns;
                if (latest) { g_vsync_stamp_ns = latest; g_vsync_stamps++; }
            }
            free(fi);
        }
    }
    in_hook = saved;
}

int hwc2_compat_display_present(void *display, int32_t *out_fence) {
    g_hwc_display = display;   /* needed by drm_shim_panel_power() below */
    /* mutter keeps committing frames after the shell blanks. Presenting into a
     * panel that is powered OFF risks blocking on an acquire fence the HAL will
     * never signal, and the compositor then comes back with the backlight lit
     * but nothing painted. Drop frames while the panel is down instead. */
    extern int drm_shim_panel_is_on(void);
    if (!drm_shim_panel_is_on()) {
        if (out_fence) *out_fence = -1;
        return 0;
    }
    hwc2_present_fn real = hwc2_present_real();
    if (!real) { if (out_fence) *out_fence = -1; return 0; }  /* fail OPEN */
    int r = real(display, out_fence);
    static int off = -1;
    if (off < 0) off = getenv("LIBDRM_HYBRIS_NO_FENCE_PHASE") ? 1 : 0;
    if (!off && out_fence && *out_fence >= 0) {
        static int prev = -1;
        if (prev >= 0) { stamp_phase_from_fence(prev); close(prev); }
        prev = dup(*out_fence);
    }
    return r;
}
/* KWin's legacy-KMS path wants master-gated state ioctls (gamma/cursor/property)
 * faked to success; wlroots/phoc needs their real result during atomic output
 * bring-up. Opt-in so only the KWin session changes behaviour. */
static int fake_kms_state(void) {
    static int f = -1;
    /* KWin's legacy-KMS path needs the faked state; phoc and mutter must see
     * the real ioctl results. Detected rather than declared, with an env
     * override for bringing up a compositor this does not recognise. */
    if (f < 0)
        f = hybris_is_kwin() || getenv("LIBDRM_HYBRIS_FAKE_KMS_STATE") ? 1 : 0;
    return f;
}
static void synth_arm(uint32_t crtc, uint64_t user_data) {
    if (g_synth_qn >= SYNTH_QMAX) {
        /* Should never happen (wlroots keeps <=1 flip in flight per connector);
         * losing a flip means a permanent freeze, so scream if it ever does. */
        LOG_WARN("SYNTH QUEUE OVERFLOW, dropping oldest flip!");
        g_synth_qh = (g_synth_qh + 1) % SYNTH_QMAX;
        g_synth_qn--;
    }
    int tail = (g_synth_qh + g_synth_qn) % SYNTH_QMAX;
    g_synth_q[tail].crtc = crtc;
    g_synth_q[tail].user = user_data;
    {
        uint64_t now = now_ns();
        uint64_t dl;
        /* LIBDRM_HYBRIS_FAST_COMPLETE: deliver the completion immediately
         * instead of at the next vblank. For compositors whose present is
         * fully decoupled from the flip (KWin QPainter + drmadapter's async
         * present worker) the vblank wait only serializes the frame pipeline
         * -- there is no real scanout to tear against. phoc/mutter keep the
         * vsync-aligned pacing (their present happens inside the flip). */
        static int fast = -1;
        if (fast < 0) fast = getenv("LIBDRM_HYBRIS_FAST_COMPLETE") ? 1 : 0;
        if (fast) {
            dl = now;
        } else if (g_vsync_stamp_ns && now > g_vsync_stamp_ns &&
            now - g_vsync_stamp_ns < 1000000000ull) {
            /* Fresh vsync reference: deliver at the next real vblank boundary. */
            uint64_t phase = (now - g_vsync_stamp_ns) % g_vsync_ns;
            dl = now + (g_vsync_ns - phase);
        } else {
            dl = now + g_vsync_ns; /* fallback: free-running period */
        }
        g_synth_q[tail].deadline = dl;
    }
    g_synth_qn++;
    g_synth_active = 1;
    if (hybris_debug.sample) {
        static unsigned long a = 0;
        if (a++ < 10)
            LOG("synth_arm #%lu crtc=%u qn=%d timer=%d wake=%d",
                    a, crtc, g_synth_qn, g_timer_fd, g_wake_fd);
    }
    /* Kick the wake eventfd so the compositor's (possibly idle) outer event loop
     * wakes and drills into the DRM epoll to consume this flip. Drained+hidden in
     * epoll_synth() so the compositor never sees the eventfd itself. */
    if (g_wake_fd >= 0) { uint64_t one = 1; ssize_t w = write(g_wake_fd, &one, sizeof one); (void)w; }
    /* Arm the deadline timer for the queue head (absolute monotonic). */
    if (g_timer_fd >= 0) {
        struct itimerspec its; memset(&its, 0, sizeof its);
        uint64_t hd = g_synth_q[g_synth_qh].deadline;
        its.it_value.tv_sec = hd / 1000000000ull;
        its.it_value.tv_nsec = hd % 1000000000ull;
        timerfd_settime(g_timer_fd, TFD_TIMER_ABSTIME, &its, NULL);
    }
    STAMP("arm");
}

static uint64_t g_last_deliver_ns = 0;  /* when the last synth completion reached the compositor */
ssize_t read(int fd, void *buf, size_t count) {
    if (!real_read) real_read = (ssize_t(*)(int,void*,size_t))hybris_resolve_next("read",(void*)read);
    if (!g_synth_active || fd != g_drm_fd || g_synth_qn <= 0 || in_hook)
        return real_read(fd, buf, count);
    if (count < sizeof(struct drm_event_vblank)) return real_read(fd, buf, count);
    struct drm_event_vblank ev; memset(&ev, 0, sizeof ev);
    ev.base.type   = DRM_EVENT_FLIP_COMPLETE;
    ev.base.length = sizeof ev;
    ev.user_data   = g_synth_q[g_synth_qh].user;
    uint64_t n = now_ns();
    /* The presentation timestamp mutter's frame clock schedules against must be
     * the vblank the frame was shown at, NOT the moment its main loop got round
     * to read()ing the event. Feeding it read() jitter makes it mispredict the
     * next deadline and miss it, which shows up as a bimodal 8.3/16.6 ms frame
     * interval with the GPU and CPU both idle. Snap to the last real panel
     * vblank boundary (HWC2 stamps g_vsync_stamp_ns) when we have a fresh
     * reference. LIBDRM_HYBRIS_RAW_TS=1 restores the old now()-based stamp. */
    uint64_t ts = n;
    int aligned = 0;
    {
        static int raw = -1;
        if (raw < 0) raw = getenv("LIBDRM_HYBRIS_RAW_TS") ? 1 : 0;
        /* Braces were missing here: aligned = 1 sat outside the if and ran
         * unconditionally, so the SYNCSTATS line below always claimed the
         * timestamp was vsync-aligned even when it had not been adjusted.
         * Diagnostic-only -- ts itself was computed correctly. */
        if (!raw && g_vsync_stamp_ns && g_vsync_ns && n > g_vsync_stamp_ns &&
            n - g_vsync_stamp_ns < 1000000000ull) {
            ts = g_vsync_stamp_ns + ((n - g_vsync_stamp_ns) / g_vsync_ns) * g_vsync_ns;
            aligned = 1;
        }
    }
    if (getenv("LIBDRM_HYBRIS_SYNCSTATS")) {
        static unsigned long e = 0;
        if ((e++ % 120) == 0)
            LOG("phase stamps=%lu  age=%lld us  aligned=%d  period=%llu ns",
                    g_vsync_stamps,
                    g_vsync_stamp_ns ? (long long)((n - g_vsync_stamp_ns)/1000) : -1LL,
                    aligned, (unsigned long long)g_vsync_ns);
    }
    ev.tv_sec  = (uint32_t)(ts / 1000000000ull);
    ev.tv_usec = (uint32_t)((ts / 1000ull) % 1000000ull);
    ev.sequence = ++g_synth_seq;
    if (hybris_debug.sample) {
        static unsigned long d = 0;
        if (d++ < 10)
            LOG("synth deliver #%lu seq=%u", d, g_synth_seq);
    }
    ev.crtc_id  = g_synth_q[g_synth_qh].crtc;
    memcpy(buf, &ev, sizeof ev);
    g_synth_qh = (g_synth_qh + 1) % SYNTH_QMAX;
    g_synth_qn--;
    g_last_deliver_ns = n;
    STAMP("read-deliver");
    tracef("SYNTH read delivered crtc=%u user=0x%llx seq=%u qn=%d\n",
           ev.crtc_id, (unsigned long long)ev.user_data, g_synth_seq, g_synth_qn);
    return sizeof ev;
}
static int synth_poll_fixup(struct pollfd *fds, nfds_t n) {
    if (!g_synth_active || g_synth_qn <= 0) return -1;
    /* NO deadline gating here: this path serves GLib/ppoll compositors
     * (mutter/gnome-mali) which have no timerfd to wake them at the deadline --
     * gating made them sleep on their own ppoll timeout waiting for a flip
     * that was waiting for them (frame clock stall, shell wedge). Immediate
     * delivery is the historical, working behavior for this path; vsync
     * pacing applies only to the epoll path where the timerfd guarantees a
     * wake-up (wlroots/phoc). */
    for (nfds_t i = 0; i < n; i++)
        if (is_synth_fd(fds[i].fd) && (fds[i].events & POLLIN)) { fds[i].revents |= POLLIN; return (int)i; }
    return -1;
}
int poll(struct pollfd *fds, nfds_t n, int timeout) {
    if (!real_poll) real_poll = (int(*)(struct pollfd*,nfds_t,int))hybris_resolve_next("poll",(void*)poll);
    if (!g_synth_active || in_hook) return real_poll(fds, n, timeout);
    if (synth_poll_fixup(fds, n) >= 0) return 1;
    int got = real_poll(fds, n, timeout);
    if (got == 0 && g_synth_qn > 0 && now_ns() >= g_synth_q[g_synth_qh].deadline)
        for (nfds_t i = 0; i < n; i++)
            if (is_synth_fd(fds[i].fd) && (fds[i].events & POLLIN)) { fds[i].revents |= POLLIN; return 1; }
    return got;
}
int ppoll(struct pollfd *fds, nfds_t n, const struct timespec *to, const sigset_t *ss) {
    if (!real_ppoll) real_ppoll = (int(*)(struct pollfd*,nfds_t,const struct timespec*,const sigset_t*))hybris_resolve_next("ppoll",(void*)ppoll);
    if (!g_synth_active || in_hook) return real_ppoll(fds, n, to, ss);
    if (synth_poll_fixup(fds, n) >= 0) return 1;
    int got = real_ppoll(fds, n, to, ss);
    if (got == 0 && g_synth_qn > 0 && now_ns() >= g_synth_q[g_synth_qh].deadline)
        for (nfds_t i = 0; i < n; i++)
            if (is_synth_fd(fds[i].fd) && (fds[i].events & POLLIN)) { fds[i].revents |= POLLIN; return 1; }
    return got;
}

/* epoll variants of the synth delivery, for compositors whose event loop waits
 * on the DRM fd via epoll rather than poll/ppoll (wlroots/wayland uses epoll;
 * mutter's GLib loop uses ppoll). We capture the epoll instance + the
 * wl_event_source data the compositor associated with the DRM fd, then inject a
 * readiness event when a synthetic flip is due. The compositor then read()s the
 * fd and our read() hook hands back the DRM_EVENT_FLIP_COMPLETE. */
static int (*real_epoll_ctl)(int,int,int,struct epoll_event*) = NULL;
static int (*real_epoll_wait)(int,struct epoll_event*,int,int) = NULL;
static int (*real_epoll_pwait)(int,struct epoll_event*,int,int,const sigset_t*) = NULL;

static int fd_is_drm(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return 0;
    return S_ISCHR(st.st_mode) && major(st.st_rdev) == 226; /* DRM major */
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev) {
    if (!real_epoll_ctl) real_epoll_ctl = (int(*)(int,int,int,struct epoll_event*))hybris_resolve_next("epoll_ctl",(void*)epoll_ctl);
    int r = real_epoll_ctl(epfd, op, fd, ev);
    if (hybris_is_compositor() && ev && (op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD) && fd_is_drm(fd)) {
        g_epoll_fd = epfd; g_drm_epoll_data = ev->data; g_drm_epoll_valid = 1;
        if (g_drm_fd < 0) g_drm_fd = fd;
        /* Add our private wake eventfd to the SAME epoll so synth_arm() can make
         * it readable and wake the (possibly idle) outer loop. */
        if (g_wake_fd < 0) {
            g_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (g_wake_fd >= 0) {
                struct epoll_event we; memset(&we, 0, sizeof we);
                we.events = EPOLLIN; we.data.ptr = &g_wake_fd;
                if (real_epoll_ctl(epfd, EPOLL_CTL_ADD, g_wake_fd, &we) != 0) {
                    close(g_wake_fd); g_wake_fd = -1;
                }
            }
        }
        if (g_timer_fd < 0) {
            g_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
            if (g_timer_fd >= 0) {
                struct epoll_event te; memset(&te, 0, sizeof te);
                te.events = EPOLLIN; te.data.ptr = &g_timer_fd;
                if (real_epoll_ctl(epfd, EPOLL_CTL_ADD, g_timer_fd, &te) != 0) {
                    close(g_timer_fd); g_timer_fd = -1;
                }
            }
        }
        if (hybris_debug.sample)
            LOG("epoll_ctl tracked DRM fd=%d epfd=%d data=0x%llx",
                    fd, epfd, (unsigned long long)ev->data.u64);
    }
    return r;
}

static unsigned long g_epoll_inject = 0;
static int epoll_synth(int epfd, struct epoll_event *events, int n, int maxevents) {
    if (n < 0) n = 0;
    if (epfd != g_epoll_fd || !g_drm_epoll_valid) return n;
    /* Drain + hide our private wake eventfd: wl_event_loop would deref its
     * data.ptr as a wl_event_source and crash, so it must never leak out. */
    if (g_wake_fd >= 0 || g_timer_fd >= 0) {
        if (!real_read) real_read = (ssize_t(*)(int,void*,size_t))hybris_resolve_next("read",(void*)read);
        for (int i = 0; i < n; ) {
            if (events[i].data.ptr == (void *)&g_wake_fd) {
                uint64_t v; int sv = in_hook; in_hook = 1;
                while (real_read && real_read(g_wake_fd, &v, sizeof v) == (ssize_t)sizeof v) {}
                in_hook = sv;
                events[i] = events[n - 1]; n--;
            } else if (events[i].data.ptr == (void *)&g_timer_fd) {
                uint64_t v; int sv = in_hook; in_hook = 1;
                while (real_read && real_read(g_timer_fd, &v, sizeof v) == (ssize_t)sizeof v) {}
                in_hook = sv;
                events[i] = events[n - 1]; n--;
            } else i++;
        }
    }
    if (g_synth_qn <= 0) return n;
    if (now_ns() < g_synth_q[g_synth_qh].deadline) return n; /* not due yet */
    g_epoll_inject++;
    STAMP("epoll-inject");
    for (int i = 0; i < n; i++)
        if (events[i].data.u64 == g_drm_epoll_data.u64) { events[i].events |= EPOLLIN; return n; }
    if (n < maxevents) {
        events[n].events = EPOLLIN;
        events[n].data   = g_drm_epoll_data;
        return n + 1;
    }
    return n;
}
/* A pending flip must be delivered without blocking. */
static int epoll_cap_timeout(int timeout) {
    if (g_synth_qn <= 0) return timeout;
    int64_t rem = (int64_t)g_synth_q[g_synth_qh].deadline - (int64_t)now_ns();
    if (rem <= 0) return 0;
    int ms = (int)((rem + 999999) / 1000000); /* round up: no busy loop */
    return (timeout >= 0 && timeout < ms) ? timeout : ms;
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout) {
    if (!real_epoll_wait) real_epoll_wait = (int(*)(int,struct epoll_event*,int,int))hybris_resolve_next("epoll_wait",(void*)epoll_wait);
    if (!g_synth_active || in_hook || epfd != g_epoll_fd || !g_drm_epoll_valid)
        return real_epoll_wait(epfd, events, maxevents, timeout);
    int n = real_epoll_wait(epfd, events, maxevents, epoll_cap_timeout(timeout));
    return epoll_synth(epfd, events, n, maxevents);
}
int epoll_pwait(int epfd, struct epoll_event *events, int maxevents, int timeout, const sigset_t *ss) {
    if (!real_epoll_pwait) real_epoll_pwait = (int(*)(int,struct epoll_event*,int,int,const sigset_t*))hybris_resolve_next("epoll_pwait",(void*)epoll_pwait);
    if (!g_synth_active || in_hook || epfd != g_epoll_fd || !g_drm_epoll_valid)
        return real_epoll_pwait(epfd, events, maxevents, timeout, ss);
    int n = real_epoll_pwait(epfd, events, maxevents, epoll_cap_timeout(timeout), ss);
    return epoll_synth(epfd, events, n, maxevents);
}
static int (*real_epoll_pwait2)(int,struct epoll_event*,int,const struct timespec*,const sigset_t*) = NULL;
int epoll_pwait2(int epfd, struct epoll_event *events, int maxevents, const struct timespec *to, const sigset_t *ss) {
    if (!real_epoll_pwait2) real_epoll_pwait2 = (int(*)(int,struct epoll_event*,int,const struct timespec*,const sigset_t*))hybris_resolve_next("epoll_pwait2",(void*)epoll_pwait2);
    if (!g_synth_active || in_hook || epfd != g_epoll_fd || !g_drm_epoll_valid)
        return real_epoll_pwait2(epfd, events, maxevents, to, ss);
    struct timespec cap; const struct timespec *eff = to;
    if (g_synth_qn > 0) {
        int64_t rem = (int64_t)g_synth_q[g_synth_qh].deadline - (int64_t)now_ns(); if (rem < 0) rem = 0;
        cap.tv_sec = rem / 1000000000; cap.tv_nsec = rem % 1000000000;
        uint64_t capn = (uint64_t)cap.tv_sec*1000000000ull + cap.tv_nsec;
        if (!to || (uint64_t)to->tv_sec*1000000000ull + to->tv_nsec > capn) eff = &cap;
    }
    int n = real_epoll_pwait2(epfd, events, maxevents, eff, ss);
    return epoll_synth(epfd, events, n, maxevents);
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
/* Exported so libhybris (eglplatformcommon) can recover the gralloc handle
 * behind a gbm_hybris dmabuf fd when wlroots imports it as an EGL image --
 * the hybris EGL imports gralloc ANativeWindowBuffers, not generic Linux
 * dmabufs, so the dmabuf import has to be bridged to a native-buffer import. */
buffer_handle_t drm_shim_lookup_gralloc(uint32_t fd) {
    return find_gralloc(fd);
}

/* Present a wlroots-rendered gralloc buffer to HWC2. wlroots has no drmadapter
 * EGL window surface, so drmadapter's present_cb (which mutter's eglSwapBuffers
 * drives) never runs and the faked KMS commit alone wouldn't reach the panel.
 * Hand the committed buffer to libhybris, which presents the matching
 * RemoteWindowBuffer via drmadapter's HWC2 display/layer. Only under
 * HYBRIS_WLROOTS; the mutter path presents itself. */
static int g_wlroots = -1;
/* The drmadapter EGL platform registers its HWC2 present callback here at init.
 * It lives in a ws module dlopen()'d RTLD_LAZY (local scope), so it can't be
 * reached by dlsym from here -- but this shim is globally preloaded, so the
 * registration goes the other way (drmadapter -> us). */
static int (*g_present_fn)(buffer_handle_t) = NULL;
void drm_shim_set_present(int (*fn)(buffer_handle_t)) {
    g_present_fn = fn;
    if (hybris_debug.sample) LOG("present callback registered fn=%p", (void*)fn);
}
/* Single-pass CPU present (QPainter/software compositors): drmadapter copies
 * the dumb-buffer mapping straight into its present buffer, skipping the
 * intermediate gralloc scratch copy this shim otherwise does. */
static int (*g_present_cpu_fn)(const void *, uint32_t) = NULL;
void drm_shim_set_present_cpu(int (*fn)(const void *, uint32_t)) {
    g_present_cpu_fn = fn;
    if (hybris_debug.sample) LOG("cpu present callback registered fn=%p", (void*)fn);
}
/* drmadapter also registers a power callback so we can drive the real HWC2
 * display power off/on when wlroots toggles the CRTC ACTIVE state (DPMS). The
 * faked atomic commit otherwise never touches the panel power: "blanking" just
 * stops phoc presenting (backlight stays on showing the last frame) and wake
 * never powers anything back -- so the screen looks frozen. With this, an
 * output-disable commit powers the panel down and the re-enable commit (driven
 * by phosh on wake input) powers it back up. */
static void (*g_power_fn)(int) = NULL;
void drm_shim_set_power(void (*fn)(int)) {
    g_power_fn = fn;
    if (hybris_debug.sample) LOG("power callback registered fn=%p", (void*)fn);
}
/* wlroots' render-completion fence for the current commit (the plane's
 * IN_FENCE_FD). We fake the KMS commit, so the kernel never waits on it -- we
 * must, or the blit/HWC2 scans out a half-rendered (flickery/black) buffer.
 * Captured in drmModeAtomicAddProperty; consumed (waited + closed) here. */
static int g_committed_fence = -1;
static void present_hwc2(buffer_handle_t h) {
    if (g_wlroots < 0)
        g_wlroots = hybris_is_wlroots() || getenv("HYBRIS_WLROOTS") ? 1 : 0;
    if (!g_wlroots || !h || !g_present_fn) {
        if (g_committed_fence >= 0) { close(g_committed_fence); g_committed_fence = -1; }
        return;
    }
    /* Block until wlroots' GPU render into this buffer is complete. The sync
     * file becomes readable (POLLIN) when signalled; act as the kernel that
     * consumes the in-fence. */
    if (g_committed_fence >= 0) {
        struct pollfd pfd = { .fd = g_committed_fence, .events = POLLIN };
        int saved = in_hook; in_hook = 1;
        real_poll ? real_poll(&pfd, 1, 1000) : poll(&pfd, 1, 1000);
        in_hook = saved;
        static int logged = 0;
        if (!logged && hybris_debug.sample) { LOG("waited on IN_FENCE_FD %d", g_committed_fence); logged = 1; }
        close(g_committed_fence); g_committed_fence = -1;
    }
    int rc = g_present_fn(h);
    static int logged2 = 0;
    if (!logged2 && hybris_debug.sample) { LOG("first present_hwc2(h=%p) rc=%d", (void*)h, rc); logged2 = 1; }
    (void)rc;
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
static unsigned long g_commit_n = 0;
/* Diagnostic: scan every registered gralloc buffer for non-black content, to
 * tell whether the rendered frame landed in a buffer we simply didn't pick. */
static void sample_all_buffers(void) {
    if (!frame_w || !frame_h) return;
    for (int i = 0; i < gmap_n; i++) {
        buffer_handle_t h = gmap[i].gralloc;
        if (!h) continue;
        void *s = NULL;
        if (hybris_gralloc_lock(h, 0x3, 0, 0, frame_w, frame_h, &s) || !s) continue;
        unsigned long nz = 0;
        for (uint32_t y = 0; y < frame_h; y += 64)
            for (uint32_t x = 0; x < frame_w; x += 64) {
                uint8_t *p = (uint8_t*)s + (size_t)y*frame_w*4 + x*4;
                if (p[0]|p[1]|p[2]) nz++;
            }
        hybris_gralloc_unlock(h);
        if (nz) LOG("buffer[%d] gralloc=%p NONBLACK samples=%lu", i, (void*)h, nz);
    }
}
static unsigned long g_lockfail = 0;
static void syncstat(long lock_us, long body_us, long unlock_us) {
    static int on = -1;
    if (on < 0) on = getenv("LIBDRM_HYBRIS_SYNCSTATS") ? 1 : 0;
    if (!on) return;
    static unsigned long n = 0, tl = 0, tb = 0, tu = 0; static long mx = 0;
    long tot = lock_us + body_us + unlock_us;
    n++; tl += lock_us; tb += body_us; tu += unlock_us; if (tot > mx) mx = tot;
    if ((n % 120) == 0) {
        LOG("sync %lu frames  lock %lu us  body %lu us  unlock %lu us  total %lu us (max %ld)  lockfail=%lu",
                n, tl/n, tb/n, tu/n, (tl+tb+tu)/n, mx, g_lockfail);
        mx = 0;
    }
}
/* Frame pacing histogram: interval between successive syncs, bucketed against
 * the 8.33 ms (120 Hz) period. Distinguishes jitter from a low average rate. */
static void pacestat(void) {
    static int on = -1;
    if (on < 0) on = getenv("LIBDRM_HYBRIS_SYNCSTATS") ? 1 : 0;
    if (!on) return;
    static uint64_t prev = 0;
    static unsigned long n = 0, b[6];
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    uint64_t now = (uint64_t)t.tv_sec*1000000000ull + t.tv_nsec;
    if (prev) {
        uint64_t d = (now - prev) / 1000;
        int i = d < 6000 ? 0 : d < 10000 ? 1 : d < 14000 ? 2
              : d < 20000 ? 3 : d < 40000 ? 4 : 5;
        b[i]++; n++;
        if ((n % 120) == 0)
            LOG("pace <6ms:%lu  6-10(120Hz):%lu  10-14:%lu  14-20(60Hz):%lu  20-40:%lu  >40ms:%lu",
                    b[0],b[1],b[2],b[3],b[4],b[5]);
    }
    prev = now;
}
/* The dumb buffer is never scanned out; this exists only to make the GPU
 * resolve its render before HWC2 presents. Measured on-device: lock 94 us,
 * unlock 23 us, full-buffer memcpy body 31000 us -- the traversal was ~100% of
 * the cost and capped the compositor near 30 fps. The lock is what forces the
 * resolve, not the traversal: touching one cache line every ROWSTEP rows gives
 * the same result for ~450 us (69x). LIBDRM_HYBRIS_SYNC=touch selects it. */
static int sync_mode_touch(void) {
    return hybris_tuning.touch_sync;
}
static void copy_to_dumb(buffer_handle_t h) {
    if (!dumb_map || !h || !frame_w || !frame_h) return;
    pacestat();
    struct timespec t0, t1, t2, t3;
    void *src = NULL;
    const int touch = sync_mode_touch();
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int lr = hybris_gralloc_lock(h, 0x3|0x30, 0, 0, frame_w, frame_h, &src);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (lr || !src) {
        g_lockfail++;
        if ((g_lockfail % 60) == 1)
            LOG("gralloc_lock FAILED rc=%d src=%p count=%lu", lr, src, g_lockfail);
        return;
    }
    uint8_t *d = dumb_map, *s = src;
    if (touch) {
        static int rowstep = -1;
        if (rowstep < 0) rowstep = hybris_tuning.row_step;
        volatile uint32_t acc = 0;
        for (uint32_t y = 0; y < frame_h; y += (uint32_t)rowstep) {
            const uint32_t *row = (const uint32_t *)(s + (size_t)y * dumb_pitch);
            for (uint32_t x = 0; x < frame_w; x += 16) acc ^= row[x];
        }
        (void)acc;
    } else {
    /* The full-frame read here is load-bearing: touching every pixel forces the
     * GPU to resolve its render into the buffer before it's presented. */
    for (uint32_t y = 0; y < frame_h; y++)
        memcpy(d + y*dumb_pitch, s + y*dumb_pitch, frame_w*4);
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    /* Diagnostic: is the committed buffer actually non-black? Sample a grid. */
    if (hybris_debug.sample) {
        unsigned long nz = 0; uint32_t cx = frame_w/2, cy = frame_h/2;
        for (uint32_t y = 0; y < frame_h; y += 64)
            for (uint32_t x = 0; x < frame_w; x += 64) {
                uint8_t *p = s + y*dumb_pitch + x*4;
                if (p[0]|p[1]|p[2]) nz++;
            }
        uint8_t *c = s + cy*dumb_pitch + cx*4;
        LOG("commit#%lu h=%p nonblack_samples=%lu center=%02x%02x%02x",
                g_commit_n, (void*)h, nz, c[0], c[1], c[2]);
    }
    hybris_gralloc_unlock(h);
    clock_gettime(CLOCK_MONOTONIC, &t3);
    syncstat((t1.tv_sec-t0.tv_sec)*1000000 + (t1.tv_nsec-t0.tv_nsec)/1000,
             (t2.tv_sec-t1.tv_sec)*1000000 + (t2.tv_nsec-t1.tv_nsec)/1000,
             (t3.tv_sec-t2.tv_sec)*1000000 + (t3.tv_nsec-t2.tv_nsec)/1000);
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

/* --- QPainter (software) present bridge ------------------------------------
 * KWin's QPainter DRM backend renders into DRM *dumb* buffers (CPU-mapped) and
 * page-flips them. Those aren't gralloc buffers, so present_hwc2() can't hand
 * them to HWC2. Track each dumb buffer's CPU mapping (from CREATE_DUMB /
 * MAP_DUMB), and on flip copy the composited pixels into a gralloc scratch
 * buffer that we present through the normal HWC2 path. This is the pure-software
 * compositing path that avoids the hybris GL driver (and its render corruption)
 * entirely. */
#define KDUMB_MAX 8
static struct { uint32_t gem; void *cpu; size_t size; uint32_t pitch; int memfd; } kdumb[KDUMB_MAX];
static int kdumb_n = 0;

/* --- Cached (memfd-backed) dumb buffers for the KWin QPainter path ---------
 * Real DRM dumb buffers mmap as write-combined memory: QPainter's blending
 * (read-modify-write) and our per-frame present readback both stall badly on
 * WC reads (~10-30ms per 1080p+ frame each). The faked KMS never scans these
 * buffers out -- they only ever live as a CPU canvas -- so back them with
 * plain CACHED anonymous memory (memfd) instead. CREATE_DUMB/MAP_DUMB are
 * answered without the kernel; the compositor's subsequent mmap() on the DRM
 * fd is redirected to the memfd by the mmap interpose below (magic offset).
 * Gated on LIBDRM_HYBRIS_FAKE_KMS_STATE (the KWin session): phoc/mutter keep
 * real dumb buffers. */
#define KDUMB_FAKE_GEM(i)   (0x4B440000u | (uint32_t)(i))
#define KDUMB_IS_FAKE(h)    (((h) & 0xFFFF0000u) == 0x4B440000u)
#define KDUMB_FAKE_OFF(i)   ((0x4B44ull << 40) | ((uint64_t)(i) << 20))
#define KDUMB_OFF_MAGIC(o)  (((uint64_t)(o) >> 40) == 0x4B44ull)
#define KDUMB_OFF_IDX(o)    ((int)(((uint64_t)(o) >> 20) & 0xFFFFFu))

static int kdumb_slot_by_gem(uint32_t gem) {
    for (int i = 0; i < kdumb_n; i++) if (kdumb[i].gem == gem) return i;
    return -1;
}
static int kdumb_create_memfd(struct drm_mode_create_dumb *cd) {
    int slot = -1;
    for (int i = 0; i < kdumb_n; i++) if (!kdumb[i].gem) { slot = i; break; }
    if (slot < 0) {
        if (kdumb_n >= KDUMB_MAX) return -ENOMEM;
        slot = kdumb_n++;
    }
    /* Deferred unmap from a previous DESTROY_DUMB of this slot (see below). */
    if (kdumb[slot].cpu) { munmap(kdumb[slot].cpu, kdumb[slot].size); kdumb[slot].cpu = NULL; }
    uint32_t pitch = cd->width * ((cd->bpp + 7) / 8);
    size_t size = ((size_t)pitch * cd->height + 4095) & ~(size_t)4095;
    int mfd = (int)syscall(SYS_memfd_create, "libdrm-hybris-dumb", 0);
    if (mfd < 0) return -errno;
    if (ftruncate(mfd, (off_t)size) < 0) { int e = errno; close(mfd); return -e; }
    void *cpu = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (cpu == MAP_FAILED) { int e = errno; close(mfd); return -e; }
    kdumb[slot].gem = KDUMB_FAKE_GEM(slot);
    kdumb[slot].cpu = cpu;
    kdumb[slot].size = size;
    kdumb[slot].pitch = pitch;
    kdumb[slot].memfd = mfd;
    cd->handle = kdumb[slot].gem;
    cd->pitch = pitch;
    cd->size = size;
    if (hybris_debug.sample)
        LOG("kdumb memfd create %ux%u gem=0x%x fd=%d (cached)",
                cd->width, cd->height, cd->handle, mfd);
    return 0;
}
static void kdumb_destroy_memfd(uint32_t gem) {
    int i = kdumb_slot_by_gem(gem);
    if (i < 0) return;
    /* Keep the CPU mapping alive: the async present worker may still be
     * copying from it (~ms). It is munmapped when the slot is reused, by
     * which point every in-flight frame has long been presented. */
    if (kdumb[i].memfd >= 0) close(kdumb[i].memfd);
    kdumb[i].gem = 0; kdumb[i].memfd = -1;
}
/* mmap interpose: redirect the compositor's mapping of a fake dumb offset to
 * the backing memfd. Passthrough goes straight to the kernel (raw syscall) so
 * there is no dlsym/recursion hazard; everything not carrying the magic
 * offset is untouched. aarch64 mmap takes the byte offset directly. */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    if (KDUMB_OFF_MAGIC(offset)) {
        int idx = KDUMB_OFF_IDX(offset);
        if (idx >= 0 && idx < kdumb_n && kdumb[idx].memfd >= 0)
            return (void *)syscall(SYS_mmap, addr, length, prot, flags, kdumb[idx].memfd, (off_t)0);
    }
    return (void *)syscall(SYS_mmap, addr, length, prot, flags, fd, offset);
}
void *mmap64(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
    __attribute__((alias("mmap")));
static buffer_handle_t g_qp_gralloc = NULL;
static uint32_t g_qp_stride = 0;
static void kdumb_note_create(uint32_t gem, size_t size, uint32_t pitch) {
    if (hybris_debug.sample)
        LOG("kdumb_create gem=%u size=%zu pitch=%u", gem, size, pitch);
    for (int i=0;i<kdumb_n;i++) if (kdumb[i].gem==gem){ kdumb[i].size=size; kdumb[i].pitch=pitch; kdumb[i].cpu=NULL; return; }
    if (kdumb_n<KDUMB_MAX){ kdumb[kdumb_n].gem=gem; kdumb[kdumb_n].cpu=NULL; kdumb[kdumb_n].size=size; kdumb[kdumb_n].pitch=pitch; kdumb_n++; }
}
static void kdumb_note_map(int fd, uint32_t gem, uint64_t offset) {
    for (int i=0;i<kdumb_n;i++) if (kdumb[i].gem==gem){
        if (!kdumb[i].cpu && kdumb[i].size){
            int sv=in_hook; in_hook=1;
            void *m = mmap(NULL, kdumb[i].size, PROT_READ, MAP_SHARED, fd, (off_t)offset);
            in_hook=sv;
            if (m!=MAP_FAILED) kdumb[i].cpu=m;
            if (hybris_debug.sample)
                LOG("kdumb_map gem=%u size=%zu off=0x%llx -> %p (errno=%d)",
                        gem, kdumb[i].size, (unsigned long long)offset, m, errno);
        }
        return;
    }
    if (hybris_debug.sample)
        LOG("kdumb_map gem=%u NOT in kdumb (n=%d)", gem, kdumb_n);
}
/* KWin exports each dumb buffer with drmPrimeHandleToFD (dumb gem -> fd) and
 * then builds the scanout FB from that fd's handle, so the flipped fb's "gem"
 * is the exported fd, not the CREATE_DUMB handle. Map fd -> dumb gem so we can
 * still find the CPU mapping. */
static struct { int fd; uint32_t dumb_gem; } primemap[MAX];
static int primemap_n = 0;
static void primemap_add(int fd, uint32_t dumb_gem) {
    for (int i=0;i<primemap_n;i++) if (primemap[i].fd==fd){ primemap[i].dumb_gem=dumb_gem; return; }
    if (primemap_n<MAX){ primemap[primemap_n].fd=fd; primemap[primemap_n].dumb_gem=dumb_gem; primemap_n++; }
    else { primemap[0].fd=fd; primemap[0].dumb_gem=dumb_gem; }
}
static uint32_t primemap_dumb(uint32_t fd) {
    for (int i=0;i<primemap_n;i++) if ((uint32_t)primemap[i].fd==fd) return primemap[i].dumb_gem;
    return 0;
}
static void *kdumb_cpu(uint32_t gem, uint32_t *pitch) {
    for (int i=0;i<kdumb_n;i++) if (kdumb[i].gem==gem){ if(pitch)*pitch=kdumb[i].pitch; return kdumb[i].cpu; }
    uint32_t dg = primemap_dumb(gem);   /* gem may be the exported prime fd */
    if (dg) for (int i=0;i<kdumb_n;i++) if (kdumb[i].gem==dg){ if(pitch)*pitch=kdumb[i].pitch; return kdumb[i].cpu; }
    return NULL;
}
/* KWin's QPainter backend never initialises EGL, so the drmadapter platform
 * (which registers drm_shim_set_present + brings up HWC2 in its init_module) is
 * never loaded. Force it: dlopen libEGL and eglInitialize once, which loads the
 * HYBRIS_EGLPLATFORM=drmadapter module and wires up g_present_fn + HWC2. */
static void ensure_present_fn(void) {
    if (g_present_fn) return;
    static int tried = 0;
    if (tried) return;
    tried = 1;
    int sv = in_hook; in_hook = 1;
    void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (egl) {
        void *(*getdisp)(void *) = (void *(*)(void *))dlsym(egl, "eglGetDisplay");
        unsigned (*init)(void *, int *, int *) = (unsigned(*)(void *,int *,int *))dlsym(egl, "eglInitialize");
        if (getdisp && init) {
            void *d = getdisp((void *)0);          /* EGL_DEFAULT_DISPLAY */
            if (d) { int mj = 0, mn = 0; init(d, &mj, &mn); }
        }
    }
    in_hook = sv;
    if (hybris_debug.sample)
        LOG("forced drmadapter EGL init, present_fn=%p", (void *)g_present_fn);
}
static int present_qpainter_dumb(uint32_t gem) {
    uint32_t spitch=0;
    void *src = kdumb_cpu(gem, &spitch);
    if (hybris_debug.sample) {
        static int n=0;
        if (n++ < 5) LOG("present_qpainter_dumb(gem=%u) src=%p kdumb_n=%d fw=%u",
                             gem, src, kdumb_n, frame_w);
    }
    if (!src || !frame_w || !frame_h) return 0;
    ensure_present_fn();
    /* Preferred: hand the dumb mapping straight to drmadapter for a single
     * swizzling copy into its present buffer (one full-frame pass instead of
     * two). When registered, it is authoritative: on failure (HWC2 not up yet)
     * skip the frame rather than fall through -- the scratch path below would
     * call hybris_gralloc_allocate before gralloc is loaded and assert. */
    if (g_present_cpu_fn) {
        return g_present_cpu_fn(src, spitch ? spitch : frame_w * 4) == 0 ? 1 : 0;
    }
    if (!g_qp_gralloc) {
        const int usage = 0x1000|0x800|0x200|0x33;   /* FB|COMPOSER|RENDER|SW rw */
        if (hybris_gralloc_allocate((int)frame_w, (int)frame_h, 1 /*RGBA_8888*/, usage,
                                    &g_qp_gralloc, &g_qp_stride) || !g_qp_gralloc) {
            g_qp_gralloc=NULL; return 0;
        }
    }
    void *dst=NULL;
    if (hybris_gralloc_lock(g_qp_gralloc, 0x3|0x30, 0, 0, (int)frame_w, (int)frame_h, &dst) || !dst) return 0;
    uint32_t dpitch = g_qp_stride*4;
    if (!spitch) spitch = frame_w*4;
    for (uint32_t y=0;y<frame_h;y++)
        memcpy((uint8_t*)dst + (size_t)y*dpitch, (uint8_t*)src + (size_t)y*spitch, (size_t)frame_w*4);
    hybris_gralloc_unlock(g_qp_gralloc);
    present_hwc2(g_qp_gralloc);
    return 1;
}
static uint32_t find_gem_by_fb(uint32_t fb_id) {
    for (int i=0;i<fmap_n;i++) if (fmap[i].fb_id==fb_id) return fmap[i].gem;
    return 0;
}

void drm_shim_panel_power(int on);   /* defined below */
/* Legacy DPMS: KWin (legacy KMS, KWIN_DRM_NO_AMS) signals panel power by setting
 * the connector "DPMS" property, which needs DRM master (HWC2 owns it) and
 * returns EACCES -> the panel never blanks (lit black screen when locked/idle).
 * Intercept it and drive the HWC2 backlight via drm_shim_panel_power() instead.
 * Applies to KWin only (detected from the process name) so phoc and mutter are
 * untouched; LIBDRM_HYBRIS_DPMS_FROM_ACTIVE forces it on for a compositor this
 * does not recognise. DPMS values: 0=On, 3=Off. */
int drmModeConnectorSetProperty(int fd, uint32_t connector_id, uint32_t property_id, uint64_t value) {
    typedef int (*fn_t)(int,uint32_t,uint32_t,uint64_t);
    fn_t real=(fn_t)hybris_resolve_next("drmModeConnectorSetProperty",(void*)drmModeConnectorSetProperty);
    static int drive = -1;
    if (drive < 0)
        drive = hybris_is_kwin() || getenv("LIBDRM_HYBRIS_DPMS_FROM_ACTIVE") ? 1 : 0;
    if (hybris_debug.sample)
        LOG("ConnectorSetProperty conn=%u prop=%u val=%llu drive=%d comp=%d",
                connector_id, property_id, (unsigned long long)value, drive, hybris_is_compositor());
    if (drive && hybris_is_compositor() && !hybris_is_gnome() && g_drm_fd < 0) g_drm_fd = fd;
    if (drive && hybris_is_compositor() && !hybris_is_gnome()) {
        int sv = in_hook; in_hook = 1;
        drmModePropertyPtr p = drmModeGetProperty(fd, property_id);
        int is_dpms = (p && strcmp(p->name, "DPMS") == 0);
        if (hybris_debug.sample)
            LOG("prop name=[%s] is_dpms=%d", p ? p->name : "(null)", is_dpms);
        if (p) drmModeFreeProperty(p);
        in_hook = sv;
        if (is_dpms) {
            drm_shim_panel_power(value == 0 ? 1 : 0);   /* 0=On -> panel on */
            if (hybris_debug.sample)
                LOG("DPMS property -> %llu (panel %s)",
                        (unsigned long long)value, value == 0 ? "ON" : "OFF");
            return 0;
        }
    }
    return real ? real(fd,connector_id,property_id,value) : -ENOSYS;
}

/* KWin 6 sets DPMS via the object-property API (its log says "object ID: N"),
 * not the connector-specific one. Same treatment. */
int drmModeObjectSetProperty(int fd, uint32_t object_id, uint32_t object_type,
                             uint32_t property_id, uint64_t value) {
    typedef int (*fn_t)(int,uint32_t,uint32_t,uint32_t,uint64_t);
    fn_t real=(fn_t)hybris_resolve_next("drmModeObjectSetProperty",(void*)drmModeObjectSetProperty);
    static int drive = -1;
    if (drive < 0)
        drive = hybris_is_kwin() || getenv("LIBDRM_HYBRIS_DPMS_FROM_ACTIVE") ? 1 : 0;
    if (drive && hybris_is_compositor() && !hybris_is_gnome()) {
        if (g_drm_fd < 0) g_drm_fd = fd;
        int sv = in_hook; in_hook = 1;
        drmModePropertyPtr p = drmModeGetProperty(fd, property_id);
        int is_dpms = (p && strcmp(p->name, "DPMS") == 0);
        if (p) drmModeFreeProperty(p);
        in_hook = sv;
        if (is_dpms) {
            drm_shim_panel_power(value == 0 ? 1 : 0);
            if (hybris_debug.sample)
                LOG("DPMS(obj) -> %llu (panel %s)",
                        (unsigned long long)value, value == 0 ? "ON" : "OFF");
            return 0;
        }
    }
    return real ? real(fd,object_id,object_type,property_id,value) : -ENOSYS;
}

int drmModeAddFB2WithModifiers(int fd, uint32_t w, uint32_t h, uint32_t fmt,
    const uint32_t handles[4], const uint32_t pitches[4], const uint32_t offsets[4],
    const uint64_t mod[4], uint32_t *buf_id, uint32_t flags) {
    if (!hybris_is_compositor()) {
        typedef int (*fn_t)(int,uint32_t,uint32_t,uint32_t,const uint32_t*,
                            const uint32_t*,const uint32_t*,const uint64_t*,uint32_t*,uint32_t);
        fn_t real=(fn_t)hybris_resolve_next("drmModeAddFB2WithModifiers",
                                     (void*)drmModeAddFB2WithModifiers);
        return real ? real(fd,w,h,fmt,handles,pitches,offsets,mod,buf_id,flags) : -ENOSYS;
    }
    if (!frame_w) { frame_w=w; frame_h=h; }
    if (!dumb_map) init_dumb(fd);
    uint32_t id=next_fake++; *buf_id=id; fmap_insert(handles[0],id);
    if (hybris_debug.sample) LOG("AddFB2Mod fb=%u handle0=%u",id,handles[0]);
    return 0;
}
/* Legacy AddFB. KWin's legacy DRM path (KWIN_DRM_NO_AMS) registers its scanout
 * buffer with this rather than AddFB2, and without an interposer it reaches the
 * real driver and comes back with a real fb id. That id is not in our map, so
 * every page flip resolves to no gralloc buffer: nothing is presented (black
 * screen) and, with no completion, the repaint loop stalls after a frame or
 * two. Hand out a fake id and record the handle exactly as AddFB2 does. */
int drmModeAddFB(int fd, uint32_t w, uint32_t h, uint8_t depth, uint8_t bpp,
                 uint32_t pitch, uint32_t bo_handle, uint32_t *buf_id) {
    TRACEALL("drmModeAddFB");
    if (!hybris_is_compositor() || !fake_kms_state()) {
        typedef int (*fn_t)(int,uint32_t,uint32_t,uint8_t,uint8_t,uint32_t,uint32_t,uint32_t*);
        fn_t real=(fn_t)hybris_resolve_next("drmModeAddFB",(void*)drmModeAddFB);
        return real ? real(fd,w,h,depth,bpp,pitch,bo_handle,buf_id) : -ENOSYS;
    }
    if (!frame_w) { frame_w=w; frame_h=h; }
    if (!dumb_map) init_dumb(fd);
    uint32_t id=next_fake++; if (buf_id) *buf_id=id; fmap_insert(bo_handle,id);
    if (hybris_debug.sample)
        LOG("AddFB fb=%u handle=%u pitch=%u %ux%u bpp=%u",
                id, bo_handle, pitch, w, h, bpp);
    return 0;
}

int drmModeAddFB2(int fd, uint32_t w, uint32_t h, uint32_t fmt,
    const uint32_t handles[4], const uint32_t pitches[4], const uint32_t offsets[4],
    uint32_t *buf_id, uint32_t flags) {
    TRACEALL("drmModeAddFB2");
    if (!hybris_is_compositor()) {
        typedef int (*fn_t)(int,uint32_t,uint32_t,uint32_t,const uint32_t*,
                            const uint32_t*,const uint32_t*,uint32_t*,uint32_t);
        fn_t real=(fn_t)hybris_resolve_next("drmModeAddFB2",(void*)drmModeAddFB2);
        return real ? real(fd,w,h,fmt,handles,pitches,offsets,buf_id,flags) : -ENOSYS;
    }
    if (!frame_w) { frame_w=w; frame_h=h; }
    if (!dumb_map) init_dumb(fd);
    uint32_t id=next_fake++; *buf_id=id; fmap_insert(handles[0],id);
    if (hybris_debug.sample)
        LOG("AddFB2 fb=%u handle0=%u pitch0=%u %ux%u",
                id, handles[0], pitches?pitches[0]:0, w, h);
    return 0;
}
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle) {
    typedef int (*fn_t)(int,int,uint32_t*);
    fn_t real=(fn_t)hybris_resolve_next("drmPrimeFDToHandle",(void*)drmPrimeFDToHandle);
    if (!hybris_is_compositor() || hybris_is_gnome())
        return real ? real(fd,prime_fd,handle) : -ENOSYS;
    int r = real ? real(fd,prime_fd,handle) : -EACCES;
    /* wlroots imports the gbm_bo's PRIME fd for scan-out, which needs DRM
     * master -- the HWC2 composer owns it, so the real import returns EACCES.
     * Use the prime fd itself as the GEM handle: gbm_hybris registered the
     * prime_fd -> gralloc mapping (gmap) and find_gralloc()/find_by_fb() key on
     * the prime fd, so AddFB2/commit can still recover the buffer. */
    if (r != 0 && handle) { *handle = (uint32_t)prime_fd; r = 0; }
    if (hybris_debug.sample && handle) LOG("PrimeFDToHandle fd=%d -> handle=%u",prime_fd,*handle);
    return r;
}
/* Reverse of the above: KWin's QPainter (software) compositing backend allocates
 * its swapchain buffer via gbm and then exports it with drmPrimeHandleToFD to get
 * a dmabuf fd. The real ioctl needs DRM master (the HWC2 composer owns it) and
 * returns EACCES -> "Failed to allocate a qpainter swapchain graphics buffer".
 * Our GEM handle IS the gbm_hybris prime fd (see drmPrimeFDToHandle), so just dup
 * it back. This lets KWin composite in software with zero hybris GL -- the escape
 * hatch from the hybris GL-render corruption. */
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd) {
    typedef int (*fn_t)(int,uint32_t,uint32_t,int*);
    fn_t real=(fn_t)hybris_resolve_next("drmPrimeHandleToFD",(void*)drmPrimeHandleToFD);
    if (!hybris_is_compositor() || hybris_is_gnome())
        return real ? real(fd,handle,flags,prime_fd) : -ENOSYS;
    /* Memfd-backed fake dumb buffer: "export" the memfd itself. */
    if (KDUMB_IS_FAKE(handle) && prime_fd) {
        int slot = kdumb_slot_by_gem(handle);
        if (slot >= 0 && kdumb[slot].memfd >= 0) {
            int dfd = dup(kdumb[slot].memfd);
            if (dfd >= 0) { *prime_fd = dfd; primemap_add(dfd, handle); return 0; }
        }
        return -EINVAL;
    }
    int r = real ? real(fd,handle,flags,prime_fd) : -EACCES;
    if (r != 0 && prime_fd) {
        int dfd = dup((int)handle);       /* handle == the gbm_hybris prime fd */
        if (dfd >= 0) { *prime_fd = dfd; r = 0; primemap_add(dfd, handle); }
        if (hybris_debug.sample) LOG("PrimeHandleToFD handle=%u -> fd=%d",handle,dfd);
    }
    return r;
}
int drmModeRmFB(int fd, uint32_t id) {
    TRACEALL("drmModeRmFB");
    if (!hybris_is_compositor()) {
        typedef int (*fn_t)(int,uint32_t);
        fn_t real=(fn_t)hybris_resolve_next("drmModeRmFB",(void*)drmModeRmFB);
        return real ? real(fd,id) : 0;
    }
    return 0;
}
int drmModeSetCrtc(int fd, uint32_t crtcId, uint32_t bufferId, uint32_t x, uint32_t y,
    uint32_t *connectors, int count, drmModeModeInfoPtr mode) {
    TRACEALL("drmModeSetCrtc");
    if (!hybris_is_compositor()) {
        typedef int (*fn_t)(int,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t*,int,drmModeModeInfoPtr);
        fn_t real=(fn_t)hybris_resolve_next("drmModeSetCrtc",(void*)drmModeSetCrtc);
        return real ? real(fd,crtcId,bufferId,x,y,connectors,count,mode) : -ENOSYS;
    }
    if (!dumb_map) init_dumb(fd);

    /* mutter blanks by calling meta_kms_device_disable(), which on the legacy
     * KMS path (MUTTER_DEBUG_FORCE_KMS_MODE=simple, as the gnome-mali session
     * sets) arrives here as a CRTC disable: no fb, no connectors, no mode.
     * Those DRM calls are swallowed below, so nothing ever reached the panel --
     * the backlight went to 0 while the DSI panel and the MediaTek display
     * pipeline kept clocking (~60 mtk_cmdq interrupts/sec with the screen
     * dark). mutter never emits an atomic ACTIVE commit here and never touches
     * the DPMS property, so neither of the existing paths could fire.
     * Translate the disable/enable into HWC2 panel power directly. */
    /* KWin's QPainter backend presents its very first frame via a modeset, then
     * page-flips. Present that first dumb buffer too so the panel isn't blank
     * until the first flip. Only fires for tracked dumb buffers (gralloc paths
     * return h!=NULL and are handled by their flip/commit). */
    if (bufferId) {
        buffer_handle_t h = find_by_fb(bufferId);
        if (!h) present_qpainter_dumb(find_gem_by_fb(bufferId));
    }
    return 0;
}
int drmModePageFlip(int fd, uint32_t crtc_id, uint32_t fb_id, uint32_t flags, void *ud) {
    TRACEALL("drmModePageFlip");
    typedef int (*fn_t)(int,uint32_t,uint32_t,uint32_t,void*);
    fn_t real=(fn_t)hybris_resolve_next("drmModePageFlip",(void*)drmModePageFlip);
    if (!hybris_is_compositor())
        return real ? real(fd,crtc_id,fb_id,flags,ud) : -ENOSYS;
    g_drm_fd = fd; synth_note_flip();
    buffer_handle_t h=find_by_fb(fb_id);
    if (hybris_debug.sample) {
        static unsigned long fn = 0;
        if (fn++ < 12)
            LOG("PageFlip fb=%u gem=%u gralloc=%p fmap_n=%d gmap_n=%d",
                    fb_id, find_gem_by_fb(fb_id), (void*)h, fmap_n, gmap_n);
    }
    if (h) { copy_to_dumb(h); present_hwc2(h); }
    else present_qpainter_dumb(find_gem_by_fb(fb_id));  /* KWin QPainter dumb buffer */
    /* Drive the real ioctl exactly as before -- it re-enters our raw-ioctl hook
     * (nr 0xb0), which is where the completion is synthesized. Arming here too
     * would deliver the completion twice per flip. copy_to_dumb + real() stay
     * unconditional: phoc (wlroots) also takes this legacy path and breaks
     * without them. */
    int r = real ? real(fd,crtc_id,dumb_fb_id?dumb_fb_id:fb_id,flags,ud) : 0;
    return (r == -EACCES) ? 0 : r;
}

/* Force an OpenGL ES 3.x context when LIBDRM_HYBRIS_FORCE_GLES3 is set.
 *
 * KWin 6's GL renderer unconditionally uses ES 3.0 entry points (glMapBufferRange
 * et al.) when drawing client window items. On the libhybris/drmadapter stack the
 * context otherwise comes up as ES 2.0, so libepoxy finds no provider for those
 * symbols and abort()s the compositor the instant a client presents a buffer. The
 * driver is ES 3.2 capable (measured on a Mali-G68; the point is the level, not
 * the vendor), so bumping the requested client version fixes
 * it. Env-gated: only the KWin session opts in; phosh/GNOME/mutter keep their ES2
 * contexts and see an exact pass-through (LD_PRELOAD is scrubbed under KWin's
 * AT_SECURE launch, so this must live in the ld.so.preload shim to take effect). */
EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config,
                            EGLContext share_context, const EGLint *attrib_list) {
    typedef EGLContext (*fn_t)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    static fn_t real = NULL;
    if (!real) real = (fn_t)hybris_resolve_next("eglCreateContext", (void *)eglCreateContext);
    if (!real) return EGL_NO_CONTEXT;

    static int force = -1;
    if (force < 0) force = getenv("LIBDRM_HYBRIS_FORCE_GLES3") ? 1 : 0;
    if (!force) return real(dpy, config, share_context, attrib_list);

    EGLint buf[64]; int n = 0, seen = 0;
    if (attrib_list) {
        for (int i = 0; attrib_list[i] != EGL_NONE && n < 60; i += 2) {
            EGLint k = attrib_list[i], v = attrib_list[i + 1];
            /* EGL_CONTEXT_CLIENT_VERSION == EGL_CONTEXT_MAJOR_VERSION_KHR (0x3098) */
            if (k == EGL_CONTEXT_CLIENT_VERSION) { if (v < 3) v = 3; seen = 1; }
            buf[n++] = k; buf[n++] = v;
        }
    }
    if (!seen && n < 60) { buf[n++] = EGL_CONTEXT_CLIENT_VERSION; buf[n++] = 3; }
    buf[n] = EGL_NONE;

    EGLContext c = real(dpy, config, share_context, buf);
    if (getenv("LIBDRM_HYBRIS_STAMP"))
        LOG("eglCreateContext ES3 cfg=%p share=%p -> %p",
                config, share_context, c);
    if (c == EGL_NO_CONTEXT) {         /* config can't do ES3 -> honour original */
        c = real(dpy, config, share_context, attrib_list);
        if (getenv("LIBDRM_HYBRIS_STAMP"))
            LOG("eglCreateContext ES3 FAILED, fallback -> %p", c);
    }
    return c;
}
/* The synthetic flip event must carry the real CRTC id: wlroots' version-3
 * page_flip_handler2 matches the event to a connector by crtc_id
 * (drm_page_flip_pop), and drops it otherwise -> the repaint loop never
 * advances. Discover it from the connected connector's encoder (the HWC2
 * composer already has the panel lit, so the kernel reports a live CRTC). */
static uint32_t g_crtc_id = 0;
static uint32_t discover_crtc(int fd) {
    if (g_crtc_id) return g_crtc_id;
    int saved = in_hook; in_hook = 1;
    drmModeRes *res = drmModeGetResources(fd);
    if (res) {
        for (int i = 0; i < res->count_connectors && !g_crtc_id; i++) {
            drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
            if (c && c->connection == DRM_MODE_CONNECTED && c->encoder_id) {
                drmModeEncoder *e = drmModeGetEncoder(fd, c->encoder_id);
                if (e && e->crtc_id) g_crtc_id = e->crtc_id;
                if (e) drmModeFreeEncoder(e);
            }
            if (c) drmModeFreeConnector(c);
        }
        if (!g_crtc_id && res->count_crtcs > 0) g_crtc_id = res->crtcs[0];
        drmModeFreeResources(res);
    }
    in_hook = saved;
    if (hybris_debug.sample)
        LOG("discovered CRTC id=%u", g_crtc_id);
    return g_crtc_id;
}

/* The atomic request can reference several framebuffers (one per swapchain
 * buffer queued over time); only the FB_ID set on the primary plane in *this*
 * commit is the frame being scanned out. Guessing the most-recent AddFB2 picks
 * the wrong (un-rendered) buffer. Capture the real FB_ID by intercepting the
 * property the compositor sets on the plane. */
static uint32_t g_fbid_prop = 0, g_crtcid_prop = 0, g_infence_prop = 0;
static int g_infence_learned = 0;
static uint32_t g_committed_fb = 0, g_committed_crtc = 0;
/* CRTC ACTIVE property -> drives DPMS (panel power) via g_power_fn. */
static uint32_t g_active_prop = 0;
static int g_pending_active = -1;   /* ACTIVE value seen in the current commit, -1 if none */
static int g_output_on = 1;          /* current panel power state (init: on) */

/* phoc calls this directly (dlsym) from its output-power-management handler to
 * power the panel off/on WITHOUT disabling the wlroots output. Disabling the
 * output tears down the mode and triggers a modeset on re-enable, after which
 * wlroots stops scheduling frames for client damage on this faked-KMS backend
 * (the screen freezes). Keeping the output enabled and only toggling HWC2 panel
 * power avoids the modeset entirely. */
/* mutter's blank never reaches the panel on this backend: it calls
 * meta_kms_device_disable(), whose DRM traffic is swallowed here, and with
 * MUTTER_DEBUG_FORCE_KMS_MODE=simple it emits neither an atomic ACTIVE commit
 * nor a DPMS property set nor a legacy CRTC disable -- all three existing
 * detection paths were checked on device and none fire. Rather than keep
 * guessing which ioctl carries it, take the signal from the one place that is
 * unambiguous: gnome-shell's powerManager, which writes this file in
 * _turnOffScreen()/_turnOnScreen(). Values are HWC2-ish: 0 = off, 1 = on. */
static void *panel_ctl_thread(void *unused) {
    (void)unused;
    char path[128];
    snprintf(path, sizeof path, "/run/user/%u/hybris-display-power",
             (unsigned)getuid());
    time_t last = 0;
    for (;;) {
        struct stat st;
        if (stat(path, &st) == 0 && st.st_mtime != last) {
            last = st.st_mtime;
            FILE *f = fopen(path, "r");
            if (f) {
                int v = -1;
                if (fscanf(f, "%d", &v) == 1 && (v == 0 || v == 1))
                    drm_shim_panel_power(v);
                fclose(f);
            }
        }
        usleep(50000);
    }
    return NULL;
}

__attribute__((constructor))
static void panel_ctl_start(void) {
    /* No env gate: the is_compositor() check below is the real condition, and
     * requiring a variable only meant the session could forget it. */
    /* Only the compositor holds an HWC2 display handle. Without this gate every
     * hybris client that inherits the session env (thumbnailers especially --
     * 1125 of them in one boot) spawns a polling thread that can only ever call
     * setPowerMode with a NULL handle. A battery fix should not ship idle
     * pollers in every process on the system. */
    if (!hybris_is_compositor()) return;
    pthread_t t;
    if (pthread_create(&t, NULL, panel_ctl_thread, NULL) == 0)
        pthread_detach(t);
}

void drm_shim_panel_power(int on) {
    on = on ? 1 : 0;
    if (on == g_output_on) return;
    g_output_on = on;
    if (g_power_fn) {
        g_power_fn(on);
    } else {
        /* Under GNOME nothing calls drm_shim_set_power(), so g_power_fn stays
         * NULL and the panel never powered down -- the backlight went to 0 but
         * the DSI panel and the MediaTek display pipeline kept clocking all
         * night (~60 mtk_cmdq interrupts/sec with the screen dark). phoc
         * registers a callback; mutter has no equivalent, so drive the HAL
         * ourselves. libhwc2.so exports this; HWC2 power modes: 0=OFF, 2=ON. */
        static hwc2_error_t (*set_pm)(void *, int32_t) = NULL;
        static int resolved = 0;
        if (!resolved) {
            set_pm = hybris_resolve_next("hwc2_compat_display_set_power_mode", NULL);
            if (!set_pm) set_pm = dlsym(RTLD_DEFAULT, "hwc2_compat_display_set_power_mode");
            resolved = 1;
            if (getenv("LIBDRM_HYBRIS_PANEL_CTL_DEBUG"))
                LOG("panel power fallback fn=%p display=%p",
                        (void *)set_pm, g_hwc_display);
        }
        if (set_pm && g_hwc_display)
            set_pm(g_hwc_display, on ? HWC2_POWER_MODE_ON : HWC2_POWER_MODE_OFF);
    }
    if (getenv("LIBDRM_HYBRIS_PANEL_CTL_DEBUG"))
        LOG("drm_shim_panel_power -> %s", on ? "ON" : "OFF");
}
/* phoc queries this on input activity: if the panel was blanked, phoc forces it
 * back on (and repaints), so ANY input wakes the screen even when phosh's
 * lockscreen/idle manager does not request the wake itself. */
int drm_shim_panel_is_on(void) { return g_output_on; }
int drmModeAtomicAddProperty(drmModeAtomicReqPtr req, uint32_t obj, uint32_t prop, uint64_t val) {
    typedef int (*fn_t)(drmModeAtomicReqPtr,uint32_t,uint32_t,uint64_t);
    fn_t real = (fn_t)hybris_resolve_next("drmModeAtomicAddProperty",(void*)drmModeAtomicAddProperty);
    if (hybris_is_compositor() && g_drm_fd >= 0 && !in_hook) {
        if (!g_fbid_prop || !g_crtcid_prop || !g_infence_learned || !g_active_prop) { /* learn prop ids (device-global) */
            in_hook = 1;
            drmModePropertyPtr p = drmModeGetProperty(g_drm_fd, prop);
            if (p) {
                if (strcmp(p->name, "FB_ID") == 0) g_fbid_prop = prop;
                else if (strcmp(p->name, "CRTC_ID") == 0) g_crtcid_prop = prop;
                else if (strcmp(p->name, "IN_FENCE_FD") == 0) { g_infence_prop = prop; g_infence_learned = 1; }
                else if (strcmp(p->name, "ACTIVE") == 0) g_active_prop = prop;
                drmModeFreeProperty(p);
            }
            in_hook = 0;
        }
        if (prop == g_fbid_prop && val) g_committed_fb = (uint32_t)val;
        /* CRTC ACTIVE=0 disables the output (DPMS off), =1 re-enables it. */
        if (g_active_prop && prop == g_active_prop) g_pending_active = (int)val;
        /* The CRTC the compositor actually drives this connector with -- the
         * synthetic flip event must carry exactly this id or wlroots'
         * handle_page_flip drops it (no buffer release, no next frame). */
        if (prop == g_crtcid_prop && val) g_committed_crtc = (uint32_t)val;
        /* wlroots' render-completion fence for this frame; present_hwc2 waits on
         * it (then closes it) before the buffer is sampled/scanned out. */
        if (g_infence_prop && prop == g_infence_prop && (int64_t)val >= 0)
            g_committed_fence = (int)val;
    }
    return real ? real(req, obj, prop, val) : -ENOSYS;
}

int drmModeAtomicCommit(int fd, drmModeAtomicReqPtr req, uint32_t flags, void *ud) {
    TRACEALL("drmModeAtomicCommit flags=0x%x", flags);
    if (!hybris_is_compositor()) {
        typedef int (*fn_t)(int,drmModeAtomicReqPtr,uint32_t,void*);
        fn_t real=(fn_t)hybris_resolve_next("drmModeAtomicCommit",(void*)drmModeAtomicCommit);
        return real ? real(fd,req,flags,ud) : -ENOSYS;
    }
    /* Atomic check (TEST_ONLY): just report the config valid, present nothing. */
    if (flags & DRM_MODE_ATOMIC_TEST_ONLY) return 0;
    g_drm_fd = fd; synth_note_flip();
    g_commit_n++;
    STAMP("commit");
    /* Present ONLY commits that carry a new framebuffer (FB_ID captured from the
     * plane property). Commits WITHOUT one (cursor/gamma/empty commits -- frequent
     * during interaction) must NOT present: the old "fall back to the most recent
     * fmap buffer" guess re-presented a STALE frame between real ones, which is
     * exactly the rapid flicker on any motion (static content = no interleaved
     * empty commits = no flicker). */
    buffer_handle_t h = g_committed_fb ? find_by_fb(g_committed_fb) : NULL;
    if (hybris_debug.sample)
        LOG("atomicCommit #%lu flags=0x%x fb=%u h=%p arm=%d",
                g_commit_n, flags, g_committed_fb, (void*)h, (flags & DRM_MODE_PAGE_FLIP_EVENT)?1:0);
    /* DPMS is driven by phoc's output-power handler via drm_shim_panel_power()
     * (which keeps the wlr output enabled -> no modeset -> no freeze). phoc
     * calls that directly; KWin does not -- it signals DPMS only through the
     * CRTC ACTIVE property. So for the KWin session (env-gated), drive the
     * panel power from ACTIVE. Safe here (unlike phoc): once the output is off
     * KWin stops committing frames, so ACTIVE=1 never immediately undoes a
     * panel-off, and there is no backlight oscillation. */
    {
        static int dpms_from_active = -1;
        if (dpms_from_active < 0)
            dpms_from_active = getenv("LIBDRM_HYBRIS_DPMS_FROM_ACTIVE") ? 1 : 0;
        if (dpms_from_active && g_pending_active >= 0)
            drm_shim_panel_power(g_pending_active);
    }
    g_pending_active = -1;
    if (h) {
        /* copy_to_dumb (per-frame CPU read of the render buffer) was needed on
         * the mutter/gnome path; for phoc it is skippable -- and its WRITE-usage
         * gralloc lock forces an AFBC writeback on unlock that RACES the blit's
         * GPU sampling (intermittent stale/black frames = motion flicker; whether
         * a session's buffers are AFBC or linear decides blank/flicker/clean).
         * Keep it only if LIBDRM_HYBRIS_DUMBCOPY=1. */
        static int dc = -1;
        if (dc < 0) {
            const char *e = getenv("LIBDRM_HYBRIS_DUMBCOPY");
            /* Default ON for the gnome/mutter session: the per-frame CPU read
             * is load-bearing there (without it gnome-session stops the shell
             * after ~43s). The wlroots/phoc session presents via its own CPU
             * copy and must NOT also do this (the write-usage lock's AFBC
             * writeback races the presenter). */
            if (e) dc = (*e == '1') ? 1 : 0;
            else   dc = hybris_is_gnome() ? 1 : 0;
        }
        if (dc) copy_to_dumb(h);
        present_hwc2(h);
    }
    else if (g_committed_fb) {
        /* A buffer WAS committed but didn't resolve to a gralloc -- real problem. */
        static unsigned long z = 0;
        if ((z++ % 300) == 0)
            LOG("PRESENT SKIPPED (unresolved fb=%u) #%lu commit=%lu fmap_n=%d gmap_n=%d",
                    g_committed_fb, z, g_commit_n, fmap_n, gmap_n);
    }
    g_committed_fb = 0;
    /* wlroots commits non-blocking and waits for a page-flip completion event
     * before scheduling the next frame. The HWC2 composer owns the CRTC so no
     * real event arrives -- synthesize one (carrying wlroots' user_data) or the
     * repaint loop stalls after a single frame. */
    if (flags & DRM_MODE_PAGE_FLIP_EVENT) {
        uint32_t crtc = g_committed_crtc ? g_committed_crtc : discover_crtc(fd);
        if (hybris_debug.sample && g_commit_n <= 3)
            LOG("synth crtc=%u (committed=%u discovered=%u)",
                    crtc, g_committed_crtc, discover_crtc(fd));
        synth_arm(crtc, (uint64_t)(uintptr_t)ud);
    }
    if (hybris_debug.sample) sample_all_buffers();
    return 0;
}

/* Dispatch uses xf86drm.h's DRM_IOCTL_NR(): the command number only. The full
 * DRM_IOCTL_* constants also encode the argument size, and this shim is
 * preloaded system-wide, so a caller built against a different libdrm version
 * would silently miss an exact match. */

int ioctl(int fd, unsigned long request, ...) {
    ensure_real();
    va_list args; va_start(args,request); void *arg=va_arg(args,void*); va_end(args);
    uint32_t magic=(request>>8)&0xff;
    if (magic != 0x64) return real_ioctl(fd,request,arg);
    /* Only the compositor's DRM ioctls drive the fake KMS framebuffer.
     * Client processes (camera etc) must reach the real DRM driver intact. */
    if (!hybris_is_compositor()) return real_ioctl(fd,request,arg);
    if (in_hook) return real_ioctl(fd,request,arg);
    uint32_t nr=request&0xff;
    in_hook=1; int ret;
    TRACEALL("ioctl nr=0x%02x", nr);
    if (nr==DRM_IOCTL_NR(DRM_IOCTL_MODE_ADDFB) && fake_kms_state()) {     /* ADDFB (legacy): fake the id, record the handle */
        struct drm_mode_fb_cmd *c=arg;
        if (!frame_w) { frame_w=c->width; frame_h=c->height; }
        if (!dumb_map) init_dumb(fd);
        uint32_t id=next_fake++; c->fb_id=id; fmap_insert(c->handle,id);
        if (hybris_debug.sample)
            LOG("ioctl AddFB fb=%u handle=%u %ux%u",
                    id, c->handle, c->width, c->height);
        ret=0;
    } else if (nr==DRM_IOCTL_NR(DRM_IOCTL_MODE_ADDFB2)) {
        /* Was indexing arg as a raw uint32_t array with the wrong offsets:
         * drm_mode_fb_cmd2 is {fb_id, width, height, pixel_format, flags,
         * handles[4], ...}, so fb[0]/fb[1] read fb_id/width as width/height,
         * and fb[6]=id wrote the fake id into handles[1] instead of fb_id. */
        struct drm_mode_fb_cmd2 *fb=arg;
        if (!frame_w) { frame_w=fb->width; frame_h=fb->height; }
        if (!dumb_map) init_dumb(fd);
        uint32_t id=next_fake++;
        fb->fb_id=id;
        fmap_insert(fb->handles[0], id);
        ret=0;
    } else if (nr==DRM_IOCTL_NR(DRM_IOCTL_MODE_RMFB)) { ret=real_ioctl(fd,request,arg);
    } else if (nr==DRM_IOCTL_NR(DRM_IOCTL_MODE_SETCRTC)) {
        if (!dumb_map) init_dumb(fd);
        real_ioctl(fd,request,arg); ret=0;
    } else if (nr==DRM_IOCTL_NR(DRM_IOCTL_MODE_PAGE_FLIP)) {
        /* Legacy PAGE_FLIP (KWin's legacy DRM path).
         *
         * GETPLANE (0xb6) used to be handled here too, which was a type
         * confusion: its argument is a struct drm_mode_get_plane, not a
         * drm_mode_crtc_page_flip. Reading flip->fb_id actually read the
         * plane's crtc_id (both at byte 4), and the fb_id rewrite below
         * clobbered that field in the caller's query. mutter enumerates planes
         * via drmModeGetPlane(), so this fired in a normal session. GETPLANE
         * now falls through to the real ioctl, which is what it always needed. Present via HWC2 and ALWAYS
         * synthesize the completion event: the real ioctl against the faked KMS
         * state can return 0 (not just EACCES), and without the completion the
         * compositor never schedules the next frame (renders 1-2 frames then
         * idles on a black screen). */
        struct drm_mode_crtc_page_flip *flip=arg;
        g_drm_fd=fd; synth_note_flip();
        buffer_handle_t h=find_by_fb(flip->fb_id);
        /* Frame-cycle profiling (LIBDRM_HYBRIS_PROF): flip-to-flip interval =
         * the compositor's whole frame period (composite + present + pacing). */
        {
            static int prof = -1;
            if (prof < 0) prof = hybris_debug.profile ? 1 : 0;
            if (prof) {
                static struct timespec last; static double acc = 0.0, accP = 0.0; static int n = 0;
                struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
                if (last.tv_sec) {
                    acc += (now.tv_sec - last.tv_sec) * 1e3 + (now.tv_nsec - last.tv_nsec) / 1e6;
                    /* produce = completion-delivery -> this flip: the compositor's
                     * own frame time (composite + client sync), pacing excluded. */
                    uint64_t nn = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
                    if (g_last_deliver_ns && nn > g_last_deliver_ns)
                        accP += (nn - g_last_deliver_ns) / 1e6;
                    if (++n >= 30) {
                        LOG("flip-to-flip avg %.2f ms (%.1f fps) | produce avg %.2f ms",
                                acc / n, n * 1000.0 / acc, accP / n);
                        acc = 0.0; accP = 0.0; n = 0;
                    }
                }
                last = now;
            }
        }
        if (h) { copy_to_dumb(h); present_hwc2(h); }
        else {
            if (hybris_debug.sample) {
                static unsigned long miss = 0;
                if (miss++ < 8)
                    LOG("flip fb=%u UNRESOLVED gem=%u fmap_n=%d gmap_n=%d",
                            flip->fb_id, find_gem_by_fb(flip->fb_id), fmap_n, gmap_n);
            }
            present_qpainter_dumb(find_gem_by_fb(flip->fb_id));  /* KWin QPainter dumb buffer */
        }
        if (dumb_fb_id) flip->fb_id=dumb_fb_id;
        ret=real_ioctl(fd,request,arg);
        /* Arm regardless of the real result: phoc's flip returns EACCES (as
         * before), KWin's returns 0 -- both need the synthetic completion or the
         * repaint loop stalls. copy_to_dumb + real_ioctl stay unconditional so
         * the wlroots (phoc) legacy path is byte-for-byte as it was. */
        if (flip->flags & DRM_MODE_PAGE_FLIP_EVENT)
            synth_arm(flip->crtc_id, flip->user_data);
        ret=0;
    } else if (nr==DRM_IOCTL_NR(DRM_IOCTL_MODE_ATOMIC)) {
        for (int i=fmap_n-1; i>=0; i--) {
            buffer_handle_t h=find_gralloc(fmap[i].gem);
            if (h) { copy_to_dumb(h); present_hwc2(h); break; }
        }
        ret=0;
    } else if (nr==DRM_IOCTL_NR(DRM_IOCTL_AUTH_MAGIC)) { ret=0;
    } else if (nr==DRM_IOCTL_NR(DRM_IOCTL_MODE_CREATE_DUMB)) {                 /* CREATE_DUMB (KWin QPainter swapchain) */
        if (fake_kms_state()) {
            /* Cached memfd-backed dumb buffer (never scanned out; see above). */
            ret=kdumb_create_memfd((struct drm_mode_create_dumb *)arg);
        } else {
            ret=real_ioctl(fd,request,arg);
            if (ret==0) { struct drm_mode_create_dumb *cd=arg; kdumb_note_create(cd->handle, (size_t)cd->size, cd->pitch); }
        }
    } else if (nr==DRM_IOCTL_NR(DRM_IOCTL_MODE_MAP_DUMB)) {                 /* MAP_DUMB */
        struct drm_mode_map_dumb *md=arg;
        if (fake_kms_state() && KDUMB_IS_FAKE(md->handle) && kdumb_slot_by_gem(md->handle) >= 0) {
            md->offset = KDUMB_FAKE_OFF(kdumb_slot_by_gem(md->handle));
            ret=0;
        } else {
            ret=real_ioctl(fd,request,arg);
            if (ret==0) kdumb_note_map(fd, md->handle, md->offset);
        }
    } else if (nr==DRM_IOCTL_NR(DRM_IOCTL_MODE_DESTROY_DUMB) && fake_kms_state() && KDUMB_IS_FAKE(((struct drm_mode_destroy_dumb *)arg)->handle)) {
        kdumb_destroy_memfd(((struct drm_mode_destroy_dumb *)arg)->handle);   /* DESTROY_DUMB */
        ret=0;
    } else if (fake_kms_state() && (nr==DRM_IOCTL_NR(DRM_IOCTL_MODE_GETGAMMA)||nr==DRM_IOCTL_NR(DRM_IOCTL_MODE_SETGAMMA)||nr==DRM_IOCTL_NR(DRM_IOCTL_MODE_SETPROPERTY)||nr==DRM_IOCTL_NR(DRM_IOCTL_MODE_CURSOR)||nr==DRM_IOCTL_NR(DRM_IOCTL_MODE_CURSOR2))) {
        /* Master-gated legacy state ioctls KWin's legacy path issues:
         * GETGAMMA/SETGAMMA (0xa4/0xa5), connector SETPROPERTY (0xab),
         * CURSOR/CURSOR2 (0xa3/0xbb). The Android composer owns master, so
         * the real calls return EACCES and KWin treats that as a fatal
         * config error. Try the real ioctl (harmless if it works), then
         * pretend success -- gamma/cursor/props have no effect on the
         * HWC2-presented output anyway (kwin renders sw cursors when the
         * cursor plane is unusable).
         *
         * KWin-only (LIBDRM_HYBRIS_FAKE_KMS_STATE): phoc/wlroots issues some of
         * these (SETPROPERTY/CURSOR) during atomic output bring-up and must see
         * the real result, or the DRM output never comes up and phosh goes
         * black. Gated so only the legacy-KMS KWin session opts in. */
        ret=real_ioctl(fd,request,arg);
        if (ret!=0) ret=0;
    } else { ret=real_ioctl(fd,request,arg); }
    in_hook=0; return ret;
}
