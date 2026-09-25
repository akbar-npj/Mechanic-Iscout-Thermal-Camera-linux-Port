# stb — vendored image codec

`stb_image.h` (v2.30) and `stb_image_write.h` (v1.16) from
[nothings/stb](https://github.com/nothings/stb), pinned to commit
`2c980bb59875b0d32144a71867fbdebb2f77cd20` (master, 2026-09-25).

Why it is vendored
------------------

The DYT still container (`src/dytjpeg.c`) is JPEG **plus** APP2 segments.  The
container itself needs no codec, but writing a still needs one, and `make check`
has to stay green on a bare compiler with no image libraries installed.  stb is
two self-contained headers in the public domain, which is the same trade the
project already made for `third_party/libuvc`.

`libjpeg` is an **optional accelerator** (`DYT_HAVE_LIBJPEG`, detected like
`DYT_HAVE_LIBUSB`).  It is never required.

What is used
------------

| Header | Used for | Implementation TU |
|---|---|---|
| `stb_image_write.h` | JPEG encode, PNG write | `src/stb_impl.c` |
| `stb_image.h` | JPEG decode | `src/stb_impl.c` |

Only the JPEG and PNG paths are used; the rest of stb is compiled but never
called.  The implementation is instantiated in exactly one translation unit
(`src/stb_impl.c`), which is built with `-w` so upstream warnings do not drown
out the port's — the same rule the vendored libuvc follows.

Do not patch these files.  To update, re-download and update the commit above.
