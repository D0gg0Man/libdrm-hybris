#define _GNU_SOURCE
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <wayland-server.h>

#define LOG(fmt, ...) g_debug("wlegl_server: " fmt, ##__VA_ARGS__)

typedef void* (*server_wlegl_create_t)(struct wl_display*);


struct wl_display *wl_display_create(void) {
    typedef struct wl_display* (*fn_t)(void);
    fn_t real = dlsym(RTLD_NEXT, "wl_display_create");
    struct wl_display *dpy = real();
    if (dpy) {
        void *lib = dlopen("libhybris-platformcommon.so", RTLD_NOW|RTLD_NOLOAD);
        if (!lib) lib = dlopen("libhybris-platformcommon.so", RTLD_NOW);
        if (lib) {
            server_wlegl_create_t create = dlsym(lib, "_Z19server_wlegl_createP10wl_display");
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
