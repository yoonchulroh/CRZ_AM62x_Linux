# Audio event recognition (AER) for the CRZ AM62x board
#
#   make                    cross-compile all modules into build/bin
#   make install DESTDIR=d  stage binaries, config, model and systemd units in d
#   make uninstall DESTDIR=d  remove what install staged
#   make clean              remove build/ (including the TensorFlow source and
#                           the TFLite build)
#
# Toolchain and headers come from the TI linux-devkit. TensorFlow Lite is not
# part of the devkit: its C library (libtensorflowlite_c.so) is cross-compiled
# here from the TensorFlow source with the devkit's CMake/Ninja, with the
# XNNPACK delegate built in. The C API never applies XNNPACK by itself
# (its op resolver has no default delegates); the inference module attaches
# it only when asked to (AER_INFERENCE_XNNPACK=1, set by "aer optimize").
# The first build downloads the TensorFlow source and the TFLite dependencies
# (XNNPACK, ruy, abseil, ...) into build/tflite.

TI_SDK_PATH	?= $(abspath ..)
DEVKIT		?= $(TI_SDK_PATH)/linux-devkit
SYSROOT		:= $(DEVKIT)/sysroots/aarch64-oe-linux
HOSTBIN		:= $(DEVKIT)/sysroots/x86_64-arago-linux/usr/bin
CC		:= $(HOSTBIN)/aarch64-oe-linux/aarch64-oe-linux-gcc --sysroot=$(SYSROOT)
WAYLAND_SCANNER	:= $(HOSTBIN)/wayland-scanner
XDG_SHELL_XML	:= $(SYSROOT)/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml

# Where AER is installed on the board
PREFIX		:= /opt/audio-event

BUILD		:= build
BIN		:= $(BUILD)/bin
GEN		:= $(BUILD)/gen
OBJ		:= $(BUILD)/obj

# TensorFlow Lite C library, cross-compiled from source
TF_VER		:= 2.14.1
TF_URL		:= https://github.com/tensorflow/tensorflow/archive/refs/tags/v$(TF_VER).tar.gz
TF_SHA		:= 6b31ed347ed7a03c45b906aa41628ac91c3db7c84cb816971400d470e58ba494
TFLITE		:= $(BUILD)/tflite
TF_TARBALL	:= $(TFLITE)/tensorflow-$(TF_VER).tar.gz
TF_SRC		:= $(TFLITE)/tensorflow-$(TF_VER)
TF_SRC_STAMP	:= $(TF_SRC)/.extracted
TFLITE_BUILD	:= $(TFLITE)/build
TFLITE_LIB	:= $(TFLITE_BUILD)/libtensorflowlite_c.so
TFLITE_TOOLCHAIN := $(TFLITE)/toolchain.cmake
CMAKE		:= $(HOSTBIN)/cmake
STRIP		:= $(HOSTBIN)/aarch64-oe-linux/aarch64-oe-linux-strip

CFLAGS		?= -O2 -g
CFLAGS		+= -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
		   -D_GNU_SOURCE -Isrc -I$(GEN) -I$(SYSROOT)/usr/include/cairo \
		   -fstack-protector-strong -D_FORTIFY_SOURCE=2
LDFLAGS		+= -Wl,-O1 -Wl,-z,relro,-z,now

# The modules; each is one C file in src/
MODULES		:= audio_input preprocess framer inference detector display \
		   notify broker
PROGRAMS	:= $(patsubst %,$(BIN)/aer-%,$(subst _,-,$(MODULES)))
COMMON_OBJS	:= $(OBJ)/common/config.o $(OBJ)/common/socket.o

LIBS_audio_input := -lasound
LIBS_preprocess	:= -lm
LIBS_framer	:= -lm
LIBS_inference	:= -L$(TFLITE_BUILD) -ltensorflowlite_c \
		   -Wl,-rpath,$(PREFIX)/lib
LIBS_display	:= -lwayland-client -lcairo -lm
EXTRA_display	:= $(OBJ)/xdg-shell-protocol.o

# Installed layout on the board (PREFIX is defined near the top: the
# inference link flags use it)
UNITS		:= $(wildcard systemd/*.service) systemd/aer.target

all: $(PROGRAMS)

define module_rule
$(BIN)/aer-$(subst _,-,$(1)): $(OBJ)/$(1).o $(COMMON_OBJS) $(EXTRA_$(1)) | $(BIN)
	$$(CC) $$(LDFLAGS) -o $$@ $$^ $(LIBS_$(1))
endef
$(foreach m,$(MODULES),$(eval $(call module_rule,$(m))))

$(OBJ)/%.o: src/%.c src/common/*.h | $(OBJ)/common
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ)/inference.o: $(TFLITE_LIB)
$(OBJ)/inference.o: CFLAGS += -I$(TF_SRC)
$(OBJ)/display.o: $(GEN)/xdg-shell-client-protocol.h

$(GEN)/xdg-shell-client-protocol.h: $(XDG_SHELL_XML) | $(GEN)
	$(WAYLAND_SCANNER) client-header $< $@

$(GEN)/xdg-shell-protocol.c: $(XDG_SHELL_XML) | $(GEN)
	$(WAYLAND_SCANNER) private-code $< $@

$(OBJ)/xdg-shell-protocol.o: $(GEN)/xdg-shell-protocol.c | $(OBJ)/common
	$(CC) $(CFLAGS) -c -o $@ $<

# TensorFlow source, checked against its SHA-256
$(TF_SRC_STAMP):
	@echo "  FETCH   TensorFlow $(TF_VER) source"
	@mkdir -p $(TFLITE)
	@if ! echo "$(TF_SHA)  $(TF_TARBALL)" | sha256sum -c --status >/dev/null 2>&1; then \
		rm -f $(TF_TARBALL); \
		curl -fsSL -o $(TF_TARBALL) $(TF_URL) || \
			wget -q -O $(TF_TARBALL) $(TF_URL) || exit 1; \
		echo "$(TF_SHA)  $(TF_TARBALL)" | sha256sum -c || exit 1; \
	fi
	@rm -rf $(TF_SRC)
	@tar -C $(TFLITE) -xzf $(TF_TARBALL)
	@touch $@

# libtensorflowlite_c.so for aarch64 with the devkit compiler, CMake and Ninja.
# The devkit bin directory goes first in PATH because XNNPACK's nested CMake
# runs (dependency downloads) look for ninja there, and
# CMAKE_POLICY_VERSION_MINIMUM lets CMake 4 configure the older dependencies.
$(TFLITE_LIB): $(TF_SRC_STAMP)
	@echo "  CMAKE   TensorFlow Lite C library (XNNPACK built in)"
	@printf '%s\n' \
		'set(CMAKE_SYSTEM_NAME Linux)' \
		'set(CMAKE_SYSTEM_PROCESSOR aarch64)' \
		'set(CMAKE_SYSROOT $(SYSROOT))' \
		'set(CMAKE_C_COMPILER $(HOSTBIN)/aarch64-oe-linux/aarch64-oe-linux-gcc)' \
		'set(CMAKE_CXX_COMPILER $(HOSTBIN)/aarch64-oe-linux/aarch64-oe-linux-g++)' \
		'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)' \
		'set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)' \
		'set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)' \
		'set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)' > $(TFLITE_TOOLCHAIN)
	PATH=$(HOSTBIN):$$PATH CMAKE_POLICY_VERSION_MINIMUM=3.5 \
		$(CMAKE) -S $(TF_SRC)/tensorflow/lite/c -B $(TFLITE_BUILD) -G Ninja \
		-DCMAKE_TOOLCHAIN_FILE=$(abspath $(TFLITE_TOOLCHAIN)) \
		-DCMAKE_BUILD_TYPE=Release -DTFLITE_ENABLE_XNNPACK=ON \
		-DTFLITE_ENABLE_RUY=ON \
		> $(TFLITE)/cmake.log 2>&1 || \
		{ tail -30 $(TFLITE)/cmake.log; exit 1; }
	PATH=$(HOSTBIN):$$PATH CMAKE_POLICY_VERSION_MINIMUM=3.5 \
		ninja -C $(TFLITE_BUILD) tensorflowlite_c

$(BIN) $(GEN) $(OBJ)/common:
	mkdir -p $@

install: all
	@test -n "$(DESTDIR)" || { echo "DESTDIR is not set"; exit 1; }
	install -d -m 755 $(DESTDIR)$(PREFIX) $(DESTDIR)$(PREFIX)/bin \
		   $(DESTDIR)$(PREFIX)/lib $(DESTDIR)/etc/systemd/system \
		   $(DESTDIR)/usr/local/bin
	install -m 755 $(PROGRAMS) $(DESTDIR)$(PREFIX)/bin
	install -m 644 $(TFLITE_LIB) $(DESTDIR)$(PREFIX)/lib
	$(STRIP) --strip-unneeded $(DESTDIR)$(PREFIX)/lib/libtensorflowlite_c.so
	install -m 644 spec/config.ini $(DESTDIR)$(PREFIX)/config.ini
	install -m 644 model/YAMNet.tflite $(DESTDIR)$(PREFIX)/yamnet.tflite
	install -m 644 model/YAMNet_Quantized.tflite \
		$(DESTDIR)$(PREFIX)/yamnet_quantized.tflite
	install -m 644 model/yamnet_class_map.csv \
		$(DESTDIR)$(PREFIX)/yamnet_class_map.csv
	install -m 644 $(UNITS) $(DESTDIR)/etc/systemd/system
	install -m 755 scripts/aer scripts/cpu-partition scripts/rt-sched \
		$(DESTDIR)/usr/local/bin

uninstall:
	@test -n "$(DESTDIR)" || { echo "DESTDIR is not set"; exit 1; }
	rm -rf $(DESTDIR)$(PREFIX)
	rm -f $(addprefix $(DESTDIR)/etc/systemd/system/,$(notdir $(UNITS)))
	rm -f $(addprefix $(DESTDIR)/usr/local/bin/,aer cpu-partition rt-sched)
	@# drop directories left empty, never DESTDIR itself
	-@for d in etc/systemd/system etc/systemd etc usr/local/bin usr/local usr \
		   opt; do rmdir $(DESTDIR)/$$d 2>/dev/null; done; true

clean:
	rm -rf $(BUILD)

.PHONY: all install uninstall clean
