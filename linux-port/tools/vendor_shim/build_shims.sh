#!/bin/sh
#
# build_shims.sh — build the shim libraries that let the vendor's Android .so
# files be dlopen'd against glibc.
#
#   usage: ./build_shims.sh [outdir]        (default: build/shim, beside this script)
#
# Four libraries are produced, because that is what the vendor .so files list in
# DT_NEEDED: libc.so, libm.so, libdl.so and liblog.so.
#
#   - libc.so and libm.so are the *same source*, compiled twice.  The vendor
#     library has two verneed records both naming LIBC — one from each DSO — so
#     both names must resolve to something that defines it.
#   - libdl.so is separate: libDYTJpegAes.so has a verneed naming LIBC against
#     libdl.so (for dl_iterate_phdr), which a symlink to the host's libdl.so.2
#     cannot satisfy.
#   - liblog.so supplies __android_log_print.
#
# libstdc++ is *not* built here: the vendor's C++ runtime only needs the
# symbols, and the host's libstdc++ provides them under the right soname, so a
# symlink is enough.
#
# See libc.c for why this is a shim rather than a patch to the vendor binary.
#
set -eu

here=$(cd "$(dirname "$0")" && pwd)
out=${1:-"$here/build/shim"}

CC=${CC:-cc}
CFLAGS=${CFLAGS:--O2 -g -Wall -Wextra -Wno-unused-parameter}

mkdir -p "$out"

build() {
    # build <soname> <source> <map>
    $CC $CFLAGS -shared -fPIC -o "$out/$1" "$2" \
        -Wl,--version-script,"$3" -Wl,-soname,"$1" -ldl
}

build libc.so "$here/libc.c" "$here/libc.map"
build libm.so "$here/libc.c" "$here/libc.map"
build libdl.so "$here/libdl.c" "$here/libdl.map"
$CC $CFLAGS -shared -fPIC -o "$out/liblog.so" "$here/liblog.c"

if [ ! -e "$out/libstdc++.so" ]; then
    host=$(ldconfig -p | awk '/libstdc\+\+\.so\.6/{print $NF; exit}')
    [ -n "$host" ] || { echo "build_shims: cannot find the host libstdc++.so.6" >&2; exit 1; }
    ln -sf "$host" "$out/libstdc++.so"
fi

echo "shims written to $out:"
ls -1 "$out"
