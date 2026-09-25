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

mkdir -p build "$OUTDIR"

# ---------------------------------------------------------------------------
# 1. Shim libraries.
#
# The vendor .so has DT_NEEDED entries for liblog/libm/libc/libdl/libstdc++,
# and references libc symbols through bionic's `LIBC` version node.  glibc has
# no such node, so we supply one.  The shim sources live in tools/vendor_shim/,
# shared with the DYT container differential — one copy, so the two harnesses
# cannot drift apart.
# ---------------------------------------------------------------------------
echo "== building shims =="
../vendor_shim/build_shims.sh "$here/build/shim" >/dev/null

# ---------------------------------------------------------------------------
# 2. Vendor library copy.
#
# Fixes: the NULL .init_array entries, PT_LOAD alignment for 4/16/64 KiB pages,
# and segment protections on pages two PT_LOADs share.  All three are
# loader-compatibility changes only; see tools/vendor_shim/prep_vendor_so.py.
# For libthermometry.so the last two are no-ops (it was linked 64 KiB-aligned
# with page-disjoint segments), so the copy stays byte-identical to the vendor
# artifact apart from the init array sizes.
# ---------------------------------------------------------------------------
echo "== preparing vendor .so =="
echo "   source: $VENDOR"
echo "   sha256: $(sha256sum "$VENDOR" | cut -d' ' -f1)"
python3 ../vendor_shim/prep_vendor_so.py "$VENDOR" build/libthermometry.so
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
LD_LIBRARY_PATH="build/shim:build" \
DYT_LOG_FILE="$OUTDIR/vendor.log" \
    ./build/harness "$OUTDIR" "$WIDTH" "$HEIGHT" "$RECBASE"

echo
echo "ground truth written to $OUTDIR/"
