/*
 * eglplatform_drmadapter.c - hybris EGL platform for GNOME Shell on Mali/HWC2
 *
 * Set HYBRIS_EGLPLATFORM=drmadapter to use this platform.
 * Handles HWC2 init and present. Exposes hwc2 state for GLVND vendor wrapper.
 */

#define _GNU_SOURCE
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <hybris/eglplatformcommon/ws.h>
#include <hybris/eglplatformcommon/eglplatformcommon.h>
#include <hybris/gralloc/gralloc.h>
#include <hardware/gralloc.h>
#include <hybris/hwc2/hwc2_compatibility_layer.h>
#include <hybris/hwcomposerwindow/hwcomposer.h>

#define LOG(fmt, ...) g_debug("drmadapter: " fmt, ##__VA_ARGS__)
#define FRAME_W 1080
#define FRAME_H 2412

static hwc2_compat_device_t  *hwc2_dev   = NULL;
static hwc2_compat_display_t *hwc2_disp  = NULL;
static hwc2_compat_layer_t   *hwc2_layer = NULL;
static int hwc2_ready = 0;
static pthread_mutex_t hwc2_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Exported getters for GLVND vendor wrapper */
hwc2_compat_display_t *drmadapter_get_hwc2_display(void) { return hwc2_disp; }
hwc2_compat_layer_t   *drmadapter_get_hwc2_layer(void)   { return hwc2_layer; }

static void on_hotplug(HWC2EventListener *s,int32_t q,hwc2_display_t d,bool c,bool p){}
static void on_vsync(HWC2EventListener *s,int32_t q,hwc2_display_t d,int64_t t){}
static void on_refresh(HWC2EventListener *s,int32_t q,hwc2_display_t d){}

static void init_hwc2(void) {
    pthread_mutex_lock(&hwc2_mutex);
    if (hwc2_ready) { pthread_mutex_unlock(&hwc2_mutex); return; }
    LOG("init_hwc2: starting");
    hybris_gralloc_initialize(0);
    hwc2_dev = hwc2_compat_device_new(false);
    if (!hwc2_dev) { LOG("device_new failed"); goto out; }
    HWC2EventListener *l = calloc(1, sizeof(*l));
    l->on_vsync_received   = on_vsync;
    l->on_hotplug_received = on_hotplug;
    l->on_refresh_received = on_refresh;
    hwc2_compat_device_register_callback(hwc2_dev, l, 0);
    hwc2_compat_device_on_hotplug(hwc2_dev, 0, true);
    hwc2_disp = hwc2_compat_device_get_display_by_id(hwc2_dev, 0);
    if (!hwc2_disp) { LOG("no display"); goto out; }
    hwc2_compat_display_set_power_mode(hwc2_disp, 2);
    hwc2_compat_display_set_vsync_enabled(hwc2_disp, 1);
    hwc2_layer = hwc2_compat_display_create_layer(hwc2_disp);
    hwc2_compat_layer_set_composition_type(hwc2_layer, 4);
    hwc2_compat_layer_set_blend_mode(hwc2_layer, 1);
    hwc2_compat_layer_set_source_crop(hwc2_layer, 0, 0, FRAME_W, FRAME_H);
    hwc2_compat_layer_set_display_frame(hwc2_layer, 0, 0, FRAME_W, FRAME_H);
    hwc2_compat_layer_set_visible_region(hwc2_layer, 0, 0, FRAME_W, FRAME_H);
    hwc2_ready = 1;
    LOG("init_hwc2: done disp=%p layer=%p", (void*)hwc2_disp, (void*)hwc2_layer);
out:
    pthread_mutex_unlock(&hwc2_mutex);
}

static void drmadapterws_init_module(struct ws_egl_interface *egl_iface) {
    LOG("init_module");
    eglplatformcommon_init(egl_iface);
    setenv("EGL_PLATFORM", "hwcomposer", 0);
    pthread_t t;
    pthread_create(&t, NULL, (void*(*)(void*))init_hwc2, NULL);
    pthread_detach(t);
}

static struct _EGLDisplay *drmadapterws_GetDisplay(EGLNativeDisplayType native) {
    LOG("GetDisplay native=%p", (void*)native);
    struct _EGLDisplay *dpy = calloc(1, sizeof(*dpy));
    dpy->display_id = EGL_DEFAULT_DISPLAY;
    dpy->dpy = EGL_NO_DISPLAY;
    return dpy;
}

static void drmadapterws_releaseDisplay(struct _EGLDisplay *dpy) {
    LOG("releaseDisplay");
    free(dpy);
}

static void drmadapterws_Terminate(struct _EGLDisplay *dpy) {
    LOG("Terminate");
}

static void drmadapterws_eglInitialized(struct _EGLDisplay *dpy) {
    LOG("eglInitialized dpy=%p", (void*)dpy);
}

static struct ANativeWindow *hwc_native_win = NULL;

static void present_cb(void *ud, struct ANativeWindow *w, struct ANativeWindowBuffer *buf) {
    if (!hwc2_ready || !hwc2_disp || !hwc2_layer || !buf) return;
    uint32_t nt = 0, nr = 0;
    hwc2_compat_display_validate(hwc2_disp, &nt, &nr);
    if (nt) hwc2_compat_display_accept_changes(hwc2_disp);
    hwc2_compat_layer_set_buffer(hwc2_layer, 0, buf, -1);
    hwc2_compat_display_set_client_target(hwc2_disp, 0, buf, -1, 0);
    int32_t fence = -1;
    hwc2_compat_display_present(hwc2_disp, &fence);
    HWCNativeBufferSetFence(buf, -1);
    if (fence >= 0) close(fence);
}

static EGLNativeWindowType drmadapterws_CreateWindow(EGLNativeWindowType win,
                                                      struct _EGLDisplay *display) {
    LOG("CreateWindow win=%p", (void*)win);
    /* Wait for HWC2 init to complete (may be running in background thread) */
    int retries = 0;
    while (!hwc2_ready && retries < 100) {
        usleep(50000); /* 50ms */
        retries++;
    }
    if (!hwc2_ready) {
        LOG("CreateWindow: HWC2 not ready after 5s, trying direct init");
        init_hwc2();
    }
    if (!hwc_native_win && hwc2_disp) {
        hwc_native_win = HWCNativeWindowCreate(FRAME_W, FRAME_H, HAL_PIXEL_FORMAT_RGBA_8888,
                                               present_cb, NULL);
        LOG("CreateWindow: created hwc_native_win=%p", (void*)hwc_native_win);
    }
    return (EGLNativeWindowType)hwc_native_win;
}

static void drmadapterws_DestroyWindow(EGLNativeWindowType win) {
    LOG("DestroyWindow win=%p", (void*)win);
}

static __eglMustCastToProperFunctionPointerType
drmadapterws_eglGetProcAddress(const char *procname) {
    return eglplatformcommon_eglGetProcAddress(procname);
}

static void drmadapterws_passthroughImageKHR(EGLContext *ctx, EGLenum *target,
                                              EGLClientBuffer *buffer,
                                              const EGLint **attrib_list) {
    eglplatformcommon_passthroughImageKHR(ctx, target, buffer, attrib_list);
}

static const char *drmadapterws_eglQueryString(EGLDisplay dpy, EGLint name,
    const char *(*real_eglQueryString)(EGLDisplay, EGLint)) {
    return eglplatformcommon_eglQueryString(dpy, name, real_eglQueryString);
}

static void drmadapterws_prepareSwap(EGLDisplay dpy, EGLNativeWindowType win,
                                      EGLint *damage_rects, EGLint damage_n_rects) {}

static void drmadapterws_finishSwap(EGLDisplay dpy, EGLNativeWindowType win) {}

static void drmadapterws_setSwapInterval(EGLDisplay dpy, EGLNativeWindowType win,
                                          EGLint interval) {}

struct ws_module ws_module_info = {
    .init_module         = drmadapterws_init_module,
    .GetDisplay          = drmadapterws_GetDisplay,
    .Terminate           = drmadapterws_Terminate,
    .CreateWindow        = drmadapterws_CreateWindow,
    .DestroyWindow       = drmadapterws_DestroyWindow,
    .eglGetProcAddress   = drmadapterws_eglGetProcAddress,
    .passthroughImageKHR = drmadapterws_passthroughImageKHR,
    .eglQueryString      = drmadapterws_eglQueryString,
    .prepareSwap         = drmadapterws_prepareSwap,
    .finishSwap          = drmadapterws_finishSwap,
    .setSwapInterval     = drmadapterws_setSwapInterval,
    .releaseDisplay      = drmadapterws_releaseDisplay,
    .eglInitialized      = drmadapterws_eglInitialized,
};

/* Fix EGL_NATIVE_VISUAL_ID to return GBM-compatible pixel formats */
static void drmadapterws_getConfigAttrib(EGLDisplay *dpy, EGLConfig *config,
                                          EGLint *attribute, EGLint *value) {
    if (*attribute == EGL_NATIVE_VISUAL_ID && value && *value) {
        EGLint alpha = 0;
        typedef EGLBoolean (*getConfigAttrib_t)(EGLDisplay,EGLConfig,EGLint,EGLint*);
        static getConfigAttrib_t real_fn = NULL;
        if (!real_fn) real_fn = (getConfigAttrib_t)dlsym(RTLD_DEFAULT, "eglGetConfigAttrib");
        if (real_fn) real_fn(*dpy, *config, EGL_ALPHA_SIZE, &alpha);
        switch (*value) {
            case 1: *value = alpha > 0 ? 0x34324241 : 0x34324258; break; /* RGBA/RGBX */
            case 5: *value = alpha > 0 ? 0x34325241 : 0x34325258; break; /* RGBA/RGBX */
            case 4: *value = 0x36314752; break; /* RG16 */
            default: *value = 0x34324258; break; /* XRGB8888 */
        }
    }
}
