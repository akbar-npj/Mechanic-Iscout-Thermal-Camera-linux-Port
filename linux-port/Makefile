# Makefile — DYT thermal camera Linux port
#
# Builds the byte-verified port (thermometry + frame + control + serial)
# into build/libdyt.a, plus the unit tests and the CLI tools.
#
#   make            build everything
#   make check      regression gate (unit tests + frozen-ground-truth diff)
#   make clean
#
# `make check` runs the two unit-test binaries and byte-compares the frame
# pipeline against the frozen vendor ground truth for all four sensor
# widths in both fix_mode configurations.  It is the CI gate: it exits
# non-zero on any failure.

CC ?= cc

# -ffp-contract=off is MANDATORY.  The thermometry port reproduces the
# vendor libthermometry.so byte-for-byte only because FP contraction is
# disabled and every fused multiply-add is written explicitly as fmaf().
# Do not remove it.  See RE Docs 08 §8.6.1.
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -ffp-contract=off
CFLAGS += -Isrc -Ithird_party/libuvc/include
LDLIBS := -lm -lpthread

# libusb-1.0 enables the real control-transfer backend and the probe tool.
USB_CFLAGS := $(shell pkg-config --cflags libusb-1.0 2>/dev/null)
USB_LIBS   := $(shell pkg-config --libs   libusb-1.0 2>/dev/null)
ifneq ($(strip $(USB_LIBS)),)
CFLAGS += -DDYT_HAVE_LIBUSB $(USB_CFLAGS)
LDLIBS += $(USB_LIBS)
HAVE_LIBUSB := 1
endif

BUILD := build

# ---------------------------------------------------------------------------
# libuvc — vendored upstream, pinned to tag v0.0.8
# (commit 4e9fc773914377ec0bcf2f31621f56da5a0fa09f).
#
# Compiled directly from source, no CMake.  JPEG is disabled: the thermal
# stream is uncompressed raw16, so frame-mjpeg.c is omitted and libjpeg is
# not linked.  Vendored third-party code is built with -w so its warnings
# do not drown out ours (we do not patch upstream).
# ---------------------------------------------------------------------------
UVC_DIR    := third_party/libuvc
UVC_SRCS   := ctrl.c ctrl-gen.c device.c diag.c frame.c init.c misc.c stream.c
UVC_LIB    := $(BUILD)/libuvc.a
UVC_OBJS   := $(addprefix $(BUILD)/libuvc/,$(UVC_SRCS:.c=.o))
UVC_CFLAGS ?= -O2 -g -fPIC -w -I$(UVC_DIR)/include $(USB_CFLAGS)

LIB      := $(BUILD)/libdyt.a
LIB_OBJS := $(BUILD)/thermometry.o $(BUILD)/frame.o \
            $(BUILD)/control.o $(BUILD)/serial.o $(BUILD)/calib.o \
            $(BUILD)/capture.o

TESTS := $(BUILD)/control_test $(BUILD)/serial_test $(BUILD)/frame_test \
         $(BUILD)/calib_test
TOOLS := $(BUILD)/dumpframe $(BUILD)/capture_demo $(BUILD)/taucmp
# probe talks to the device directly through libusb — only buildable with it.
ifdef HAVE_LIBUSB
TOOLS += $(BUILD)/probe
endif

# Frozen vendor ground truth, produced by tools/thermometry_diff/run.sh
# from the real libthermometry.so.  dumpframe --check diffs against these.
FROZEN := $(addprefix tools/thermometry_diff/out/, \
            240 240_fixon 256 256_fixon 384 384_fixon 640 640_fixon)

.PHONY: all check clean

all: $(LIB) $(UVC_LIB) $(TESTS) $(TOOLS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/libuvc/%.o: $(UVC_DIR)/src/%.c | $(BUILD)
	@mkdir -p $(BUILD)/libuvc
	$(CC) $(UVC_CFLAGS) -c $< -o $@

# The 'D' is not optional: without it ar embeds each member's mtime and
# uid/gid in the archive header, so two builds from identical objects
# produce different .a files.  With it the whole build is byte-reproducible
# (verified: two clean builds hash identically across all 24 artifacts).
$(UVC_LIB): $(UVC_OBJS)
	$(AR) rcsD $@ $^

$(LIB): $(LIB_OBJS)
	$(AR) rcsD $@ $^

$(BUILD)/control_test: src/control_test.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

$(BUILD)/serial_test: src/serial_test.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

$(BUILD)/frame_test: src/frame_test.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

# calib_test reads the vendor blobs from calib/ — see calib/README.md.
$(BUILD)/calib_test: src/calib_test.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

$(BUILD)/dumpframe: src/dumpframe.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

$(BUILD)/probe: tools/probe.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

$(BUILD)/capture_demo: tools/capture_demo.c $(LIB) $(UVC_LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(UVC_LIB) $(LDLIBS) -o $@

# taucmp is a diagnostic, not part of the port: it compares the analytic
# transmittance model against the two table families.  See RE Docs 10 §8.3.
$(BUILD)/taucmp: tools/taucmp.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

check: all
	@echo "== unit tests =="
	@$(BUILD)/control_test
	@$(BUILD)/serial_test
	@$(BUILD)/frame_test
	@$(BUILD)/calib_test calib
ifdef HAVE_LIBUSB
	@echo "== probe safety self-test =="
	@$(BUILD)/probe --selftest
endif
	@echo "== frame pipeline vs frozen vendor ground truth =="
	@fail=0; \
	for d in $(FROZEN); do \
	    printf '  %-22s ' "$$d"; \
	    if $(BUILD)/dumpframe --check $$d >/dev/null 2>&1; then \
	        echo PASS; \
	    else \
	        echo FAIL; fail=1; \
	    fi; \
	done; \
	if [ $$fail -ne 0 ]; then echo "== CHECK FAILED =="; exit 1; fi; \
	echo "== all checks PASS =="

clean:
	rm -rf $(BUILD)
