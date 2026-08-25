**libdrm-hybris**

Unified LD_PRELOAD shim for running phosh and gnome on FuriOS devices
with a Mali GPU via the Android HWComposer2 (HWC2) backend.

One shared object containing every shim required:

* libseat -- fake API opening devices directly with O_NONBLOCK
* DRM caps -- render node advertisement, PRIME/vblank/timestamp patches
* EGL -- visual-id fix for Android EGL on RGBA8888 configs; gnome EGL
  platform display intercepts
* Wayland -- android_wlegl injection into gnome-shell's wl_display
* HWC2 vsync -- always returns HWC2_ERROR_NONE so schedule_frame() runs
* Buffer shims -- force 2 HWC buffers; discard redundant acquire fences
* DRM ioctls -- ADDFB2/ATOMIC/PAGE_FLIP intercepts plus a dumb linear
  KMS framebuffer maintained as a copy of the HWC2 front buffer

Session paths are detected at runtime, so the same shared object serves
phosh, gnome, weston, or any other compositor.

Tested on Furiphone FLX1 (Dimensity 900, Mali-G68 MC4) running FuriOS.

*Layout*

* src/ -- the unified shim (libdrm-hybris.c) plus companion libraries
  (wlegl_server.c)
* built/ -- pre-built aarch64 binaries of everything in src/

The drmadapter EGL platform lives in-tree in libhybris:
https://github.com/D0gg0Man/libhybris/tree/gnome-mali-drmadapter

*Install*

```
./install-libdrm-hybris.sh
```

Builds the shim, installs it as both libdrm-hybris.so and as a
replacement for libseat.so.1 (phoc links libseat directly and PAM strips
LD_PRELOAD for the greeter user, so library replacement is the only
reliable interception point), adds it to /etc/ld.so.preload, adds the
greeter user to the input group, and installs the phosh EGL drop-in so
phosh renders as a Wayland client via the drmadapter platform.


*Build only*

```
make
sudo make install
```
