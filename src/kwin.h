/*
 * kwin.h -- entry points libdrm-hybris.c uses for the KWin paths.
 * All of these are no-ops or unreachable unless hybris_is_kwin().
 */

#ifndef LIBDRM_HYBRIS_KWIN_H
#define LIBDRM_HYBRIS_KWIN_H

#include "common.h"

#include <drm.h>
#include <stdint.h>

/* Fake dumb-buffer swapchain: KWin's legacy KMS path cannot create real ones
 * because the Android composer holds DRM master. */
HYBRIS_INTERNAL int  kwin_dumb_create_memfd (struct drm_mode_create_dumb *create);
HYBRIS_INTERNAL void kwin_dumb_destroy_memfd (uint32_t gem);
HYBRIS_INTERNAL void kwin_dumb_note_create (uint32_t gem, size_t size, uint32_t pitch);
HYBRIS_INTERNAL void kwin_dumb_note_map (int fd, uint32_t gem, uint64_t offset);
HYBRIS_INTERNAL int  kwin_dumb_slot_by_gem (uint32_t gem);

/* Copy a software-composited dumb buffer out through the HWC2 bridge. */
HYBRIS_INTERNAL int  kwin_present_qpainter_dumb (uint32_t gem);

/* PRIME export of a fake dumb buffer, and the fd -> dumb-gem mapping that
 * makes the exported fd resolvable back to its CPU mapping. */
HYBRIS_INTERNAL int  kwin_dumb_export_memfd (uint32_t handle, int *prime_fd);
HYBRIS_INTERNAL void kwin_primemap_add (int fd, uint32_t dumb_gem);
HYBRIS_INTERNAL uint32_t kwin_primemap_dumb (uint32_t fd);

/* Fake GEM handles and mmap offsets carry a magic tag so they are recognisable
 * when they come back through MAP_DUMB / DESTROY_DUMB / mmap. */
#define KWIN_DUMB_FAKE_GEM(i)  (0x4B440000u | (uint32_t)(i))
#define KWIN_DUMB_IS_FAKE(h)   (((h) & 0xFFFF0000u) == 0x4B440000u)
#define KWIN_DUMB_FAKE_OFF(i)  ((0x4B44ull << 40) | ((uint64_t)(i) << 20))
#define KWIN_DUMB_OFF_MAGIC(o) (((uint64_t)(o) >> 40) == 0x4B44ull)
#define KWIN_DUMB_OFF_IDX(o)   ((int)(((uint64_t)(o) >> 20) & 0xFFFFFu))

#endif /* LIBDRM_HYBRIS_KWIN_H */
