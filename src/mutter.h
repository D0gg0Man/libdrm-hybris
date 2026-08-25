/*
 * mutter.h -- GNOME/mutter-specific entry points.
 *
 * wl_display_create() is an interposed symbol and is exported directly, so
 * there is nothing to declare for it. The panel-power watcher starts itself
 * from a constructor.
 */

#ifndef LIBDRM_HYBRIS_MUTTER_H
#define LIBDRM_HYBRIS_MUTTER_H

#include "common.h"

/* Implemented in libdrm-hybris.c: shared with phoc (which calls it directly by
 * dlsym) and with KWin's DPMS path. */
void drm_shim_panel_power (int on);

#endif /* LIBDRM_HYBRIS_MUTTER_H */
