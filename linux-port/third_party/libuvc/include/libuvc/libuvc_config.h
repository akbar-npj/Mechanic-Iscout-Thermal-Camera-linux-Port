/*
 * libuvc_config.h — hand-written replacement for the header CMake would
 * normally generate from libuvc_config.h.in.
 *
 * We compile libuvc's sources directly from our own Makefile rather than
 * going through CMake, so this one generated header is supplied by hand.
 * Keep it in sync with the vendored tag (currently v0.0.8).
 *
 * LIBUVC_HAS_JPEG is deliberately left undefined: the thermal stream is
 * uncompressed raw16 and never decodes MJPEG, so frame-mjpeg.c is not
 * compiled and libjpeg is not linked.  Define it (and add frame-mjpeg.c
 * plus -ljpeg) only if the visible MJPEG stream is wanted later.
 */
#ifndef LIBUVC_CONFIG_H
#define LIBUVC_CONFIG_H

#define LIBUVC_VERSION_MAJOR 0
#define LIBUVC_VERSION_MINOR 0
#define LIBUVC_VERSION_PATCH 8
#define LIBUVC_VERSION_STR "0.0.8"
#define LIBUVC_VERSION_INT                      \
  ((LIBUVC_VERSION_MAJOR << 16) |               \
   (LIBUVC_VERSION_MINOR << 8) |                \
   (LIBUVC_VERSION_PATCH))

/** @brief Test whether libuvc is new enough */
#define LIBUVC_VERSION_GTE(major, minor, patch)                         \
  (LIBUVC_VERSION_INT >= (((major) << 16) | ((minor) << 8) | (patch)))

#endif /* LIBUVC_CONFIG_H */
