CC     ?= gcc
CFLAGS ?= -O2 -fPIC -shared -Wall -Wextra \
          -I/usr/include/libdrm \
          -I/usr/include \
          -I/usr/include/android
LDFLAGS ?=
LIBDIR  ?= /usr/lib/aarch64-linux-gnu

.PHONY: all clean install

all: libseat_shim.so drm_shim.so

libseat_shim.so: libseat_shim.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -ldl -lEGL

drm_shim.so: drm_ioctl_shim.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -ldl -lEGL -lgralloc -ldrm

install: all
	install -d $(DESTDIR)$(LIBDIR)
	install -m 755 libseat_shim.so $(DESTDIR)$(LIBDIR)/libseat_shim.so
	install -m 755 drm_shim.so     $(DESTDIR)$(LIBDIR)/drm_shim.so

clean:
	rm -f libseat_shim.so drm_shim.so
