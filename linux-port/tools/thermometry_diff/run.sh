#!/bin/sh
#
# run.sh — build and run the differential thermometry harness.
#
# Everything here is derived from static analysis of the vendor APK; no thermal
# camera hardware is required.  The point of the harness is to obtain *numerical
# ground truth* from the vendor's own compiled libthermometry.so, so that a
# portable C reimplementation can be diffed against it byte-for-byte.
#
# The host is expected to be aarch64 Linux with glibc.  The vendor library is
# arm64-v8a, so it runs natively — no qemu.
#
#   usage: ./run.sh [outdir] [width] [total_height] [rec_base]
#
# On success it writes to <outdir>:
#   in_frame.bin    the synthesised frame (raw pixels + reference band)
#   in_lut_in.bin   the LUT the vendor library produced (16384 floats)
#   out_image.bin   the vendor library's converted image
#   meta.txt        scalars, for the diff script to read
#   vendor.log      the vendor's own log output, captured via the liblog shim
#
set -eu

here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

# From linux-port/tools/thermometry_diff/ the vendor lib lives three levels up,
# inside the RE Workspace directory extracted from the APK.
VENDOR=${VENDOR:-"../../../RE Workspace/jadx/resources/lib/arm64-v8a/libthermometry.so"}
# Fall back to the copy inside the APK's own lib/ tree if the above is absent.
[ -f "$VENDOR" ] || VENDOR="../../../RE Workspace/apk/lib/arm64-v8a/libthermometry.so"

CC=${CC:-cc}
CFLAGS=${CFLAGS:--O2 -g -Wall -Wextra -Wno-unused-parameter}

OUTDIR=${1:-out/256}
WIDTH=${2:-256}
HEIGHT=${3:-196}
RECBASE=${4:-0x200}

mkdir -p build shim "$OUTDIR"

# ---------------------------------------------------------------------------
# 1. Shim libraries.
#
# The vendor .so has DT_NEEDED entries for liblog/libm/libc/libdl/libstdc++,
# and references libc symbols through bionic's `LIBC` version node.  glibc has
# no such node, so we supply one.  The same source is compiled twice to produce
# both libc.so and libm.so, because the vendor library has two verneed records
# both naming LIBC — one from each of those two DSOs.
# ---------------------------------------------------------------------------
echo "== building shims =="
$CC $CFLAGS -shared -fPIC -o shim/libc.so shim/libc.c \
    -Wl,--version-script,shim/libc.map -Wl,-soname,libc.so -ldl
$CC $CFLAGS -shared -fPIC -o shim/libm.so shim/libc.c \
    -Wl,--version-script,shim/libc.map -Wl,-soname,libm.so -ldl
$CC $CFLAGS -shared -fPIC -o shim/liblog.so shim/liblog.c
[ -e shim/libdl.so ]     || ln -sf "$(ldconfig -p | awk '/libdl\.so\.2/{print $NF; exit}')" shim/libdl.so
[ -e shim/libstdc++.so ] || ln -sf "$(ldconfig -p | awk '/libstdc\+\+\.so\.6/{print $NF; exit}')" shim/libstdc++.so

# ---------------------------------------------------------------------------
# 2. Vendor library copy.
#
# The only modification is clearing DT_INIT_ARRAYSZ / DT_FINI_ARRAYSZ, whose
# arrays hold nothing but NULLs.  bionic skips NULL slots; glibc jumps to
# address 0.  See prep_vendor_so.py for the full argument.
# ---------------------------------------------------------------------------
echo "== preparing vendor .so =="
echo "   source: $VENDOR"
echo "   sha256: $(sha256sum "$VENDOR" | cut -d' ' -f1)"
python3 prep_vendor_so.py "$VENDOR" build/libthermometry.so
echo "   patched sha256: $(sha256sum build/libthermometry.so | cut -d' ' -f1)"

# ---------------------------------------------------------------------------
# 3. Harness and probe.
# ---------------------------------------------------------------------------
echo "== building harness =="
$CC $CFLAGS -o build/harness harness.c -ldl
$CC $CFLAGS -o build/probe   probe.c   -ldl

# ---------------------------------------------------------------------------
# 4. Run.
# ---------------------------------------------------------------------------
echo "== running =="
LD_LIBRARY_PATH="shim:build" \
DYT_LOG_FILE="$OUTDIR/vendor.log" \
    ./build/harness "$OUTDIR" "$WIDTH" "$HEIGHT" "$RECBASE"

echo
echo "ground truth written to $OUTDIR/"
