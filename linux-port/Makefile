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

CC  ?= cc
CXX ?= c++

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

# OpenCV (C++ only) enables the live viewer, tools/dytview.cpp.  It also needs
# libusb for the capture layer, so the tool is only built when both are present.
#
# Only the five modules the viewer actually calls are linked.  opencv4.pc is a
# metapackage whose Libs line names every built module — videoio pulls in
# gstreamer/ffmpeg backends the viewer never touches — so take just the search
# path and -pthread from it and list the modules explicitly.
CV_CFLAGS := $(shell pkg-config --cflags opencv4 2>/dev/null)
CV_LIBS   := $(shell pkg-config --libs-only-L --libs-only-other opencv4 2>/dev/null) \
             -lopencv_core -lopencv_imgproc -lopencv_highgui \
             -lopencv_videoio -lopencv_imgcodecs
CXXFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -ffp-contract=off
CXXFLAGS += -Isrc -Ithird_party/libuvc/include $(USB_CFLAGS)
ifneq ($(shell pkg-config --exists opencv4 && echo yes),)
CXXFLAGS += -DDYT_HAVE_OPENCV $(CV_CFLAGS)
HAVE_OPENCV := 1
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

# ---------------------------------------------------------------------------
# Flag-change detection.
#
# CFLAGS gains -DDYT_HAVE_LIBUSB when libusb is installed and CXXFLAGS gains
# -DDYT_HAVE_OPENCV when OpenCV is.  make compares *file mtimes*, never
# variable values, so a tree configured before those packages were present
# would keep its old capture.o / dytview forever: capture_demo would silently
# print "built without libusb" while `make check` stayed green.  Stamp the
# flags and make every object depend on the stamp, so changing CFLAGS,
# CXXFLAGS or UVC_CFLAGS forces a rebuild.  The stamp is only rewritten when
# the flags actually differ, so a normal build still rebuilds nothing.
# ---------------------------------------------------------------------------
FLAG_STAMP := $(BUILD)/.flags
$(shell mkdir -p $(BUILD); \
        if [ "$$(cat $(FLAG_STAMP) 2>/dev/null)" != '$(CFLAGS) | $(UVC_CFLAGS) | $(CXXFLAGS)' ]; then \
            printf '%s\n' '$(CFLAGS) | $(UVC_CFLAGS) | $(CXXFLAGS)' > $(FLAG_STAMP); \
        fi)

LIB      := $(BUILD)/libdyt.a
LIB_OBJS := $(BUILD)/thermometry.o $(BUILD)/frame.o \
            $(BUILD)/control.o $(BUILD)/serial.o $(BUILD)/calib.o \
            $(BUILD)/capture.o $(BUILD)/palette.o $(BUILD)/imgwrite.o

TESTS := $(BUILD)/control_test $(BUILD)/serial_test $(BUILD)/frame_test \
         $(BUILD)/calib_test $(BUILD)/pipeline_test $(BUILD)/palette_test
TOOLS := $(BUILD)/dumpframe $(BUILD)/taucmp
# capture_demo and probe talk to the device directly through libusb — only
# buildable with it.  capture_demo's prereq on $(UVC_LIB) is what pulls the
# vendored libuvc into the build when libusb is present; with no libusb,
# nothing references $(UVC_LIB) and it stays dormant.
ifdef HAVE_LIBUSB
TOOLS += $(BUILD)/capture_demo $(BUILD)/probe $(BUILD)/isotest
# The viewer is C++ and needs OpenCV on top of libusb.
ifdef HAVE_OPENCV
TOOLS += $(BUILD)/dytview
endif
endif

# Frozen vendor ground truth, produced by tools/thermometry_diff/run.sh
# from the real libthermometry.so.  dumpframe --check diffs against these.
FROZEN := $(addprefix tools/thermometry_diff/out/, \
            240 240_fixon 256 256_fixon 384 384_fixon 640 640_fixon)

.PHONY: all check clean

# libuvc is only needed by the live-capture tools; the byte-verified
# pipeline and its tests build with just a C compiler, so $(UVC_LIB) is
# pulled in transitively via capture_demo (see below) and not listed here.
all: $(LIB) $(TESTS) $(TOOLS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c $(FLAG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/libuvc/%.o: $(UVC_DIR)/src/%.c $(FLAG_STAMP) | $(BUILD)
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

# pipeline_test drives the device-independent live-capture pipeline over a
# frozen mode-1000 frame captured from the real device (testdata/) plus the
# vendor 0x44c ground-truth frame.  See testdata/README.md.
$(BUILD)/pipeline_test: src/pipeline_test.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

# palette_test checks the render core against RE Docs 06 §1.2 and against a
# golden PPM rendered from the frozen frame.  See src/palette_test.c.
$(BUILD)/palette_test: src/palette_test.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

$(BUILD)/dumpframe: src/dumpframe.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

$(BUILD)/probe: tools/probe.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

$(BUILD)/capture_demo: tools/capture_demo.c $(LIB) $(UVC_LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(UVC_LIB) $(LDLIBS) -o $@

# The live viewer (C++ over libdyt + libuvc + OpenCV).  Only reachable when
# HAVE_OPENCV is set — see the TOOLS list above.
$(BUILD)/dytview: tools/dytview.cpp $(LIB) $(UVC_LIB) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< $(LIB) $(UVC_LIB) $(LDLIBS) $(CV_LIBS) -o $@

# isotest bypasses libuvc to test each streaming altsetting directly — see the
# header comment in tools/isotest.c.  Read-only.
$(BUILD)/isotest: tools/isotest.c | $(BUILD)
	$(CC) $(CFLAGS) $< $(LDLIBS) -o $@

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
	@$(BUILD)/pipeline_test testdata/mode1000_256x192.raw \
	                       tools/thermometry_diff/out/256/in_frame.bin
	@$(BUILD)/palette_test palettes testdata/mode1000_256x192.raw \
	                       testdata/golden/mode1000_iron_30-31.2.ppm
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
