# audio-event-recognition: cross-compiled with the SDK toolchain (see
# sample/touch-demo). "./build.sh aer" runs "make" and "make install
# DESTDIR=image/custom_datafs".

SDK_DIR    ?= $(abspath $(CURDIR)/..)
DEVKIT     ?= $(SDK_DIR)/linux-devkit/sysroots
SYSROOT    ?= $(DEVKIT)/aarch64-oe-linux
HOST_SYS   ?= $(DEVKIT)/x86_64-arago-linux

TOOLPREFIX := $(HOST_SYS)/usr/bin/aarch64-oe-linux/aarch64-oe-linux-
CC         := $(TOOLPREFIX)gcc
AR         := $(TOOLPREFIX)ar
STRIP      := $(TOOLPREFIX)strip
SCANNER    := $(HOST_SYS)/usr/bin/wayland-scanner
XDG_XML    := $(SYSROOT)/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml

# Downloaded by scripts/fetch-deps.sh: model, labels, Ubuntu TFLite packages
DEPS       := third_party
TFLITE     := $(DEPS)/tflite
TFLITE_LIB := $(TFLITE)/usr/lib/aarch64-linux-gnu
DEPS_STAMP := $(DEPS)/tflite/.done

B          := build
PREFIX     ?= /usr/local
DESTDIR    ?=

CFLAGS     += --sysroot=$(SYSROOT) -O2 -g -Wall -Wextra -Icommon -I$(B)
LDFLAGS    += --sysroot=$(SYSROOT)

MODULES    := aer-capture aer-preprocess aer-framer aer-infer aer-detect \
              aer-display aer-notify aer-broker
BINS       := $(addprefix $(B)/,$(MODULES))
COMMON     := $(B)/libaer.a
COMMON_OBJ := $(B)/util.o $(B)/conf.o $(B)/ipc.o $(B)/mqtt.o

UNITS      := aer.target $(addsuffix .service,$(MODULES))

all: $(BINS)

deps: $(DEPS_STAMP)

$(DEPS_STAMP):
	./scripts/fetch-deps.sh

$(B):
	mkdir -p $@

$(B)/%.o: common/%.c common/*.h | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

$(COMMON): $(COMMON_OBJ)
	$(AR) rcs $@ $^

# ---- one program per module

$(B)/aer-capture: capture/aer-capture.c $(COMMON)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(COMMON) -lasound

$(B)/aer-preprocess: preprocess/aer-preprocess.c $(COMMON)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(COMMON) -lm

$(B)/aer-framer: framer/aer-framer.c $(COMMON)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(COMMON)

# Links against Ubuntu's libtensorflow-lite.so.2.14.1 (C API included);
# -rpath-link lets the linker resolve its absl/cpuinfo/... dependencies.
$(B)/aer-infer: infer/aer-infer.c $(COMMON) $(DEPS_STAMP)
	$(CC) $(CFLAGS) -I$(TFLITE)/usr/include $(LDFLAGS) -o $@ $< $(COMMON) \
		-L$(TFLITE_LIB) -Wl,-rpath-link,$(TFLITE_LIB) -l:libtensorflow-lite.so.2.14.1 -lm

$(B)/aer-detect: detect/aer-detect.c $(COMMON)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(COMMON)

$(B)/xdg-shell-client-protocol.h: $(XDG_XML) | $(B)
	$(SCANNER) client-header $< $@

$(B)/xdg-shell-protocol.c: $(XDG_XML) | $(B)
	$(SCANNER) private-code $< $@

$(B)/aer-display: display/aer-display.c $(B)/xdg-shell-protocol.c $(B)/xdg-shell-client-protocol.h $(COMMON)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(B)/xdg-shell-protocol.c $(COMMON) \
		-lwayland-client -lcairo -lm

$(B)/aer-notify: notify/aer-notify.c $(COMMON)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(COMMON)

$(B)/aer-broker: broker/aer-broker.c $(COMMON)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(COMMON)

# ---- installation into the target root (image/custom_datafs)

SYSTEMD    := $(DESTDIR)/etc/systemd/system

install: all $(DEPS_STAMP)
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/aer $(DESTDIR)/etc/aer \
		$(SYSTEMD)/multi-user.target.wants $(SYSTEMD)/aer.target.wants
	install -m 755 -s --strip-program=$(STRIP) $(BINS) $(DESTDIR)$(PREFIX)/bin/
	install -m 644 $(DEPS)/yamnet.tflite $(DEPS)/yamnet_labels.txt README.md \
		$(DESTDIR)$(PREFIX)/share/aer/
	install -m 644 config/aer.conf $(DESTDIR)/etc/aer/aer.conf
	install -m 644 $(addprefix systemd/,$(UNITS)) $(SYSTEMD)/
	# enabled, as "systemctl enable" would do on the target
	ln -sf /etc/systemd/system/aer.target $(SYSTEMD)/multi-user.target.wants/aer.target
	for m in $(MODULES); do \
		ln -sf /etc/systemd/system/$$m.service $(SYSTEMD)/aer.target.wants/$$m.service; \
	done

clean:
	rm -rf $(B)

distclean: clean
	rm -rf $(DEPS)

.PHONY: all deps install clean distclean
