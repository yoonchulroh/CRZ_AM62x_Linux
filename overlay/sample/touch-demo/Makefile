# touch-demo: Wayland touch test client, cross-compiled with the SDK toolchain

SDK_DIR    ?= $(abspath $(CURDIR)/../..)
DEVKIT     ?= $(SDK_DIR)/linux-devkit/sysroots
SYSROOT    ?= $(DEVKIT)/aarch64-oe-linux
HOST_SYS   ?= $(DEVKIT)/x86_64-arago-linux

CC         := $(HOST_SYS)/usr/bin/aarch64-oe-linux/aarch64-oe-linux-gcc
SCANNER    := $(HOST_SYS)/usr/bin/wayland-scanner
XDG_XML    := $(SYSROOT)/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml

CFLAGS     += --sysroot=$(SYSROOT) -O2 -Wall -Wextra -I.
LDFLAGS    += --sysroot=$(SYSROOT)
LDLIBS     := -lwayland-client

GEN        := xdg-shell-client-protocol.h xdg-shell-protocol.c
OBJS       := touch-demo.o xdg-shell-protocol.o

all: touch-demo

touch-demo: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

touch-demo.o: touch-demo.c xdg-shell-client-protocol.h
	$(CC) $(CFLAGS) -c -o $@ $<

xdg-shell-protocol.o: xdg-shell-protocol.c
	$(CC) $(CFLAGS) -c -o $@ $<

xdg-shell-client-protocol.h: $(XDG_XML)
	$(SCANNER) client-header $< $@

xdg-shell-protocol.c: $(XDG_XML)
	$(SCANNER) private-code $< $@

clean:
	rm -f touch-demo $(OBJS) $(GEN)

.PHONY: all clean
