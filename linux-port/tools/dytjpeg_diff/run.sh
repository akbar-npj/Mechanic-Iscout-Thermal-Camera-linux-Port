#!/bin/sh
#
# run.sh — build and run the DYT container differential against the vendor's
# own libDYTJpegAes.so.
#
# This is the Phase 6 cross-check: the port's container code (src/dytjpeg.c) was
# written from the decompilation of the vendor's `D_updateData` / `D_jpegOpen`;
# here the real library runs on the host and the two implementations are
# compared on the same inputs, both directions, byte-for-byte.
#
# It is deliberately NOT wired into `make check`:
#   - it needs the vendor .so from the APK (not in the repository);
#   - the vendor libraries are arm64-v8a, so it needs an aarch64 host with glibc
#     (they run natively there — no qemu);
#   - it needs the bionic shims in tools/vendor_shim/.
# A bare build must stay green with none of that present.
#
#   usage: ./run.sh [workdir] [raw-payload-file]
#
# The default payload is synthetic and spans several APP2 chunks; pass
# ../../testdata/mode1000_256x384_default.raw to use the frozen device fixture.
#
# On success the last line is "<n> checks, 0 failures".
#
set -eu

here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

# The vendor lib lives in the APK extracted into the RE Workspace, three levels
# up from linux-port/tools/dytjpeg_diff/.
VENDOR=${VENDOR:-"../../../RE Workspace/jadx/resources/lib/arm64-v8a/libDYTJpegAes.so"}
[ -f "$VENDOR" ] || VENDOR="../../../RE Workspace/apk/lib/arm64-v8a/libDYTJpegAes.so"

WORK=${1:-out}
RAW=${2:-}

CC=${CC:-cc}
CFLAGS=${CFLAGS:--O2 -g -Wall -Wextra -Wno-unused-parameter -ffp-contract=off}
PORT_ROOT=$(cd ../.. && pwd)

mkdir -p build "$WORK"

echo "== building the port's container module =="
# Rebuilt from source rather than taken from ../../build, so this harness cannot
# pass against a stale object.
$CC $CFLAGS -I"$PORT_ROOT/src" -c "$PORT_ROOT/src/dytjpeg.c" -o build/dytjpeg.o

echo "== preparing the vendor .so =="
echo "   source: $VENDOR"
echo "   sha256: $(sha256sum "$VENDOR" | cut -d' ' -f1)"
python3 ../vendor_shim/prep_vendor_so.py "$VENDOR" build/libDYTJpegAes.so

echo "== building the shims =="
../vendor_shim/build_shims.sh build/shim >/dev/null

echo "== building the harness =="
$CC $CFLAGS -I"$PORT_ROOT/src" -o build/harness harness.c build/dytjpeg.o -ldl

echo "== running =="
# A relative raw-payload path is taken from the port root, since this script has
# already cd'd into tools/dytjpeg_diff/.
if [ -n "$RAW" ]; then
    case "$RAW" in
        /*) RAW_PATH=$RAW ;;
        *)  RAW_PATH=$PORT_ROOT/$RAW ;;
    esac
    LD_LIBRARY_PATH="build/shim:build" ./build/harness \
        ./build/libDYTJpegAes.so "$WORK" "$RAW_PATH"
else
    LD_LIBRARY_PATH="build/shim:build" ./build/harness \
        ./build/libDYTJpegAes.so "$WORK"
fi
