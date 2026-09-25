#!/bin/sh
#
# run.sh — build and run the MNN differential against the vendor's own
# super-resolution path.
#
# This is the Phase 8 cross-check.  The port's seam (src/mnn.c) runs the model
# recovered from libmnnmodel.so; here the vendor's *own* implementation runs on
# the same host and the two are compared on the same planes.
#
# It reaches the vendor code through libmnnmodel.so, whose entry points are
# plain C symbols, so there is no C++ ABI to reproduce:
#
#     init_mnn_model_module(NULL, NULL)   -- sets a flag; both args ignored
#     mnn_run_1(MNN_ZOOM_X2, key)         -- decrypt + createFromBuffer
#     mnn_run_2(MNN_ZOOM_X2, in, out)     -- per frame
#     mnn_run_3(MNN_ZOOM_X2)              -- teardown
#
# As with dytjpeg_diff, it is deliberately NOT wired into `make check`:
#   - it needs the vendor .so files from the APK (not in the repository);
#   - they are arm64-v8a, so it needs an aarch64 host with glibc (they run
#     natively there — no qemu);
#   - it needs the bionic shims in tools/vendor_shim/.
# A bare build must stay green with none of that present.
#
#   usage: ./run.sh [workdir] [input.raw]
#
# With no input it regenerates the fixture plane with gen_plane.c, so
#     ./run.sh
# reproduces out/in_plane.raw and out/vendor_out.raw byte-for-byte.  That pair
# is the frozen ground truth `make check` compares the port's upscale against,
# so this is the command to re-run if the vendor libraries ever change.  See
# out/meta.txt for what agreement to expect.
#
# With an input it uses that plane instead and does not touch out/in_plane.raw.
#
set -eu

here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

VENDOR_DIR=${VENDOR_DIR:-"../../../RE Workspace/apk/lib/arm64-v8a"}
WORK=${1:-out}
IN=${2:-}

CC=${CC:-cc}
CFLAGS=${CFLAGS:--O2 -g -Wall -Wextra -Wno-unused-parameter}

mkdir -p build "$WORK"

echo "== preparing the vendor libraries =="
# All four, not just libmnnmodel.so: it NEEDs the other three.  prep_vendor_so
# also has to fix libmnnmodel.so's PT_LOAD layout (see its fix 5), which is what
# makes it loadable at all on a 16 KiB-page host.
for so in libmnnmodel.so libMNN.so libMNN_Express.so libc++_shared.so; do
    [ -f "$VENDOR_DIR/$so" ] || {
        echo "run.sh: missing $VENDOR_DIR/$so" >&2
        echo "         set VENDOR_DIR to the APK's lib/arm64-v8a directory" >&2
        exit 1
    }
    python3 ../vendor_shim/prep_vendor_so.py "$VENDOR_DIR/$so" "build/$so" \
        >/dev/null
done
echo "   prepared: libmnnmodel.so libMNN.so libMNN_Express.so libc++_shared.so"

echo "== building the bionic shims =="
../vendor_shim/build_shims.sh build/shim >/dev/null

echo "== building the harness and the plane generator =="
$CC $CFLAGS -o build/harness harness.c -ldl
$CC $CFLAGS -o build/gen_plane gen_plane.c -lm

if [ -n "$IN" ]; then
    PLANE=$IN
else
    # No input given: regenerate the fixture plane, so that this one command
    # reproduces the frozen ground truth in out/.
    echo "== generating the fixture input plane =="
    PLANE="$WORK/in_plane.raw"
    ./build/gen_plane "$PLANE"
fi

echo "== running the vendor path =="
LD_LIBRARY_PATH="build/shim:build" ./build/harness "$PLANE" "$WORK/vendor_out.raw"
