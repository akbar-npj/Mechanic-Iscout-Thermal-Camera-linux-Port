# third_party — vendored dependencies

| directory | what it is | tracked |
|---|---|---|
| `libuvc/` | the UVC camera library, sources vendored directly (see the note in the root `.gitignore` for why it is not a submodule) | yes |
| `stb/` | `stb_image_write.h`, for the optional PNG path in `src/imgwrite.c` | yes |
| `mnn-install/` | an MNN install prefix: `include/MNN/*.h` and `lib/libMNN.so` | **no** — build output |
| `MNN-src/`, `MNN-build/` | the upstream MNN tree and its CMake build tree | **no** — build output |

Everything except `libuvc/` and `stb/` is ignored; see the MNN section of the
root `.gitignore`. The build output is not tracked because MNN is genuinely
optional: with no install present the Makefile compiles `src/mnn.c` to a seam
that reports `DYT_MNN_UNAVAILABLE` and refuses, and every target — `make check`
included — still builds and passes.

## Producing an MNN install

`MNN_ROOT` defaults to `third_party/mnn-install` and must carry
`include/MNN/Interpreter.hpp` and `lib/libMNN.so`. The Makefile enables the
runtime (`-DDYT_HAVE_MNN`) only when both exist, so pointing `MNN_ROOT` at a
missing path is a supported way to exercise the runtime-free build.

From `linux-port/third_party`:

```sh
curl -sSL --retry 6 -o /tmp/mnn.tar.gz \
    https://codeload.github.com/alibaba/MNN/tar.gz/refs/heads/master
tar xzf /tmp/mnn.tar.gz && mv MNN-master MNN-src

# One patch is required; see below.
$EDITOR MNN-src/CMakeLists.txt

cmake -S MNN-src -B MNN-build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DMNN_BUILD_SHARED_LIBS=ON \
    -DMNN_BUILD_CONVERTER=OFF -DMNN_BUILD_TRAIN=OFF \
    -DMNN_BUILD_TOOLS=OFF -DMNN_BUILD_TEST=OFF -DMNN_BUILD_DEMO=OFF \
    -DMNN_BUILD_QUANTOOLS=OFF -DMNN_EVALUATION=OFF \
    -DMNN_ARM82=ON \
    -DCMAKE_INSTALL_PREFIX="$PWD/mnn-install"
cmake --build MNN-build -j"$(nproc)"
cmake --install MNN-build
```

The result should be a `libMNN.so` that NEEDs only `libstdc++`, `libm`,
`libgcc_s` and `libc` — no Python, no vendored C++ runtime. Check with:

```sh
readelf -d mnn-install/lib/libMNN.so | grep NEEDED
```

### How the app finds it

`make install` ships `libMNN.so` (and `models/zoom2.mnn`) with the app: the
library goes to `$(LIBDIR)/dytqt`, the model to `$(DATADIR)/dytqt/models`, and
the binary's rpath is `$ORIGIN:$ORIGIN/../lib/dytqt` — so the same build works
from `/usr` and `/usr/local`, and from the build tree, without ever naming an
absolute path that a `.deb` would then ship. In the build tree the rpath's first
entry is satisfied by a symlink the Makefile creates at `build/libMNN.so`; a
build without MNN creates neither the symlink nor the rpath, because the binary
has no `libMNN.so` NEEDED entry to satisfy.

That pairing is checked: `packaging/check.sh` (part of `make check`) fails if a
binary that links libMNN.so lacks the rpath, if the install would not ship the
library, or if the rpath carries a path into the build tree. `make install`
prints a note and skips the library when the build has no runtime.

### The one patch

MNN's `CMakeLists.txt` sets `-D__STRICT_ANSI__` for Linux:

```cmake
if(CMAKE_SYSTEM_NAME MATCHES "^Linux")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D__STRICT_ANSI__")
```

**Delete that `set(...)` line.** With GCC 16's libstdc++ the macro makes
`<type_traits>`, `<limits>`, `<bits/std_abs.h>` and friends define their
`__int128` specialisations twice — once via the `__GLIBCXX_TYPE_INT_N_0` macro
and once via the literal `__int128`, which are the same type there — and the
build dies with a wall of

```
error: redefinition of 'struct std::__is_integral_helper<__int128>'
```

Upstream adds the flag to suppress GNU extensions; nothing in MNNCore needs
that suppression, so dropping it is safe.

## Why not the vendor's `libMNN.so`?

The Android APK ships its own MNN 2.5.0, and `tools/vendor_shim/` can indeed
make it load on glibc — `tools/mnn_diff/` does exactly that to use it as a
differential oracle. It is not a good choice for the port's runtime: it is a
bionic build, it needs the whole shim, and it is not redistributable. The
upstream build above is host-native and links against the system libstdc++.

The two do not agree bit-for-bit; see `tools/mnn_diff/out/meta.txt`.
