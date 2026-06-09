**shims**

LD_PRELOAD shims for running phoc/phosh on FuriOS devices with a Mali GPU
via the wlroots hwcomposer backend (Android HWC2).

Pre-built aarch64 binaries are included (libseat_shim.so, drm_shim.so).

*libseat_shim.so*

* Fake libseat API with O_NONBLOCK device opens (critical for frame loop)
* DRM capability patches (render node, PRIME, vblank, timestamp)
* EGL visual-id fix for Android EGL returning 0 on RGBA8888 configs
* HWC2 vsync shim so schedule_frame() is always called
* Force 2 HWC buffers; discard redundant acquire fences

*drm_shim.so*

* DRM ioctl intercepts: ADDFB2, ATOMIC, AUTH_MAGIC, SETCRTC, PAGE_FLIP
* Dumb linear KMS framebuffer maintained as copy of HWC2 front buffer

*Building*

```
make
sudo make install
```

*See also*

* https://github.com/D0gg0Man/phosh-hwcomposer-session
