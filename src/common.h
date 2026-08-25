/*
 * common.h -- shared state, logging and helpers for libdrm-hybris.
 *
 * libdrm-hybris is an LD_PRELOAD shim that lets a Linux compositor drive an
 * Android graphics stack through libhybris. libhybris is the abstraction: the
 * GPU underneath may be Mali, Adreno, PowerVR or anything else with an Android
 * driver, and nothing in this library should assume which.
 *
 * The shim is listed in /etc/ld.so.preload, so it loads into EVERY process on
 * the system. Two consequences run through the whole codebase:
 *
 *   - Keep the dependency footprint minimal. In particular there is no glib
 *     here, which is why LOG() below is hand-rolled rather than wrapping
 *     g_debug() the way gbm_hybris and drmadapter do -- those are loaded only
 *     into graphics clients, this one is loaded into /bin/true.
 *   - Interception must be inert for processes that are not the compositor.
 *     See hybris_is_compositor().
 */

#ifndef LIBDRM_HYBRIS_COMMON_H
#define LIBDRM_HYBRIS_COMMON_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <hybris/gralloc/gralloc.h>

/* This shim is in /etc/ld.so.preload, so every symbol it exports lands in the
 * global namespace of every process on the system. Only the interposed entry
 * points (ioctl, open, drmMode*, ...) and the drm_shim_* API that drmadapter
 * dlsym()s may be visible; everything internal is hidden. */
#define HYBRIS_INTERNAL __attribute__((visibility ("hidden")))

/* ---- logging ------------------------------------------------------------
 * Same call-site shape as gbm_hybris/drmadapter's LOG(), minus glib. Output is
 * off unless the matching environment variable is set, so a preloaded shim
 * stays silent in normal operation. */

HYBRIS_INTERNAL void hybris_logv (const char *fmt, va_list ap);
HYBRIS_INTERNAL void hybris_log (const char *fmt, ...) __attribute__((format (printf, 1, 2)));

#define LOG(fmt, ...)   hybris_log ("libdrm-hybris: " fmt, ##__VA_ARGS__)

/* Verbose paths, each behind its own flag so a debugging session can enable
 * one without drowning in the others. */
#define LOG_IF(flag, fmt, ...) \
    do { if (hybris_debug.flag) LOG (fmt, ##__VA_ARGS__); } while (0)

/* Environment-controlled behaviour, read once at load instead of calling
 * getenv() on hot paths (the previous code did that 54 times). */
struct hybris_debug_flags {
    bool trace;         /* LIBDRM_HYBRIS_TRACE  -- ioctl tracing to a file  */
    bool sample;        /* LIBDRM_HYBRIS_SAMPLE -- framebuffer bookkeeping  */
    bool profile;       /* LIBDRM_HYBRIS_PROF   -- frame timing             */
    bool log_enabled;   /* any of the above, or LIBDRM_HYBRIS_DEBUG         */
};

HYBRIS_INTERNAL extern struct hybris_debug_flags hybris_debug;

HYBRIS_INTERNAL void hybris_common_init (void);

/* ---- process role -------------------------------------------------------
 * Only a compositor may drive the faked KMS state; every other process must
 * reach the real driver untouched. */

HYBRIS_INTERNAL bool hybris_is_compositor (void);
HYBRIS_INTERNAL bool hybris_is_gnome (void);

/* ---- shared state -------------------------------------------------------
 * Previously ~60 loose file-static variables. Grouped by lifetime and owner so
 * it is clear what belongs together and what each module may touch. */

#define HYBRIS_MAX_BUFFERS 256

/* gralloc handle <-> DRM identity. Two directions are needed: PRIME fd -> the
 * gralloc buffer it came from, and GEM handle -> the framebuffer id we faked
 * for it. */
struct hybris_buffer_registry {
    struct {
        uint32_t        prime_fd;
        buffer_handle_t gralloc;
    } by_prime[HYBRIS_MAX_BUFFERS];
    int n_by_prime;

    struct {
        uint32_t gem;
        uint32_t fb_id;
    } by_gem[HYBRIS_MAX_BUFFERS];
    int n_by_gem;

    /* Framebuffer ids we invent. Kept well above any real id. */
    uint32_t next_fake_fb_id;
};

/* The single dumb buffer the faked KMS state scans out of. */
struct hybris_dumb_buffer {
    uint32_t handle;
    uint32_t fb_id;
    uint32_t pitch;
    void    *map;
    size_t   size;
};

/* Panel geometry, learned from the first framebuffer or from HWC2. */
struct hybris_frame_geometry {
    uint32_t width;
    uint32_t height;
};

HYBRIS_INTERNAL extern struct hybris_buffer_registry hybris_buffers;
HYBRIS_INTERNAL extern struct hybris_dumb_buffer     hybris_dumb;
HYBRIS_INTERNAL extern struct hybris_frame_geometry  hybris_frame;

/* ---- symbol resolution --------------------------------------------------
 * dlsym(RTLD_NEXT) is not enough on its own: this shim is installed both as
 * libdrm-hybris.so and as libseat.so.1, so RTLD_NEXT can resolve back into
 * another copy of itself and recurse forever. resolve_next() detects that. */

HYBRIS_INTERNAL void *hybris_resolve_next (const char *name, void *self_addr);

#endif /* LIBDRM_HYBRIS_COMMON_H */
