/*
 * common.c -- logging, process-role detection, shared state and symbol
 * resolution for libdrm-hybris. See common.h for why this has no glib.
 */

#define _GNU_SOURCE

#include "common.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

HYBRIS_INTERNAL struct hybris_debug_flags   hybris_debug;
HYBRIS_INTERNAL struct hybris_buffer_registry hybris_buffers = { .next_fake_fb_id = 0x80000000u };
HYBRIS_INTERNAL struct hybris_dumb_buffer     hybris_dumb;
HYBRIS_INTERNAL struct hybris_frame_geometry  hybris_frame;
HYBRIS_INTERNAL struct hybris_tuning hybris_tuning = {
    .touch_sync = true,
    .row_step   = 64,
};

/* ---- logging ------------------------------------------------------------ */

HYBRIS_INTERNAL void
hybris_logv (const char *fmt, va_list ap)
{
    if (!hybris_debug.log_enabled)
        return;

    vfprintf (stderr, fmt, ap);
    fputc ('\n', stderr);
}

HYBRIS_INTERNAL void
hybris_log (const char *fmt, ...)
{
    va_list ap;

    if (!hybris_debug.log_enabled)
        return;

    va_start (ap, fmt);
    hybris_logv (fmt, ap);
    va_end (ap);
}

/* ---- init --------------------------------------------------------------- */

HYBRIS_INTERNAL void
hybris_common_init (void)
{
    static bool done;

    if (done)
        return;
    done = true;

    hybris_debug.trace   = getenv ("LIBDRM_HYBRIS_TRACE") != NULL;
    hybris_debug.sample  = getenv ("LIBDRM_HYBRIS_SAMPLE") != NULL;
    hybris_debug.profile = getenv ("LIBDRM_HYBRIS_PROF") != NULL;
    /* Overrides only. Both default to the fast, measured path above. */
    const char *sync = getenv ("LIBDRM_HYBRIS_SYNC");
    const char *step = getenv ("LIBDRM_HYBRIS_ROWSTEP");

    if (sync && strcmp (sync, "copy") == 0)
        hybris_tuning.touch_sync = false;
    if (step) {
        int n = atoi (step);

        if (n >= 1)
            hybris_tuning.row_step = n;
    }

    hybris_debug.log_enabled = hybris_debug.trace ||
                               hybris_debug.sample ||
                               hybris_debug.profile ||
                               getenv ("LIBDRM_HYBRIS_DEBUG") != NULL;
}

__attribute__((constructor))
static void
common_ctor (void)
{
    hybris_common_init ();
}

/* ---- process role -------------------------------------------------------
 * Detected from the executable name. Everything the shim fakes is only correct
 * inside a compositor; a camera app or a browser opening the same device must
 * see the real driver. */

static const char *const compositor_names[] = {
    "gnome-shell",
    "mutter",
    "phoc",
    "weston",
    "wlroots",
    "sway",
    NULL,
};

static const char *
process_basename (void)
{
    static char exe[256];
    static bool resolved;
    const char *base;
    ssize_t n;

    if (!resolved) {
        resolved = true;
        n = readlink ("/proc/self/exe", exe, sizeof (exe) - 1);
        exe[n > 0 ? n : 0] = '\0';
    }

    base = strrchr (exe, '/');
    return base ? base + 1 : exe;
}

HYBRIS_INTERNAL bool
hybris_is_compositor (void)
{
    static int cached = -1;
    const char *base;

    if (cached >= 0)
        return cached;

    base = process_basename ();
    cached = 0;

    for (int i = 0; compositor_names[i]; i++) {
        if (strcmp (base, compositor_names[i]) == 0) {
            cached = 1;
            break;
        }
    }

    /* KWin ships under several names (kwin_wayland, kwin_wayland_wrapper). */
    if (!cached && strstr (base, "kwin"))
        cached = 1;

    return cached;
}

HYBRIS_INTERNAL bool
hybris_is_kwin (void)
{
    static int cached = -1;

    if (cached < 0)
        cached = strstr (process_basename (), "kwin") != NULL;
    return cached;
}

HYBRIS_INTERNAL bool
hybris_is_wlroots (void)
{
    static int cached = -1;
    const char *base;

    if (cached < 0) {
        base = process_basename ();
        cached = (strcmp (base, "phoc") == 0 || strcmp (base, "wlroots") == 0);
    }
    return cached;
}

HYBRIS_INTERNAL bool
hybris_is_gnome (void)
{
    static int cached = -1;
    const char *base;

    if (cached >= 0)
        return cached;

    base = process_basename ();
    cached = (strcmp (base, "gnome-shell") == 0 || strcmp (base, "mutter") == 0);
    return cached;
}

/* ---- symbol resolution -------------------------------------------------- */

/* Exported so a duplicate copy of this shim can be recognised; see below. */
int libdrm_hybris_shim_marker = 1;

HYBRIS_INTERNAL void *
hybris_resolve_next (const char *name, void *self_addr)
{
    Dl_info info;
    void *fn;

    fn = dlsym (RTLD_NEXT, name);
    if (!fn || fn == self_addr)
        return NULL;

    /* This shim is installed both as libdrm-hybris.so and as libseat.so.1.
     * RTLD_NEXT can therefore resolve to the *other* copy of the same
     * function, which would call straight back into us and recurse until the
     * stack runs out. A marker symbol identifies our own builds. */
    if (dladdr (fn, &info) && info.dli_fname) {
        void *handle = dlopen (info.dli_fname, RTLD_NOW | RTLD_NOLOAD);

        if (handle) {
            bool is_duplicate = dlsym (handle, "libdrm_hybris_shim_marker") != NULL;

            dlclose (handle);
            if (is_duplicate)
                return NULL;
        }
    }

    return fn;
}
