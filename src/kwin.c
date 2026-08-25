/*
 * kwin.c -- the KWin-specific paths.
 *
 * KWin's QPainter DRM backend composites in software into DRM dumb buffers and
 * page-flips them. Those are not gralloc buffers, so they cannot be handed to
 * HWC2 directly: this module tracks each dumb buffer's CPU mapping and copies
 * the composited pixels into a gralloc scratch buffer that goes out through the
 * shared HWC2 present bridge.
 *
 * It also backs the faked CREATE_DUMB/MAP_DUMB/DESTROY_DUMB responses. KWin's
 * legacy KMS path issues those against a device whose master is owned by the
 * Android composer, so the real ioctls fail; the memfd-backed buffers here let
 * it proceed. mutter and phoc use real dumb buffers and never come through
 * here -- hybris_is_kwin() gates entry, in libdrm-hybris.c.
 *
 * This is the pure-software compositing path, and the escape hatch from the
 * hybris GL render corruption.
 */

#define _GNU_SOURCE

#include "common.h"
#include "kwin.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <drm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

/* --- QPainter (software) present bridge ------------------------------------
 * KWin's QPainter DRM backend renders into DRM *dumb* buffers (CPU-mapped) and
 * page-flips them. Those aren't gralloc buffers, so hybris_present_hwc2() can't hand
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

HYBRIS_INTERNAL int kwin_dumb_slot_by_gem(uint32_t gem) {
    for (int i = 0; i < kdumb_n; i++) if (kdumb[i].gem == gem) return i;
    return -1;
}
HYBRIS_INTERNAL int kwin_dumb_create_memfd(struct drm_mode_create_dumb *cd) {
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
    kdumb[slot].gem = KWIN_DUMB_FAKE_GEM(slot);
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
HYBRIS_INTERNAL void kwin_dumb_destroy_memfd(uint32_t gem) {
    int i = kwin_dumb_slot_by_gem(gem);
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
    if (KWIN_DUMB_OFF_MAGIC(offset)) {
        int idx = KWIN_DUMB_OFF_IDX(offset);
        if (idx >= 0 && idx < kdumb_n && kdumb[idx].memfd >= 0)
            return (void *)syscall(SYS_mmap, addr, length, prot, flags, kdumb[idx].memfd, (off_t)0);
    }
    return (void *)syscall(SYS_mmap, addr, length, prot, flags, fd, offset);
}
void *mmap64(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
    __attribute__((alias("mmap")));
static buffer_handle_t g_qp_gralloc = NULL;
static uint32_t g_qp_stride = 0;
HYBRIS_INTERNAL void kwin_dumb_note_create(uint32_t gem, size_t size, uint32_t pitch) {
    if (hybris_debug.sample)
        LOG("kdumb_create gem=%u size=%zu pitch=%u", gem, size, pitch);
    for (int i=0;i<kdumb_n;i++) if (kdumb[i].gem==gem){ kdumb[i].size=size; kdumb[i].pitch=pitch; kdumb[i].cpu=NULL; return; }
    if (kdumb_n<KDUMB_MAX){ kdumb[kdumb_n].gem=gem; kdumb[kdumb_n].cpu=NULL; kdumb[kdumb_n].size=size; kdumb[kdumb_n].pitch=pitch; kdumb_n++; }
}
HYBRIS_INTERNAL void kwin_dumb_note_map(int fd, uint32_t gem, uint64_t offset) {
    for (int i=0;i<kdumb_n;i++) if (kdumb[i].gem==gem){
        if (!kdumb[i].cpu && kdumb[i].size){
            int sv=hybris_in_hook; hybris_in_hook=1;
            void *m = mmap(NULL, kdumb[i].size, PROT_READ, MAP_SHARED, fd, (off_t)offset);
            hybris_in_hook=sv;
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
static struct { int fd; uint32_t dumb_gem; } primemap[HYBRIS_MAX_BUFFERS];
static int primemap_n = 0;
HYBRIS_INTERNAL void kwin_primemap_add(int fd, uint32_t dumb_gem) {
    for (int i=0;i<primemap_n;i++) if (primemap[i].fd==fd){ primemap[i].dumb_gem=dumb_gem; return; }
    if (primemap_n<HYBRIS_MAX_BUFFERS){ primemap[primemap_n].fd=fd; primemap[primemap_n].dumb_gem=dumb_gem; primemap_n++; }
    else { primemap[0].fd=fd; primemap[0].dumb_gem=dumb_gem; }
}
HYBRIS_INTERNAL uint32_t kwin_primemap_dumb(uint32_t fd) {
    for (int i=0;i<primemap_n;i++) if ((uint32_t)primemap[i].fd==fd) return primemap[i].dumb_gem;
    return 0;
}
/* Export a fake dumb buffer as a PRIME fd by duplicating its memfd. Keeping
 * this here means the interposer in libdrm-hybris.c does not have to reach into
 * the kdumb table. */
HYBRIS_INTERNAL int kwin_dumb_export_memfd(uint32_t handle, int *prime_fd) {
    int slot = kwin_dumb_slot_by_gem(handle);

    if (slot >= 0 && kdumb[slot].memfd >= 0) {
        int dfd = dup(kdumb[slot].memfd);

        if (dfd >= 0) {
            *prime_fd = dfd;
            kwin_primemap_add(dfd, handle);
            return 0;
        }
    }
    return -EINVAL;
}

static void *kdumb_cpu(uint32_t gem, uint32_t *pitch) {
    for (int i=0;i<kdumb_n;i++) if (kdumb[i].gem==gem){ if(pitch)*pitch=kdumb[i].pitch; return kdumb[i].cpu; }
    uint32_t dg = kwin_primemap_dumb(gem);   /* gem may be the exported prime fd */
    if (dg) for (int i=0;i<kdumb_n;i++) if (kdumb[i].gem==dg){ if(pitch)*pitch=kdumb[i].pitch; return kdumb[i].cpu; }
    return NULL;
}
/* KWin's QPainter backend never initialises EGL, so the drmadapter platform
 * (which registers drm_shim_set_present + brings up HWC2 in its init_module) is
 * never loaded. Force it: dlopen libEGL and eglInitialize once, which loads the
 * HYBRIS_EGLPLATFORM=drmadapter module and wires up hybris_present_fn + HWC2. */
static void kwin_ensure_present_fn(void) {
    if (hybris_present_fn) return;
    static int tried = 0;
    if (tried) return;
    tried = 1;
    int sv = hybris_in_hook; hybris_in_hook = 1;
    void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (egl) {
        void *(*getdisp)(void *) = (void *(*)(void *))dlsym(egl, "eglGetDisplay");
        unsigned (*init)(void *, int *, int *) = (unsigned (*)(void *,int *,int *))dlsym(egl, "eglInitialize");
        if (getdisp && init) {
            void *d = getdisp((void *)0);          /* EGL_DEFAULT_DISPLAY */
            if (d) { int mj = 0, mn = 0; init(d, &mj, &mn); }
        }
    }
    hybris_in_hook = sv;
    if (hybris_debug.sample)
        LOG("forced drmadapter EGL init, present_fn=%p", (void *)hybris_present_fn);
}
HYBRIS_INTERNAL int kwin_present_qpainter_dumb(uint32_t gem) {
    uint32_t spitch=0;
    void *src = kdumb_cpu(gem, &spitch);
    if (hybris_debug.sample) {
        static int n=0;
        if (n++ < 5) LOG("kwin_present_qpainter_dumb(gem=%u) src=%p kdumb_n=%d fw=%u",
                             gem, src, kdumb_n, hybris_frame.width);
    }
    if (!src || !hybris_frame.width || !hybris_frame.height) return 0;
    kwin_ensure_present_fn();
    /* Preferred: hand the dumb mapping straight to drmadapter for a single
     * swizzling copy into its present buffer (one full-frame pass instead of
     * two). When registered, it is authoritative: on failure (HWC2 not up yet)
     * skip the frame rather than fall through -- the scratch path below would
     * call hybris_gralloc_allocate before gralloc is loaded and assert. */
    if (hybris_present_cpu_fn) {
        return hybris_present_cpu_fn(src, spitch ? spitch : hybris_frame.width * 4) == 0 ? 1 : 0;
    }
    if (!g_qp_gralloc) {
        const int usage = 0x1000|0x800|0x200|0x33;   /* FB|COMPOSER|RENDER|SW rw */
        if (hybris_gralloc_allocate((int)hybris_frame.width, (int)hybris_frame.height, 1 /*RGBA_8888*/, usage,
                                    &g_qp_gralloc, &g_qp_stride) || !g_qp_gralloc) {
            g_qp_gralloc=NULL; return 0;
        }
    }
    void *dst=NULL;
    if (hybris_gralloc_lock(g_qp_gralloc, 0x3|0x30, 0, 0, (int)hybris_frame.width, (int)hybris_frame.height, &dst) || !dst) return 0;
    uint32_t dpitch = g_qp_stride*4;
    if (!spitch) spitch = hybris_frame.width*4;
    for (uint32_t y=0;y<hybris_frame.height;y++)
        memcpy((uint8_t*)dst + (size_t)y*dpitch, (uint8_t*)src + (size_t)y*spitch, (size_t)hybris_frame.width*4);
    hybris_gralloc_unlock(g_qp_gralloc);
    hybris_present_hwc2(g_qp_gralloc);
    return 1;
}
