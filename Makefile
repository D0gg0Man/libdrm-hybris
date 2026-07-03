CC     ?= gcc
CFLAGS ?= -O2 -fPIC -shared -Wall -Wextra \
          -I/usr/include/libdrm \
          -I/usr/include \
          -I/usr/include/android
LIBDIR ?= /usr/lib/aarch64-linux-gnu

OUT = built/libdrm-hybris.so

.PHONY: all clean install

all: $(OUT)

$(OUT): src/libdrm-hybris.c
	mkdir -p built
	$(CC) $(CFLAGS) -o $@ $< -ldl -lEGL -lgralloc -ldrm -lwayland-server

install: all
	install -d $(DESTDIR)$(LIBDIR)
	install -m 755 $(OUT) $(DESTDIR)$(LIBDIR)/libdrm-hybris.so

clean:
	rm -f $(OUT)
