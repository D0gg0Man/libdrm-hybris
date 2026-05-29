#define _GNU_SOURCE

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <stdarg.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <wayland-server.h>

#include <hybris/gralloc/gralloc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define LOG(fmt, ...) g_debug("drm_shim: " fmt, ##__VA_ARGS__)

#define GMAP_MAX 64
#define FRAME_W  1080
#define FRAME_H  2412

/* From EGL wrapper */
extern void gnome_mali_hwc2_present_gralloc(void *handle,
                                            int w,
                                            int h,
                                            int stride) __attribute__((weak));

typedef void *(*server_wlegl_create_t)(struct wl_display *);

static struct {
    uint32_t prime_fd;
    buffer_handle_t gralloc;
} gmap[GMAP_MAX];

static int gmap_n = 0;

static struct {
    uint32_t gem;
    uint32_t fb_id;
} fmap[GMAP_MAX];

static int fmap_n = 0;

static uint32_t dumb_handle = 0;
static uint32_t dumb_fb_id = 0;
static uint32_t dumb_pitch = 0;
static void *dumb_map = NULL;
static size_t dumb_size = 0;
static uint32_t next_fake = 500;
static int in_hook = 0;
static int crtc_set = 0;
static int saved_drm_fd = -1;
static uint32_t saved_crtc_id = 0;
static uint32_t saved_conn_id = 0;
static struct drm_mode_modeinfo saved_mode;

/* Forward declare hybris init */
static EGLDisplay our_egl_display = EGL_NO_DISPLAY;

typedef EGLDisplay (*eglGetPlatformDisplayEXT_t)(EGLenum,
                                                 void *,
                                                 const EGLint *);

typedef EGLDisplay (*eglGetPlatformDisplay_t)(EGLenum,
                                              void *,
                                              const EGLAttrib *);

typedef int (*ioctl_t)(int, unsigned long, ...);

static ioctl_t real_ioctl = NULL;

__attribute__((constructor))
static void
setup_egl_vendor(void)
{
    const char *desktop = getenv("XDG_SESSION_DESKTOP");

    if (desktop && strcmp(desktop, "gnome") == 0)
        g_debug("drm_shim: set EGL vendor for gnome session");
}

static int
is_gnome_shell(void)
{
    char buf[256] = {0};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);

    if (n < 0)
        return 0;

    buf[n] = '\0';

    return strstr(buf, "gnome-shell") != NULL;
}

struct wl_display *
wl_display_create(void)
{
    typedef struct wl_display *(*fn_t)(void);

    fn_t real = dlsym(RTLD_NEXT, "wl_display_create");
    struct wl_display *dpy = real();

    if (dpy && is_gnome_shell()) {
        void *lib = dlopen("libhybris-platformcommon.so", RTLD_NOW | RTLD_NOLOAD);

        if (!lib)
            lib = dlopen("libhybris-platformcommon.so", RTLD_NOW);

        if (lib) {
            server_wlegl_create_t create =
                dlsym(lib, "_Z19server_wlegl_createP10wl_display");

            if (create) {
                void *wlegl = create(dpy);

                LOG("android_wlegl injected into display: %p", wlegl);
            } else {
                LOG("server_wlegl_create not found in libhybris-platformcommon");
            }
        } else {
            LOG("libhybris-platformcommon not found: %s", dlerror());
        }
    }

    return dpy;
}

static void
init_our_display(void)
{
    if (our_egl_display != EGL_NO_DISPLAY)
        return;

    /* Get hybris display directly */
    void *libhybris = dlopen("libEGL_hybris_wrapper.so", RTLD_NOW | RTLD_NOLOAD);

    if (!libhybris)
        libhybris = dlopen("libEGL_hybris_wrapper.so", RTLD_NOW);

    if (libhybris) {
        typedef EGLDisplay (*fn_t)(EGLenum, void *, const EGLAttrib *);

        fn_t fn = (fn_t)dlsym(libhybris, "wrapped_getPlatformDisplay");

        if (fn) {
            our_egl_display = fn(0x31D7, (void *)1, NULL);

            g_debug("drm_shim: intercepted EGL display = %p", (void *)our_egl_display);
        }
    }
}

EGLDisplay
eglGetPlatformDisplayEXT(EGLenum platform, void *native, const EGLint *attribs)
{
    const char *desktop = getenv("XDG_SESSION_DESKTOP");

    if (desktop && strcmp(desktop, "gnome") == 0) {
        g_debug("drm_shim: eglGetPlatformDisplayEXT intercepted platform=0x%x", platform);

        init_our_display();

        if (our_egl_display != EGL_NO_DISPLAY)
            return our_egl_display;
    }

    eglGetPlatformDisplayEXT_t real = (eglGetPlatformDisplayEXT_t)dlsym(RTLD_NEXT, "eglGetPlatformDisplayEXT");

    return real ? real(platform, native, attribs) : EGL_NO_DISPLAY;
}

EGLDisplay
eglGetPlatformDisplay(EGLenum platform, void *native, const EGLAttrib *attribs)
{
    const char *desktop = getenv("XDG_SESSION_DESKTOP");

    if (desktop && strcmp(desktop, "gnome") == 0) {
        g_debug("drm_shim: eglGetPlatformDisplay intercepted platform=0x%x", platform);

        init_our_display();

        if (our_egl_display != EGL_NO_DISPLAY)
            return our_egl_display;
    }

    eglGetPlatformDisplay_t real = (eglGetPlatformDisplay_t)dlsym(RTLD_NEXT, "eglGetPlatformDisplay");

    return real ? real(platform, native, attribs) : EGL_NO_DISPLAY;
}

static void
ensure_real(void)
{
    if (!real_ioctl)
        real_ioctl = dlsym(RTLD_NEXT, "ioctl");
}

void
drm_shim_register_bo(uint32_t prime_fd, buffer_handle_t gralloc)
{
    for (int i = 0; i < gmap_n; i++) {
        if (gmap[i].prime_fd == prime_fd) {
            gmap[i].gralloc = gralloc;
            return;
        }
    }

    if (gmap_n < GMAP_MAX) {
        gmap[gmap_n].prime_fd = prime_fd;
        gmap[gmap_n++].gralloc = gralloc;
    }
}

static buffer_handle_t
find_gralloc(uint32_t gem)
{
    for (int i = 0; i < gmap_n; i++) {
        if (gmap[i].prime_fd == gem)
            return gmap[i].gralloc;
    }

    return NULL;
}

static buffer_handle_t
find_by_fb(uint32_t fb_id)
{
    for (int i = 0; i < fmap_n; i++) {
        if (fmap[i].fb_id == fb_id)
            return find_gralloc(fmap[i].gem);
    }

    return NULL;
}

static void
copy_to_dumb(buffer_handle_t h)
{
    if (!dumb_map || !h)
        return;

    void *src = NULL;

    if (hybris_gralloc_lock(h, 0x3 | 0x30, 0, 0, 1080, 2412, &src) || !src)
        return;

    uint8_t *d = dumb_map;
    uint8_t *s = src;

    for (int y = 0; y < 2412; y++)
        memcpy(d + y * dumb_pitch, s + y * 4352, 1080 * 4);

    hybris_gralloc_unlock(h);
}

static int
init_dumb(int fd)
{
    if (dumb_map)
        return 0;

    int s = in_hook;

    in_hook = 1;

    struct drm_mode_create_dumb cd = {
        .height = 2412,
        .width = 1080,
        .bpp = 32,
    };

    if (real_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) {
        in_hook = s;
        return -1;
    }

    dumb_handle = cd.handle;
    dumb_pitch = cd.pitch;
    dumb_size = cd.size;

    LOG("dumb handle=%u pitch=%u size=%zu", dumb_handle, dumb_pitch, dumb_size);

    struct drm_mode_fb_cmd fb = {
        .width = 1080,
        .height = 2412,
        .pitch = dumb_pitch,
        .bpp = 32,
        .depth = 24,
        .handle = dumb_handle,
    };

    if (real_ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb) == 0) {
        dumb_fb_id = fb.fb_id;
        LOG("ADDFB ok fb_id=%u", dumb_fb_id);
    } else {
        struct drm_mode_fb_cmd2 fb2 = {
            .width = 1080,
            .height = 2412,
            .pixel_format = 0x34325258,
        };

        fb2.handles[0] = dumb_handle;
        fb2.pitches[0] = dumb_pitch;

        if (real_ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb2)) {
            in_hook = s;
            return -1;
        }

        dumb_fb_id = fb2.fb_id;
        LOG("ADDFB2 ok fb_id=%u", dumb_fb_id);
    }

    struct drm_mode_map_dumb md = {
        .handle = dumb_handle,
    };

    if (real_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) {
        in_hook = s;
        return -1;
    }

    dumb_map = mmap(NULL,
                    dumb_size,
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED,
                    fd,
                    md.offset);

    if (dumb_map == MAP_FAILED) {
        dumb_map = NULL;
        in_hook = s;
        return -1;
    }

    LOG("dumb mapped at %p", dumb_map);

    drmModeRes *res = drmModeGetResources(fd);

    if (res && res->count_crtcs > 0 && res->count_connectors > 0) {
        saved_drm_fd = fd;
        saved_crtc_id = res->crtcs[0];
        saved_conn_id = res->connectors[0];

        drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[0]);

        if (conn && conn->count_modes > 0) {
            saved_mode = *(struct drm_mode_modeinfo *)&conn->modes[0];

            LOG("saved mode from connector: %dx%d@%dHz clock=%d",
                saved_mode.hdisplay,
                saved_mode.vdisplay,
                saved_mode.vrefresh,
                saved_mode.clock);

            /* Override refresh rate to 120Hz */
            if (saved_mode.vrefresh > 120) {
                saved_mode.vrefresh = 120;

                /* Recalculate clock: clock_kHz = htotal * vtotal * vrefresh / 1000 */
                saved_mode.clock = (uint32_t)((uint64_t)saved_mode.htotal * saved_mode.vtotal * 120 / 1000);

                LOG("overrode refresh to 120Hz clock=%d", saved_mode.clock);
            }
        }

        if (conn)
            drmModeFreeConnector(conn);

        drmModeFreeResources(res);
    }

    in_hook = s;

    return 0;
}

int
drmModeAddFB2WithModifiers(int fd,
                           uint32_t w,
                           uint32_t h,
                           uint32_t fmt,
                           const uint32_t handles[4],
                           const uint32_t pitches[4],
                           const uint32_t offsets[4],
                           const uint64_t mod[4],
                           uint32_t *buf_id,
                           uint32_t flags)
{
    if (!dumb_map)
        init_dumb(fd);

    uint32_t id = next_fake++;

    *buf_id = id;

    buffer_handle_t g = find_gralloc(handles[0]);

    LOG("AddFB2WithMod gem=%u -> fb=%u gralloc=%p", handles[0], id, (void *)g);

    if (fmap_n < GMAP_MAX) {
        fmap[fmap_n].gem = handles[0];
        fmap[fmap_n++].fb_id = id;
    }

    return 0;
}

int
drmModeAddFB2(int fd,
              uint32_t w,
              uint32_t h,
              uint32_t fmt,
              const uint32_t handles[4],
              const uint32_t pitches[4],
              const uint32_t offsets[4],
              uint32_t *buf_id,
              uint32_t flags)
{
    if (!dumb_map)
        init_dumb(fd);

    uint32_t id = next_fake++;

    *buf_id = id;

    if (fmap_n < GMAP_MAX) {
        fmap[fmap_n].gem = handles[0];
        fmap[fmap_n++].fb_id = id;
    }

    return 0;
}

int
drmModeRmFB(int fd, uint32_t id)
{
    return 0;
}

int
drmModeSetCrtc(int fd,
               uint32_t crtcId,
               uint32_t bufferId,
               uint32_t x,
               uint32_t y,
               uint32_t *connectors,
               int count,
               drmModeModeInfoPtr mode)
{
    LOG("drmModeSetCrtc called -> returning success");

    if (!dumb_map)
        init_dumb(fd);

    crtc_set = 1;

    return 0;
}

int
drmModePageFlip(int fd,
                uint32_t crtc_id,
                uint32_t fb_id,
                uint32_t flags,
                void *ud)
{
    buffer_handle_t h = find_by_fb(fb_id);

    copy_to_dumb(h);

    typedef int (*fn_t)(int, uint32_t, uint32_t, uint32_t, void *);

    fn_t real = dlsym(RTLD_NEXT, "drmModePageFlip");

    return real ? real(fd, crtc_id, dumb_fb_id ? dumb_fb_id : fb_id, flags, ud) : 0;
}

int
drmModeAtomicCommit(int fd, drmModeAtomicReqPtr req, uint32_t flags, void *ud)
{
    for (int i = fmap_n - 1; i >= 0; i--) {
        buffer_handle_t h = find_gralloc(fmap[i].gem);

        if (h) {
            copy_to_dumb(h);
            break;
        }
    }

    return 0;
}

int
ioctl(int fd, unsigned long request, ...)
{
    ensure_real();

    va_list args;

    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);

    if (in_hook)
        return real_ioctl(fd, request, arg);

    uint32_t nr = request & 0xff;
    uint32_t magic = (request >> 8) & 0xff;

    if (magic != 0x64)
        return real_ioctl(fd, request, arg);

    in_hook = 1;

    int ret;

    if (nr == 0xb8 || nr == 0xb9) {
        if (!dumb_map)
            init_dumb(fd);

        uint32_t *fb = arg;
        uint32_t gem = fb[5];
        uint32_t id = next_fake++;

        fb[6] = id;

        if (fmap_n < GMAP_MAX) {
            fmap[fmap_n].gem = gem;
            fmap[fmap_n++].fb_id = id;
        }

        ret = 0;
    } else if (nr == 0xaf) {
        ret = 0;
    } else if (nr == 0xa2) {
        /* SETCRTC - always succeed */
        if (!dumb_map)
            init_dumb(fd);

        real_ioctl(fd, request, arg);
        crtc_set = 1;
        ret = 0;
    } else if (nr == 0xb0 || nr == 0xb6) {
        /* PAGE_FLIP or GETPLANE - intercept page flips */
        struct drm_mode_crtc_page_flip *flip = arg;
        buffer_handle_t h = find_by_fb(flip->fb_id);

        copy_to_dumb(h);

        if (dumb_fb_id)
            flip->fb_id = dumb_fb_id;

        ret = real_ioctl(fd, request, arg);

        if (ret != 0)
            crtc_set = 0;
    } else if (nr == 0xaa || nr == 0xbc) {
        for (int i = fmap_n - 1; i >= 0; i--) {
            buffer_handle_t h = find_gralloc(fmap[i].gem);

            if (h) {
                copy_to_dumb(h);
                break;
            }
        }

        ret = 0;
    } else {
        ret = real_ioctl(fd, request, arg);
    }

    in_hook = 0;

    return ret;
}

typedef void *EGLNativeDisplayType2;
typedef void *(*eglGetDisplay_t)(EGLNativeDisplayType2);

EGLDisplay
eglGetDisplay(EGLNativeDisplayType2 native)
{
    const char *desktop = getenv("XDG_SESSION_DESKTOP");

    g_debug("drm_shim: eglGetDisplay native=%p desktop=%s", native, desktop ? desktop : "null");

    fflush(stderr);

    eglGetDisplay_t real = (eglGetDisplay_t)dlsym(RTLD_NEXT, "eglGetDisplay");

    return real ? real(native) : (void *)0;
}
