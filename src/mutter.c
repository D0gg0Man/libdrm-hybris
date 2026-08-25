/*
 * mutter.c -- the GNOME/mutter-specific paths.
 *
 * Two things mutter needs that no other compositor does:
 *
 *   android_wlegl injection. gnome-shell's Wayland server does not offer the
 *   android_wlegl protocol, which hybris clients need in order to share gralloc
 *   buffers. Injecting it at wl_display_create() time is the only hook that
 *   runs before any client connects.
 *
 *   Panel power. mutter blanks by calling meta_kms_device_disable(), whose DRM
 *   traffic this shim swallows, and with MUTTER_DEBUG_FORCE_KMS_MODE=simple it
 *   emits no atomic ACTIVE commit, no DPMS property set and no legacy CRTC
 *   disable -- all three were checked on device and none fire. So the signal
 *   comes from the one unambiguous place, gnome-shell's powerManager, through a
 *   small control file. The HWC2 side of it lives with the shared
 *   drm_shim_panel_power() API in libdrm-hybris.c; only the watcher is here.
 */

#define _GNU_SOURCE

#include "common.h"
#include "mutter.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <wayland-server.h>

/* BUG 4 fix: only inject wlegl into gnome-shell itself, not every
 * wayland server that happens to run in a gnome session */
static int is_gnome_shell(void) {
    char buf[256] = {0};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0) return 0;
    buf[n] = '\0';
    return strstr(buf, "gnome-shell") != NULL;
}

/* ---- android_wlegl injection ---------------------------------------------
 * Injected as the display is created, before any client can connect. */

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

