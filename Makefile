CC = gcc

CFLAGS = -O2 -fPIC -I/usr/include/android `pkg-config --cflags glib-2.0 wayland-server libdrm`
LDFLAGS = -shared `pkg-config --libs glib-2.0 wayland-server libdrm libgralloc`

SOURCES = src/libdrm-hybris.c

TARGET  = libdrm-hybris.so

PREFIX ?= /usr
TRIPLET ?= $(shell $(CC) -dumpmachine)

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(SOURCES) -o $(TARGET) $(CFLAGS) $(LDFLAGS)

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/lib/$(TRIPLET)/libdrm-hybris/
	install -m 0644 $(TARGET) $(DESTDIR)$(PREFIX)/lib/$(TRIPLET)/libdrm-hybris/

clean:
	rm -f $(TARGET)

.PHONY: all install clean
