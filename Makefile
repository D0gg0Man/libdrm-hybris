CC     ?= gcc
CFLAGS ?= -O2 -fPIC -shared -Wall -Wextra \
          -I/usr/include/libdrm \
          -I/usr/include \
          -I/usr/include/android
LIBDIR ?= /usr/lib/aarch64-linux-gnu

OUT = built/libdrm-hybris.so

# libdrm-hybris.c holds the interposed entry points; the rest is split by the
# compositor each path exists for. common.c carries the shared state and the
# helpers every module needs.
SRCS = src/libdrm-hybris.c \
       src/common.c \
       src/wlroots.c \
       src/mutter.c \
       src/kwin.c

HDRS = src/common.h src/wlroots.h src/mutter.h src/kwin.h

.PHONY: all clean install

all: $(OUT)

$(OUT): $(SRCS) $(HDRS)
	mkdir -p built
	$(CC) $(CFLAGS) -o $@ $(SRCS) -ldl -lEGL -lgralloc -ldrm -lwayland-server -lpthread

install: all
	install -d $(DESTDIR)$(LIBDIR)
	install -m 755 $(OUT) $(DESTDIR)$(LIBDIR)/libdrm-hybris.so

clean:
	rm -f $(OUT)
