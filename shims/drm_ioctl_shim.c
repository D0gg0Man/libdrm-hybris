/*
 * drm_ioctl_shim.c -- DRM ioctl shim for phoc/phosh on FuriOS Furiphone FLX1
 *
 * The hwcomposer wlroots backend renders into HWCNativeWindow (Android
 * gralloc) and submits via HWC2. wlroots also opens /dev/dri/card0 for
 * auxiliary DRM operations. This shim intercepts ioctls that fail against
 * the Mali DRM driver and maintains a dumb KMS framebuffer copy for
 * screen capture tools.
 *
 * Intercepted ioctls:
 *   0xb8  ADDFB2       -- synthetic fb_ids (gralloc has no real GEM handles)
 *   0xbc  ATOMIC       -- stubbed, HWC2 handles presentation
 *   0x11  AUTH_MAGIC   -- always succeed
 *   0xa2  SETCRTC      -- succeed silently
 *   0xb0  PAGE_FLIP    -- redirect to dumb buffer
 */

#define _GNU_SOURCE
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
#include <android/android-config.h>
#include <hybris/gralloc/gralloc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

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
static int       crtc_set    = 0;

typedef int (*ioctl_t)(int, unsigned long, ...);
static ioctl_t real_ioctl = NULL;
static void ensure_real(void) {
    if (!real_ioctl) real_ioctl = dlsym(RTLD_NEXT, "ioctl");
}

void drm_shim_register_bo(uint32_t prime_fd, buffer_handle_t gralloc) {
    for (int i = 0; i < gmap_n; i++)
        if (gmap[i].prime_fd == prime_fd) { gmap[i].gralloc = gralloc; return; }
    int slot = gmap_n < MAX ? gmap_n++ : (gmap_n % MAX);
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
static void fmap_insert(uint32_t gem, uint32_t fb_id) {
    for (int i = 0; i < fmap_n; i++)
        if (fmap[i].gem == gem) { fmap[i].fb_id = fb_id; return; }
    int slot = fmap_n < MAX ? fmap_n++ : (fmap_n % MAX);
    fmap[slot].gem = gem; fmap[slot].fb_id = fb_id;
}
static void copy_to_dumb(buffer_handle_t h) {
    if (!dumb_map || !h || !frame_w || !frame_h) return;
    void *src = NULL;
    if (hybris_gralloc_lock(h, 0x3|0x30, 0, 0, frame_w, frame_h, &src) || !src) return;
    uint8_t *d = dumb_map, *s = src;
    for (uint32_t y = 0; y < frame_h; y++)
        memcpy(d + y*dumb_pitch, s + y*dumb_pitch, frame_w*4);
    hybris_gralloc_unlock(h);
}
static int init_dumb(int fd) {
    if (dumb_map) return 0;
    if (!frame_w || !frame_h) return -1;
    int saved = in_hook; in_hook = 1;
    struct drm_mode_create_dumb cd = { .height=frame_h, .width=frame_w, .bpp=32 };
    if (real_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { in_hook=saved; return -1; }
    dumb_handle=cd.handle; dumb_pitch=cd.pitch; dumb_size=cd.size;
    struct drm_mode_fb_cmd fb = {
        .width=frame_w,.height=frame_h,.pitch=dumb_pitch,.bpp=32,.depth=24,.handle=dumb_handle
    };
    if (real_ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb) == 0) {
        dumb_fb_id = fb.fb_id;
    } else {
        struct drm_mode_fb_cmd2 fb2 = { .width=frame_w,.height=frame_h,.pixel_format=0x34325258 };
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

int drmModeAddFB2WithModifiers(int fd, uint32_t w, uint32_t h, uint32_t fmt,
    const uint32_t handles[4], const uint32_t pitches[4], const uint32_t offsets[4],
    const uint64_t mod[4], uint32_t *buf_id, uint32_t flags) {
    if (!frame_w) { frame_w=w; frame_h=h; }
    if (!dumb_map) init_dumb(fd);
    uint32_t id=next_fake++; *buf_id=id; fmap_insert(handles[0],id); return 0;
}
int drmModeAddFB2(int fd, uint32_t w, uint32_t h, uint32_t fmt,
    const uint32_t handles[4], const uint32_t pitches[4], const uint32_t offsets[4],
    uint32_t *buf_id, uint32_t flags) {
    if (!frame_w) { frame_w=w; frame_h=h; }
    if (!dumb_map) init_dumb(fd);
    uint32_t id=next_fake++; *buf_id=id; fmap_insert(handles[0],id); return 0;
}
int drmModeRmFB(int fd, uint32_t id) { return 0; }
int drmModeSetCrtc(int fd, uint32_t crtcId, uint32_t bufferId, uint32_t x, uint32_t y,
    uint32_t *connectors, int count, drmModeModeInfoPtr mode) {
    if (!dumb_map) init_dumb(fd); crtc_set=1; return 0;
}
int drmModePageFlip(int fd, uint32_t crtc_id, uint32_t fb_id, uint32_t flags, void *ud) {
    buffer_handle_t h=find_by_fb(fb_id); copy_to_dumb(h);
    typedef int (*fn_t)(int,uint32_t,uint32_t,uint32_t,void*);
    fn_t real=dlsym(RTLD_NEXT,"drmModePageFlip");
    return real ? real(fd,crtc_id,dumb_fb_id?dumb_fb_id:fb_id,flags,ud) : 0;
}
int drmModeAtomicCommit(int fd, drmModeAtomicReqPtr req, uint32_t flags, void *ud) {
    for (int i=fmap_n-1; i>=0; i--) {
        buffer_handle_t h=find_gralloc(fmap[i].gem);
        if (h) { copy_to_dumb(h); break; }
    }
    return 0;
}
int ioctl(int fd, unsigned long request, ...) {
    ensure_real();
    va_list args; va_start(args,request); void *arg=va_arg(args,void*); va_end(args);
    if (in_hook) return real_ioctl(fd,request,arg);
    uint32_t nr=request&0xff, magic=(request>>8)&0xff;
    if (magic != 0x64) return real_ioctl(fd,request,arg);
    in_hook=1; int ret;
    if (nr==0xb8) {
        uint32_t *fb=arg, w=fb[0], h=fb[1], gem=fb[5];
        if (!frame_w) { frame_w=w; frame_h=h; }
        if (!dumb_map) init_dumb(fd);
        uint32_t id=next_fake++; fb[6]=id; fmap_insert(gem,id); ret=0;
    } else if (nr==0xaf) { ret=real_ioctl(fd,request,arg);
    } else if (nr==0xa2) {
        if (!dumb_map) init_dumb(fd);
        real_ioctl(fd,request,arg); crtc_set=1; ret=0;
    } else if (nr==0xb0||nr==0xb6) {
        struct drm_mode_crtc_page_flip *flip=arg;
        buffer_handle_t h=find_by_fb(flip->fb_id); copy_to_dumb(h);
        if (dumb_fb_id) flip->fb_id=dumb_fb_id;
        ret=real_ioctl(fd,request,arg);
        if (ret!=0) crtc_set=0;
    } else if (nr==0xbc) {
        for (int i=fmap_n-1; i>=0; i--) {
            buffer_handle_t h=find_gralloc(fmap[i].gem);
            if (h) { copy_to_dumb(h); break; }
        }
        ret=0;
    } else if (nr==0x11) { ret=0;
    } else { ret=real_ioctl(fd,request,arg); }
    in_hook=0; return ret;
}
