/*
 * stb_impl.c — the single translation unit that instantiates the vendored stb
 * implementations (third_party/stb — see its README for the pin).
 *
 * Upstream code, not ours: this file is built with -w so stb's warnings cannot
 * drown out the port's, exactly as the vendored libuvc is.  Nothing else in
 * the tree defines the STB_*_IMPLEMENTATION macros, so the symbols live here
 * and only here.
 *
 * Only the memory API is used (no stdio), and only the two formats the port
 * needs: JPEG, because the DYT container is a JPEG (RE Docs 06 §2.1), and PNG,
 * for imgwrite's optional PNG output.
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
