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
CFLAGS += -Isrc -Ithird_party/libuvc/include -Ithird_party/stb
LDLIBS := -lm -lpthread

# The app's version, in one place: the GUI compiles it in (the -DDYT_VERSION
# macro below) and both packages are named after it, so the About box and the
# packages cannot disagree.  See the packaging section at the end of this file.
VERSION ?= 0.1.0

# The build directory.  Defined here, before the feature-detection blocks, so
# their rules (the build-tree libMNN.so symlink) can name a path under it.
BUILD := build

# libusb-1.0 enables the real control-transfer backend and the probe tool.
USB_CFLAGS := $(shell pkg-config --cflags libusb-1.0 2>/dev/null)
USB_LIBS   := $(shell pkg-config --libs   libusb-1.0 2>/dev/null)
ifneq ($(strip $(USB_LIBS)),)
CFLAGS += -DDYT_HAVE_LIBUSB $(USB_CFLAGS)
LDLIBS += $(USB_LIBS)
HAVE_LIBUSB := 1
endif

# ---------------------------------------------------------------------------
# JPEG codec backend (src/jpeg.c).
#
# The default is the **vendored stb** (third_party/stb), because it is always
# present and produces identical bytes on every machine — which is the same
# reproducibility property the archive rule below protects.  libjpeg is an
# optional accelerator, selected explicitly:
#
#     make JPEG=libjpeg
#
# It is detected (not auto-selected) so that a default build does not silently
# change codec depending on what happens to be installed.  Availability is
# reported as HAVE_LIBJPEG; asking for libjpeg without it is a build error.
# ---------------------------------------------------------------------------
JPEG_CFLAGS := $(shell pkg-config --cflags libjpeg 2>/dev/null)
JPEG_LIBS   := $(shell pkg-config --libs   libjpeg 2>/dev/null)
ifneq ($(strip $(JPEG_LIBS)),)
HAVE_LIBJPEG := 1
endif

JPEG ?= stb
ifeq ($(JPEG),libjpeg)
ifndef HAVE_LIBJPEG
$(error JPEG=libjpeg requested but libjpeg was not found by pkg-config)
endif
CFLAGS += -DDYT_HAVE_LIBJPEG $(JPEG_CFLAGS)
LDLIBS += $(JPEG_LIBS)
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

# MNN (C++ only) backs the optional super-resolution seam in src/mnn.c.  It is
# genuinely optional: without it src/mnn.c compiles to a seam that reports
# DYT_MNN_UNAVAILABLE and never invents a frame, and every target below still
# builds.  This is the same gate as OpenCV's.
#
# MNN_ROOT must name an install carrying include/MNN/Interpreter.hpp and
# lib/libMNN.so.  See third_party/README.md for how to produce one.
MNN_ROOT ?= third_party/mnn-install
ifneq ($(wildcard $(MNN_ROOT)/include/MNN/Interpreter.hpp),)
ifneq ($(wildcard $(MNN_ROOT)/lib/libMNN.so),)
CFLAGS   += -DDYT_HAVE_MNN
MNN_CFLAGS := -I$(MNN_ROOT)/include
# Where libMNN.so is looked for at run time, relative to the binary — never an
# absolute path into the machine that built it, because a package would ship
# that path (lintian's binary-or-shlib-defines-rpath) and a target machine
# could then load a library the package does not contain.  Three entries cover
# the three layouts, each of which puts the library one directory up and over
# from the binary ($(BINDIR) -> $(LIBDIR)/dytqt, both under $(PREFIX)):
#
#   $ORIGIN                 the build tree, where the Makefile symlinks the lib
#   $ORIGIN/../lib/dytqt    $(PREFIX)/lib — `make install`'s default, and the deb
#   $ORIGIN/../lib64/dytqt  $(PREFIX)/lib64 — Fedora's %{_libdir}, and the rpm
#
# The quoted form is required — the tree's own path contains a space, and
# unquoted the shell would hand the linker three arguments.  $$ORIGIN is
# make-escaped: one `$` survives to the recipe and the shell's single quotes
# leave it for the linker.
MNN_RPATH  := -Wl,-rpath,'$$ORIGIN:$$ORIGIN/../lib/dytqt:$$ORIGIN/../lib64/dytqt'
MNN_LIBS   := -L$(MNN_ROOT)/lib -lMNN $(MNN_RPATH)
HAVE_MNN   := 1
endif
endif

# The build-tree libMNN.so the $ORIGIN rpath resolves to, as a prerequisite of
# the two binaries that link the runtime.  Empty without MNN, so those rules do
# not depend on a target that has no rule.
MNN_DEP :=
ifdef HAVE_MNN
MNN_DEP := $(BUILD)/libMNN.so
endif

# ---------------------------------------------------------------------------
# Qt6 Widgets (C++ only) backs the native GUI shell, gui/dytqt.cpp.
#
# Qt is an *alternative* front end to the OpenCV viewer, not a replacement for
# the port's rule that every capability is reachable without a display: the Qt
# app is a thin shell over the same toolkit-independent presentation layer
# (src/view_model.c) that tools/dytview.cpp draws with.  Only QtWidgets is
# named; QtGui and QtCore arrive as its dependencies, and pkg-config's
# Qt6Widgets.pc already carries all three sets of flags.
#
# -fPIC is not optional on Qt6: the Qt libraries are built with
# -reduce-relocations, so an executable that links them must itself be
# position-independent or the link fails with "recompile with -fPIC".
#
# Detected, never required — without Qt6 the GUI target is simply absent and
# every other target still builds.
# ---------------------------------------------------------------------------
QT6_CFLAGS := $(shell pkg-config --cflags Qt6Widgets 2>/dev/null)
QT6_LIBS   := $(shell pkg-config --libs   Qt6Widgets 2>/dev/null)
ifneq ($(strip $(QT6_LIBS)),)
QT6_CFLAGS += -fPIC
HAVE_QT6   := 1
endif

# The Qt app's About box names the features the binary was built with, so it
# needs the same two macros the C objects get.  It links both libraries (libuvc
# through UVC_LINK, MNN through libdyt), so a box that claimed otherwise would
# be lying about the binary it is in.
ifdef HAVE_LIBUSB
QT_DEFS += -DDYT_HAVE_LIBUSB
endif
ifdef HAVE_MNN
QT_DEFS += -DDYT_HAVE_MNN
endif

# The version the About box reports, taken from the same VERSION the package is
# built as.  The single quotes survive make and the shell and leave the double
# quotes for the compiler, so the macro is the string "0.1.0" and not a number.
QT_DEFS += -DDYT_VERSION='"$(VERSION)"'

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
# stb — vendored upstream, pinned to commit
# 2c980bb59875b0d32144a71867fbdebb2f77cd20 (third_party/stb/README.md).
#
# src/stb_impl.c is the single translation unit that instantiates the stb
# implementations.  It is the port's JPEG/PNG codec, so unlike libuvc it *is*
# on the default build path; -ffp-contract=off is kept even here so no new
# object can quietly reintroduce contraction.
# ---------------------------------------------------------------------------
STB_CFLAGS ?= -O2 -g -w -ffp-contract=off -Ithird_party/stb

# ---------------------------------------------------------------------------
# Flag-change detection.
#
# CFLAGS gains -DDYT_HAVE_LIBUSB when libusb is installed, -DDYT_HAVE_LIBJPEG
# when JPEG=libjpeg is asked for, and CXXFLAGS gains -DDYT_HAVE_OPENCV when
# OpenCV is.  make compares *file mtimes*, never variable values, so a tree
# configured before those packages were present would keep its old capture.o /
# dytview forever: capture_demo would silently print "built without libusb"
# while `make check` stayed green.  Stamp the flags and make every object
# depend on the stamp, so changing CFLAGS, CXXFLAGS, UVC_CFLAGS or STB_CFLAGS
# forces a rebuild.  The stamp is only rewritten when the flags actually
# differ, so a normal build still rebuilds nothing.
# ---------------------------------------------------------------------------
FLAG_STAMP := $(BUILD)/.flags
FLAG_STAMP_VAL := $(CFLAGS) | $(UVC_CFLAGS) | $(STB_CFLAGS) | $(CXXFLAGS)
$(shell mkdir -p $(BUILD); \
        if [ "$$(cat $(FLAG_STAMP) 2>/dev/null)" != '$(FLAG_STAMP_VAL)' ]; then \
            printf '%s\n' '$(FLAG_STAMP_VAL)' > $(FLAG_STAMP); \
        fi)

# ---------------------------------------------------------------------------
# Header dependencies.
#
# The generic object rule below names only the .c, so make has no idea that an
# object depends on the headers it includes.  Editing a header therefore left
# stale objects behind, and because the test binaries are compiled *from
# source* on every run they picked up the new header while libdyt.a kept the
# old one.  That is not a build error but a silent ABI mismatch: adding one
# field to dyt_snapshot_t made view_model.o read every later field at the old
# offset, so view_model_test failed with nonsense like "got 0.0 C want 273.1 K"
# and the same class of staleness could just as easily have made it *pass* for
# the wrong reason.  `make check` is the project's gate, so it has to mean
# something after a header edit.
#
# Coarse on purpose: every object depends on every header.  The tree has ~20 of
# them and a full rebuild is seconds, so per-header tracking would buy nothing
# but a dependency file to maintain.
# ---------------------------------------------------------------------------
HDRS := $(wildcard src/*.h) $(wildcard tools/*.h) $(wildcard gui/*.h)

LIB      := $(BUILD)/libdyt.a
LIB_OBJS := $(BUILD)/thermometry.o $(BUILD)/frame.o \
            $(BUILD)/control.o $(BUILD)/serial.o $(BUILD)/calib.o \
            $(BUILD)/capture.o $(BUILD)/palette.o $(BUILD)/imgwrite.o \
            $(BUILD)/units.o $(BUILD)/display.o $(BUILD)/measure.o \
            $(BUILD)/alarm.o $(BUILD)/session.o $(BUILD)/params.o \
            $(BUILD)/visible.o $(BUILD)/fusion.o $(BUILD)/session_capture.o \
            $(BUILD)/jpeg.o $(BUILD)/stb_impl.o $(BUILD)/dytjpeg.o \
            $(BUILD)/mnn.o $(BUILD)/sr.o $(BUILD)/view_model.o \
            $(BUILD)/frame_ready.o

# The MNN runtime object belongs *in* the archive, beside mnn.o.  mnn.o is a
# member of libdyt.a and, when a runtime is present, references
# dyt_mnn_runtime_*; without mnn_runtime.o the archive is not self-consistent
# and any consumer that calls the seam and links libdyt.a alone fails with
# undefined references.  Archive semantics keep this cheap: a member is only
# extracted when something references it, so a front end that never touches the
# seam (dytrec, dytview) still links no MNN — verified by there being no
# dyt_mnn symbol and no libMNN NEEDED entry in those binaries.
#
# dytqt is the exception: it installs the upscaler at start-up, so it references
# the seam and does link libMNN.  That is why its rule carries $(MNN_LIBS).
ifdef HAVE_MNN
LIB_OBJS += $(BUILD)/mnn_runtime.o
endif

TESTS := $(BUILD)/control_test $(BUILD)/serial_test $(BUILD)/frame_test \
         $(BUILD)/calib_test $(BUILD)/pipeline_test $(BUILD)/palette_test \
         $(BUILD)/units_test $(BUILD)/display_test $(BUILD)/measure_test \
         $(BUILD)/alarm_test $(BUILD)/session_test $(BUILD)/params_test \
         $(BUILD)/visible_test $(BUILD)/fusion_test $(BUILD)/jpeg_test \
         $(BUILD)/dytjpeg_test $(BUILD)/imgwrite_test $(BUILD)/mnn_test \
         $(BUILD)/sr_test $(BUILD)/view_model_test $(BUILD)/frame_ready_test \
         $(BUILD)/poll_model_test
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

# The mp4 recorder (Phase 7) needs only OpenCV: its fixture and --selftest paths
# are device-free, so it is useful — and testable — on a host with no libusb.
# Live recording additionally needs libuvc, which is linked only when present.
#
# player_test is the reader's gate and needs only OpenCV too — it round-trips a
# synthetic frame through the writer and back, so it is device-free.
ifdef HAVE_OPENCV
TOOLS += $(BUILD)/dytrec $(BUILD)/player_test
endif

# The mp4 writer both dytrec and the Qt app's `v` key drive.  It needs OpenCV,
# so it exists only when OpenCV does; when it does not, both tools are built
# without it and the app says so rather than failing at the first frame.
REC_OBJ :=
ifdef HAVE_OPENCV
REC_OBJ := $(BUILD)/recorder.o
endif

# The mp4 reader, the writer's mirror: the Qt app's gallery plays clips through
# it.  OpenCV-gated like the writer, so a build without OpenCV has neither and
# the gallery says clips need OpenCV rather than failing to link.  player_test
# links both objects to pin them as a symmetric pair.
PLAY_OBJ :=
ifdef HAVE_OPENCV
PLAY_OBJ := $(BUILD)/player.o
endif

# The Qt6 GUI shell.  Like dytrec, its fixture and --selftest paths are
# device-free, so it is useful on a host with no libusb; live capture goes
# through the same tools/frame_source.c and so needs libuvc only when present.
ifdef HAVE_QT6
TOOLS += $(BUILD)/dytqt
endif

# libuvc, for the targets that can actually reach the capture layer.
#
# It is present only when libusb is: without it src/capture.c compiles to its
# declining stubs (capture.c:790) and nothing references the vendored archive.
# Three targets need it for three different reasons — dytrec and dytqt link
# the live frame source, and poll_model_test calls the session_capture adapter,
# which references dyt_capture_plane_geometry/last_raw and so drags capture.o
# in on the host's libusb build.
UVC_LINK :=
ifdef HAVE_LIBUSB
UVC_LINK := $(UVC_LIB)
endif

# Frozen vendor ground truth, produced by tools/thermometry_diff/run.sh
# from the real libthermometry.so.  dumpframe --check diffs against these.
FROZEN := $(addprefix tools/thermometry_diff/out/, \
            240 240_fixon 256 256_fixon 384 384_fixon 640 640_fixon)

.PHONY: all check clean install uninstall deb rpm

# libuvc is only needed by the live-capture tools; the byte-verified
# pipeline and its tests build with just a C compiler, so $(UVC_LIB) is
# pulled in transitively via capture_demo (see below) and not listed here.
all: $(LIB) $(TESTS) $(TOOLS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c $(HDRS) $(FLAG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/libuvc/%.o: $(UVC_DIR)/src/%.c $(FLAG_STAMP) | $(BUILD)
	@mkdir -p $(BUILD)/libuvc
	$(CC) $(UVC_CFLAGS) -c $< -o $@

# The stb implementations (vendored, built -w).  This explicit rule must come
# before nothing in particular — it simply overrides the generic src/%.c rule,
# which would otherwise compile it with the port's strict CFLAGS and without
# the stb include path.
$(BUILD)/stb_impl.o: src/stb_impl.c $(HDRS) $(FLAG_STAMP) | $(BUILD)
	$(CC) $(STB_CFLAGS) -c $< -o $@

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

# units_test covers the °C/°F/K conversion the vendor app's temp_unit setting
# exposes (RE Docs 06 §3.2).  Pure — no hardware.
$(BUILD)/units_test: src/units_test.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

# display_test covers the display-range, palette-selection, mirror/zoom and
# start-up-filler rules that used to live inside tools/dytview.cpp.
$(BUILD)/display_test: src/display_test.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

# measure_test drives the point/ROI/line/isotherm geometry over a synthetic
# ramp, so every expected statistic is checkable by hand.  No hardware.
$(BUILD)/measure_test: src/measure_test.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

# alarm_test covers the high/low Schmitt triggers: the in-band non-clear and
# the on-threshold non-trip, which are what make an alarm usable.  No hardware.
$(BUILD)/alarm_test: src/alarm_test.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

# session_test drives the device-free model object with synthetic frames —
# including a two-thread torn-read check.  No hardware, no libuvc.
$(BUILD)/session_test: src/session_test.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

# view_model_test pins the toolkit-independent layer lifted out of
# tools/dytview.cpp: the status and readout wording, the colour-bar ticks, the
# device panel's rows, the runtime-parameter arm/confirm machine, and the
# pointer-to-tool mapping.  It exists so the OpenCV viewer and the Qt6 app
# cannot drift apart, so the assertions are exact strings and exact
# transitions.  No hardware, no window.
$(BUILD)/view_model_test: src/view_model_test.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

# frame_ready_test pins the coalescing wakeup: that N pings with nobody
# listening arm exactly one wakeup, that the timed wait rides CLOCK_MONOTONIC
# and does not expire early, that a blocking wait is actually woken, and that
# the arm/consume bookkeeping stays exact when a producer outruns a consumer.
# No hardware, no toolkit.
$(BUILD)/frame_ready_test: src/frame_ready_test.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

# params_test pins the device's stored-parameter encodings against the
# literals in MechaniscoutPcap/4.pcapng and the live read-back.  No hardware.
$(BUILD)/params_test: src/params_test.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

# visible_test covers the YUYV luma extraction against the frozen dual-half
# fixture (testdata/) and its own synthetic planes.  No hardware.
$(BUILD)/visible_test: src/visible_test.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

# fusion_test drives all six patterns over synthetic grey + thermal planes
# and writes a golden PPM per pattern (testdata/fusion/).  No hardware.
$(BUILD)/fusion_test: src/fusion_test.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

# jpeg_test round-trips a synthetic image through the selected codec backend.
# JPEG is lossy, so it pins structure (SOI/EOI, dimensions) and a tolerance,
# not bytes; the container's exact bytes are dytjpeg_test's job.  No hardware.
$(BUILD)/jpeg_test: src/jpeg_test.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

# dytjpeg_test checks the DYT container's bytes with its own segment walker —
# it needs no codec, only the frozen dual-half frame's payload as raw data.
# See testdata/README.md.
$(BUILD)/dytjpeg_test: src/dytjpeg_test.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

# imgwrite_test covers the PPM writer on its own (it was only ever exercised
# indirectly, through the golden comparisons) and the PNG writer structurally.
$(BUILD)/imgwrite_test: src/imgwrite_test.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

# mnn_test pins the Phase 8 super-resolution seam: the shape contract, the
# model validator, argument checking, and — when a runtime is present — a real
# upscale compared against the vendor's own mnn_run_2 output frozen in
# tools/mnn_diff/out/ (provenance in its meta.txt).  It needs the extracted
# model at models/zoom2.mnn, so run it from the port root.
#
# The test itself is C, but the runtime half is C++, so the link goes through
# $(CXX) whenever MNN is present.  mnn_runtime.o comes from $(LIB) — it is a
# member of the archive — so the link only adds the MNN library itself.
ifdef HAVE_MNN
$(BUILD)/mnn_runtime.o: src/mnn_runtime.cpp $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(MNN_CFLAGS) -c $< -o $@

# The build-tree half of the $ORIGIN rpath: the loader expands $ORIGIN to the
# binary's own directory, so libMNN.so has to be *there* for a build-tree run to
# start at all.  A symlink rather than a copy keeps the 3.3 MB library out of
# the build tree and means rebuilding MNN does not need a re-copy.  The path is
# quoted for the same reason the rpath is — the tree's path has a space.
$(BUILD)/libMNN.so: $(MNN_ROOT)/lib/libMNN.so | $(BUILD)
	ln -sf "$(abspath $(MNN_ROOT)/lib/libMNN.so)" $@
endif

$(BUILD)/mnn_test.o: src/mnn_test.c $(HDRS) $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/mnn_test: $(BUILD)/mnn_test.o $(LIB) $(MNN_DEP) | $(BUILD)
	$(CXX) $(BUILD)/mnn_test.o $(LIB) $(LDLIBS) $(MNN_LIBS) -o $@

# sr_test covers the super-resolution *policy* — which 8 bits each mode feeds
# the model, that the thermal mode's grey is the palette's own normalisation,
# the model's byte packing, and the nearest-2x of the channel the model did not
# upscale.  All pure: no model, no runtime, no device.  The model call itself
# is mnn_test's job.
$(BUILD)/sr_test: src/sr_test.c $(LIB) | $(BUILD)
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

# The mp4 recorder (Phase 7).  tools/frame_source.c is C, so it gets its own
# object rather than being fed to the C++ compiler — it allocates with malloc
# and relies on C's implicit void* conversions.  Its header lives in tools/, so
# the generic src/%.c rule does not cover it.
$(BUILD)/frame_source.o: tools/frame_source.c tools/frame_source.h $(HDRS) $(FLAG_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# The live hand-off, end to end: session_capture.c's adapter on the producer
# thread and frame_source.c's live path on the consumer, which is exactly the
# two-thread shape the Qt6 app will have.  It uses no wakeup primitive on
# purpose — it pins what the session's existing lock already guarantees, so
# the coalescing wakeup stays an optimisation rather than a load-bearing
# assumption.
#
# It lives in tools/ beside frame_source.c because it links frame_source.o,
# and a src/ test must not depend on a tools/ object (src/ is the library;
# tools/ consumes it).  It needs no hardware, but it does link libuvc on a
# host with libusb — see the $(UVC_LINK) comment above.
$(BUILD)/poll_model_test: tools/poll_model_test.c $(BUILD)/frame_source.o $(LIB) $(UVC_LINK) | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/frame_source.o $(LIB) $(UVC_LINK) $(LDLIBS) -o $@

# The shared mp4 writer.  An explicit rule because the generic pattern only
# covers src/*.c, and because it is C++ over OpenCV.
$(BUILD)/recorder.o: tools/recorder.cpp tools/recorder.h $(FLAG_STAMP) | $(BUILD)
	$(CXX) $(CXXFLAGS) -Itools -c $< -o $@

# The shared mp4 reader, beside the writer it mirrors.
$(BUILD)/player.o: tools/player.cpp tools/player.h $(FLAG_STAMP) | $(BUILD)
	$(CXX) $(CXXFLAGS) -Itools -c $< -o $@

# dytrec links only the writer: it records, and the reader is a separate seam
# with its own gate (player_test, below).
$(BUILD)/dytrec: tools/dytrec.cpp tools/recorder.h $(REC_OBJ) $(BUILD)/frame_source.o $(LIB) $(UVC_LINK) | $(BUILD)
	$(CXX) $(CXXFLAGS) -Itools $< $(REC_OBJ) $(BUILD)/frame_source.o $(LIB) $(UVC_LINK) $(LDLIBS) $(CV_LIBS) -o $@

# The reader/writer round-trip.  The test is C and drives both seams through
# their C headers, but the objects behind them are C++ over OpenCV, so it is
# compiled as C and linked with $(CXX) — the same shape as mnn_test.
$(BUILD)/player_test.o: tools/player_test.c tools/recorder.h tools/player.h $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/player_test: $(BUILD)/player_test.o $(REC_OBJ) $(PLAY_OBJ) | $(BUILD)
	$(CXX) $(BUILD)/player_test.o $(REC_OBJ) $(PLAY_OBJ) $(LDLIBS) $(CV_LIBS) -o $@

# The Qt6 GUI shell.  It links the same two pieces as dytrec — the port library
# and the frame source — plus Qt6 Widgets.  The frame source is the seam that
# keeps the app testable: pointed at a fixture it is device-free, pointed at
# libuvc it is the live camera, and the GUI code does not change between them.
# $(UVC_LINK) is empty when libusb is absent, exactly as for dytrec.  $(REC_OBJ)
# and $(CV_LIBS) are empty when OpenCV is: the app then has no `v` key and says
# so, rather than failing at the first recorded frame.  $(PLAY_OBJ) is the same
# gate for clip playback — without OpenCV the gallery refuses a clip with a
# message naming OpenCV instead of failing to link.  $(MNN_LIBS) is empty when
# there is no MNN runtime: the app then reports no model at start-up and the SR
# keys refuse, which is the same shape the other optional features take.
#
# -Itools is explicit because dytrec.cpp sits beside frame_source.h and finds
# it relatively, whereas gui/dytqt.cpp does not.
$(BUILD)/dytqt: gui/dytqt.cpp tools/recorder.h tools/player.h $(REC_OBJ) $(PLAY_OBJ) $(BUILD)/frame_source.o $(LIB) $(UVC_LINK) $(MNN_DEP) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(QT_DEFS) $(QT6_CFLAGS) -Itools $< $(REC_OBJ) $(PLAY_OBJ) $(BUILD)/frame_source.o $(LIB) $(UVC_LINK) $(LDLIBS) $(CV_LIBS) $(MNN_LIBS) $(QT6_LIBS) -o $@

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
	                       tools/thermometry_diff/out/256/in_frame.bin \
	                       testdata/mode1000_256x384_default.raw
	@$(BUILD)/palette_test palettes testdata/mode1000_256x192.raw \
	                       testdata/golden/mode1000_iron_30-31.2.ppm
	@$(BUILD)/units_test
	@$(BUILD)/display_test
	@$(BUILD)/measure_test
	@$(BUILD)/alarm_test
	@$(BUILD)/session_test
	@$(BUILD)/view_model_test
	@$(BUILD)/frame_ready_test
	@$(BUILD)/poll_model_test
	@$(BUILD)/params_test
	@$(BUILD)/visible_test testdata/mode1000_256x384_default.raw
	@$(BUILD)/fusion_test
	@$(BUILD)/jpeg_test
	@$(BUILD)/dytjpeg_test testdata/mode1000_256x384_default.raw
	@$(BUILD)/imgwrite_test
	@$(BUILD)/mnn_test models/zoom2.mnn tools/mnn_diff/out
	@$(BUILD)/sr_test
ifdef HAVE_LIBUSB
	@echo "== probe safety self-test =="
	@$(BUILD)/probe --selftest
endif
ifdef HAVE_OPENCV
	@echo "== mp4 recorder self-test =="
	@$(BUILD)/dytrec --selftest
	@echo "== mp4 reader/writer round-trip =="
	@$(BUILD)/player_test
endif
ifdef HAVE_QT6
	@echo "== Qt6 GUI self-test =="
	@$(BUILD)/dytqt --selftest
endif
	@sh packaging/check.sh
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

# ---------------------------------------------------------------------------
# Packaging: install and .deb
#
# The desktop entry and icon live in packaging/ and are checked against the app
# by packaging/check.sh, which `make check` runs.  `make install` lays the
# freedesktop files out under DESTDIR/PREFIX; `make deb` wraps that same layout
# in a Debian package, so the two cannot drift — the deb installs exactly what
# `make install` does, plus the control metadata.
#
# The build host is Fedora, so the deb is a cross-format artifact: dpkg-deb
# builds and inspects it, but nothing here installs or runs it.  Depends is
# measured from the binary with dpkg-shlibdeps when dpkg-dev is present, and
# otherwise falls back to DEB_DEPENDS below; the recipe says which it used, so a
# guess is never mistaken for a measurement.
# ---------------------------------------------------------------------------
PREFIX  ?= /usr/local
DESTDIR ?=
BINDIR   = $(PREFIX)/bin
LIBDIR   = $(PREFIX)/lib
DATADIR  = $(PREFIX)/share
APPDIR   = $(DATADIR)/applications
ICONDIR  = $(DATADIR)/icons/hicolor

# The vendor palettes ship with the app: the engine falls back to six built-in
# ramps when it cannot find them, so without this an installed dytqt would
# silently offer 6 of the 28.  dyt_vm_find_data_dir() looks for them under
# <datadir>/dytqt/palettes in each $XDG_DATA_DIRS entry, which is this location
# for both /usr and /usr/local.
PALETTEDIR = $(DATADIR)/dytqt/palettes

# The super-resolution model, found by dyt_vm_find_model() under
# <datadir>/dytqt/models — the same search the palettes use, and the same
# location for both prefixes.  It is inert without a runtime (dyt_mnn_load
# refuses), but it is 13 KB and shipping it means a build that gains MNN needs
# no second install step.
MODELDIR = $(DATADIR)/dytqt/models

# The shipped MNN runtime.  It goes one directory *up* from $(BINDIR) and into
# its own subdirectory rather than $(LIBDIR) itself, because it is a private
# dependency of this app and not something another program should pick up by
# accident.  That relative layout is what the binary's
# -rpath '$ORIGIN/../lib/dytqt' expects (see MNN_RPATH); the two are a pair and
# must not be changed one without the other.
MNNDIR = $(LIBDIR)/dytqt

# The hicolor sizes this icon is installed at.  The SVG alone is not enough:
# several desktops still do not read scalable icons, so the Makefile rasterises
# it with ImageMagick at install time and ships no PNG in the source tree.
ICON_SIZES := 16 24 32 48 64 128 256
ICON_TOOL  := $(shell command -v magick 2>/dev/null || command -v convert 2>/dev/null)

DEB_NAME       := dytqt
DEB_ARCH       := $(shell dpkg --print-architecture 2>/dev/null || echo unknown)
DEB_MAINTAINER ?= dytqt port <noreply@example.invalid>

# The fallback dependency list, used only when dpkg-shlibdeps cannot measure it.
# The OpenCV runtime packages carry the upstream soname (libopencv-core406 on
# Debian bookworm, ...410 on trixie), so this default names one release; set
# DEB_DEPENDS for your target.  The Qt6 and libusb names are unversioned.
#
# libMNN is deliberately absent, and needs no entry: it ships *inside* this
# package (at $(MNNDIR)) and its own NEEDED entries are libstdc++/libm/
# libgcc_s/libc, which are already here.  dpkg-shlibdeps cannot know that — it
# resolves the soname against the installed package database and finds nothing —
# so the measurement runs with --ignore-missing-info and simply omits it.  The
# alternative, a fabricated shlibs file for libMNN, would be a lie about a
# library no Debian package provides.
DEB_DEPENDS ?= libc6, libstdc++6, libgcc-s1, libusb-1.0-0, \
	libopencv-core406, libopencv-imgproc406, libopencv-highgui406, \
	libopencv-videoio406, libopencv-imgcodecs406, \
	libqt6widgets6, libqt6gui6, libqt6core6

DEB_ROOT := $(BUILD)/deb/$(DEB_NAME)_$(VERSION)_$(DEB_ARCH)
DEB_FILE := $(BUILD)/$(DEB_NAME)_$(VERSION)_$(DEB_ARCH).deb

install: $(BUILD)/dytqt
	@test -n "$(ICON_TOOL)" || { \
	    echo "install: ImageMagick (magick or convert) is needed for the icon" >&2; \
	    exit 1; }
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 0755 $(BUILD)/dytqt "$(DESTDIR)$(BINDIR)/dytqt"
	install -d "$(DESTDIR)$(APPDIR)"
	install -m 0644 packaging/dytqt.desktop "$(DESTDIR)$(APPDIR)/dytqt.desktop"
	install -d "$(DESTDIR)$(PALETTEDIR)"
	install -m 0644 palettes/*.dat "$(DESTDIR)$(PALETTEDIR)/"
	install -d "$(DESTDIR)$(MODELDIR)"
	install -m 0644 models/zoom2.mnn "$(DESTDIR)$(MODELDIR)/zoom2.mnn"
# The runtime is shipped only when the binary was built against one: without it
# the binary has no libMNN.so NEEDED entry, so a copy would be dead weight and
# the note below says why it is absent.  install(1) rather than a symlink,
# because the .deb must carry the library itself.
ifdef HAVE_MNN
	install -d "$(DESTDIR)$(MNNDIR)"
	install -m 0755 "$(MNN_ROOT)/lib/libMNN.so" "$(DESTDIR)$(MNNDIR)/libMNN.so"
else
	@echo "install: no MNN runtime in this build; super-resolution stays off"
endif
	@for s in $(ICON_SIZES); do \
	    install -d "$(DESTDIR)$(ICONDIR)/$${s}x$${s}/apps"; \
	    $(ICON_TOOL) -background none packaging/dytqt.svg -resize $${s}x$${s} \
	        "PNG32:$(DESTDIR)$(ICONDIR)/$${s}x$${s}/apps/dytqt.png" || exit 1; \
	done
	@echo "installed dytqt $(VERSION) to $(DESTDIR)$(PREFIX)"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/dytqt"
	rm -f "$(DESTDIR)$(APPDIR)/dytqt.desktop"
	rm -rf "$(DESTDIR)$(PALETTEDIR)"
	rm -rf "$(DESTDIR)$(MODELDIR)"
# Only the app's own subdirectory: $(LIBDIR) may hold libraries this package did
# not put there, so it must never be removed wholesale.
	rm -rf "$(DESTDIR)$(MNNDIR)"
	@for s in $(ICON_SIZES); do \
	    rm -f "$(DESTDIR)$(ICONDIR)/$${s}x$${s}/apps/dytqt.png"; \
	done
	@echo "removed dytqt from $(DESTDIR)$(PREFIX)"

deb: $(BUILD)/dytqt packaging/deb/control.in packaging/deb/copyright
	@command -v dpkg-deb >/dev/null 2>&1 || { \
	    echo "deb: dpkg-deb is required (install dpkg)" >&2; exit 1; }
	rm -rf $(DEB_ROOT)
	install -d $(DEB_ROOT)/DEBIAN
# DESTDIR stays relative: this tree's path contains a space ("Thermal Camera"),
# and an absolute DESTDIR would be split by the sub-make.  The recipes quote
# their paths, but the make variable itself must not carry the space.
	$(MAKE) install DESTDIR=$(DEB_ROOT) PREFIX=/usr BUILD=$(BUILD)
	@dep="$(DEB_DEPENDS)"; \
	if command -v dpkg-shlibdeps >/dev/null 2>&1; then \
	    if d=$$(dpkg-shlibdeps --ignore-missing-info -O $(BUILD)/dytqt 2>/dev/null \
	            | sed -n 's/^shlibs:Depends=//p') && [ -n "$$d" ]; then \
	        dep="$$d"; echo "deb: Depends measured from the binary"; \
	    else \
	        echo "deb: Depends from DEB_DEPENDS (dpkg-shlibdeps gave nothing)"; \
	    fi; \
	else \
	    echo "deb: Depends from DEB_DEPENDS (dpkg-shlibdeps absent)"; \
	fi; \
	sed -e 's|@PACKAGE@|$(DEB_NAME)|g' \
	    -e 's|@VERSION@|$(VERSION)|g' \
	    -e 's|@ARCH@|$(DEB_ARCH)|g' \
	    -e "s|@DEPENDS@|$$dep|g" \
	    -e 's|@MAINTAINER@|$(DEB_MAINTAINER)|g' \
	    packaging/deb/control.in > $(DEB_ROOT)/DEBIAN/control; \
	chmod 0644 $(DEB_ROOT)/DEBIAN/control
	install -d $(DEB_ROOT)/usr/share/doc/$(DEB_NAME)
	install -m 0644 packaging/deb/copyright \
	    $(DEB_ROOT)/usr/share/doc/$(DEB_NAME)/copyright
# The staged copy is stripped (the build tree keeps its symbols, so a local
# `make install` stays debuggable); this is a 10x size difference.
	@command -v strip >/dev/null 2>&1 && \
	    strip $(DEB_ROOT)/usr/bin/dytqt || true
	dpkg-deb --root-owner-group --build $(DEB_ROOT) $(DEB_FILE)
	@echo "built $(DEB_FILE)"
	@dpkg-deb --info $(DEB_FILE)

# ---------------------------------------------------------------------------
# Packaging: RPM
#
# Same principle as `deb`: the RPM installs exactly what `make install` stages,
# so the two cannot drift.  `make rpm` stages into DESTDIR, then rpmbuild copies
# that stage into its own %{buildroot} and packages it — the spec compiles
# nothing (see packaging/rpm/dytqt.spec).
#
# _topdir and the stage are deliberately ABSOLUTE and space-free.  This tree's
# path contains a space and rpmbuild is not uniform about quoting macro
# expansions, so keeping every rpm-internal path free of spaces removes a class
# of failure.  A *relative* _topdir is worse than useless: %install's preamble
# runs `cd "%{builddir}"` first, so a relative %{buildroot} would resolve
# against %{_topdir}/BUILD while rpm's own file lookups used the invocation
# directory.  Hence the guard in the recipe.
# ---------------------------------------------------------------------------
RPM_NAME   := dytqt
RPM_ARCH   := $(shell rpm --eval '%{_arch}'   2>/dev/null || uname -m)
RPM_LIBDIR := $(shell rpm --eval '%{_libdir}' 2>/dev/null || echo /usr/lib64)
RPM_TOPDIR ?= $(HOME)/.cache/dytqt/rpmbuild
RPM_STAGE  ?= $(HOME)/.cache/dytqt/stage
RPM_SPEC   := packaging/rpm/dytqt.spec
# The same switch drives `make install` (which ships libMNN.so) and the spec's
# %if %{with mnn}, so the two cannot disagree about whether the runtime ships.
RPM_WITH   := $(if $(HAVE_MNN),--with mnn,--without mnn)

rpm: $(BUILD)/dytqt $(RPM_SPEC)
	@command -v rpmbuild >/dev/null 2>&1 || { \
	    echo "rpm: rpmbuild is required (dnf install rpm-build)" >&2; exit 1; }
	@case "$(RPM_TOPDIR)$(RPM_STAGE)" in \
	    *" "*) echo "rpm: RPM_TOPDIR/RPM_STAGE must not contain a space." >&2; \
	           echo "rpm: rpmbuild resolves a relative buildroot against _topdir/BUILD," >&2; \
	           echo "rpm: and a spaced path is not quoted consistently, so set both to a" >&2; \
	           echo "rpm: space-free location." >&2; exit 1 ;; \
	esac
	rm -rf "$(RPM_STAGE)" "$(RPM_TOPDIR)"
	mkdir -p "$(RPM_TOPDIR)/BUILD" "$(RPM_TOPDIR)/RPMS" \
	    "$(RPM_TOPDIR)/SOURCES" "$(RPM_TOPDIR)/SPECS" "$(RPM_TOPDIR)/SRPMS"
# An absolute DESTDIR is safe only because it has no space — a spaced absolute
# one would be split by the sub-make, which is why `deb` keeps its DESTDIR
# relative.  LIBDIR is overridden to rpm's own %{_libdir} so the shipped
# libMNN.so lands where the binary's $ORIGIN/../lib64/dytqt entry looks.
	$(MAKE) install DESTDIR=$(RPM_STAGE) PREFIX=/usr LIBDIR=$(RPM_LIBDIR) BUILD=$(BUILD)
	rpmbuild -bb $(RPM_SPEC) \
	    --define "_topdir $(RPM_TOPDIR)" \
	    --define "_stage $(RPM_STAGE)" \
	    --define "_license $(abspath ../LICENSE)" \
	    --define "_dytqt_version $(VERSION)" \
	    $(RPM_WITH)
	cp "$(RPM_TOPDIR)/RPMS/$(RPM_ARCH)/$(RPM_NAME)-$(VERSION)-"*.rpm $(BUILD)/
	@echo "built $(BUILD)/$(RPM_NAME)-$(VERSION)-*.rpm"
	@rpm -qp --qf 'name=%{NAME} version=%{VERSION}-%{RELEASE} arch=%{ARCH}\n' \
	    $(BUILD)/$(RPM_NAME)-$(VERSION)-*.rpm
