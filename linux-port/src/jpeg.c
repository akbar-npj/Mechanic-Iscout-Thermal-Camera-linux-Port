/*
 * jpeg.c — JPEG encode/decode behind the dyt_jpeg_* interface (jpeg.h).
 *
 * Exactly one backend is compiled in.  stb is the default because it is
 * vendored (third_party/stb) and therefore always present; libjpeg is an
 * optional accelerator selected by DYT_HAVE_LIBJPEG, detected like
 * DYT_HAVE_LIBUSB.  The two backends are not expected to produce identical
 * bytes — JPEG is lossy and the encoders differ — so the container tests pin
 * the *container*, not the JPEG, and jpeg_test tolerates codec differences.
 *
 * build (stb):     cc -O2 -g -Wall -Wextra -ffp-contract=off -I. \
 *                     -Ithird_party/stb -c jpeg.c
 * build (libjpeg): same, plus -DDYT_HAVE_LIBJPEG and the libjpeg cflags.
 */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "jpeg.h"

#ifdef DYT_HAVE_LIBJPEG

/* ----------------------------------------------------------------- libjpeg -- */

#include <setjmp.h>
#include <stdio.h>

#include <jpeglib.h>

dyt_jpeg_backend_t dyt_jpeg_backend(void)      { return DYT_JPEG_LIBJPEG; }
const char        *dyt_jpeg_backend_name(void){ return "libjpeg"; }

/* libjpeg's stock error handler calls exit(); a corrupt buffer must fail the
 * call, not the process.  `pub` has to come first so the downcast below is
 * valid — that is libjpeg's documented custom-error-mgr layout. */
typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf               escape;
} jpeg_err;

static void jpeg_silent_output(j_common_ptr cinfo)
{
    (void)cinfo;                    /* do not spam stderr on bad input */
}

static void jpeg_error_exit(j_common_ptr cinfo)
{
    jpeg_err *e = (jpeg_err *)cinfo->err;
    longjmp(e->escape, 1);
}

/* Every field libjpeg writes lives on the heap, so a longjmp out of a libjpeg
 * error cannot leave a clobbered stack local behind (which is what -Wclobbered
 * is there to warn about). */
typedef struct {
    struct jpeg_compress_struct cinfo;
    jpeg_err                    err;
    unsigned char              *buf;
    unsigned long               size;
    int                         created;
} enc_ctx;

int dyt_jpeg_encode(const uint8_t *rgb, int w, int h, int quality,
                    uint8_t **out, size_t *out_len)
{
    enc_ctx *c;

    if (!rgb || !out || !out_len || w <= 0 || h <= 0)
        return -1;
    if (quality < 1)   quality = 1;
    if (quality > 100) quality = 100;

    c = calloc(1, sizeof *c);
    if (!c)
        return -1;

    c->cinfo.err            = jpeg_std_error(&c->err.pub);
    c->err.pub.error_exit   = jpeg_error_exit;
    c->err.pub.output_message = jpeg_silent_output;

    if (setjmp(c->err.escape)) {
        if (c->created) jpeg_destroy_compress(&c->cinfo);
        free(c->buf);
        free(c);
        return -1;
    }

    jpeg_create_compress(&c->cinfo);
    c->created = 1;
    jpeg_mem_dest(&c->cinfo, &c->buf, &c->size);

    c->cinfo.image_width      = (JDIMENSION)w;
    c->cinfo.image_height     = (JDIMENSION)h;
    c->cinfo.input_components = 3;
    c->cinfo.in_color_space   = JCS_RGB;

    jpeg_set_defaults(&c->cinfo);
    jpeg_set_quality(&c->cinfo, quality, TRUE);
    jpeg_start_compress(&c->cinfo, TRUE);

    while (c->cinfo.next_scanline < c->cinfo.image_height) {
        JSAMPROW row = (JSAMPROW)(rgb + (size_t)c->cinfo.next_scanline *
                                        (size_t)w * 3);
        jpeg_write_scanlines(&c->cinfo, &row, 1);
    }

    jpeg_finish_compress(&c->cinfo);
    jpeg_destroy_compress(&c->cinfo);
    c->created = 0;

    if (!c->buf || c->size == 0) {
        free(c->buf);
        free(c);
        return -1;
    }

    *out     = c->buf;
    *out_len = (size_t)c->size;
    free(c);
    return 0;
}

typedef struct {
    struct jpeg_decompress_struct cinfo;
    jpeg_err                      err;
    uint8_t                      *out;
    int                           created;
} dec_ctx;

int dyt_jpeg_decode(const uint8_t *jpeg, size_t len,
                    uint8_t **rgb, int *w, int *h)
{
    dec_ctx *c;
    int ww, hh;

    if (!jpeg || !rgb || !w || !h || len == 0)
        return -1;

    c = calloc(1, sizeof *c);
    if (!c)
        return -1;

    c->cinfo.err              = jpeg_std_error(&c->err.pub);
    c->err.pub.error_exit     = jpeg_error_exit;
    c->err.pub.output_message = jpeg_silent_output;

    if (setjmp(c->err.escape)) {
        if (c->created) jpeg_destroy_decompress(&c->cinfo);
        free(c->out);
        free(c);
        return -1;
    }

    jpeg_create_decompress(&c->cinfo);
    c->created = 1;
    jpeg_mem_src(&c->cinfo, jpeg, (unsigned long)len);
    jpeg_read_header(&c->cinfo, TRUE);

    c->cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&c->cinfo);

    ww = (int)c->cinfo.output_width;
    hh = (int)c->cinfo.output_height;
    if (ww <= 0 || hh <= 0 || c->cinfo.output_components != 3) {
        jpeg_destroy_decompress(&c->cinfo);
        free(c);
        return -1;
    }

    c->out = malloc((size_t)ww * (size_t)hh * 3);
    if (!c->out) {
        jpeg_destroy_decompress(&c->cinfo);
        free(c);
        return -1;
    }

    while (c->cinfo.output_scanline < c->cinfo.output_height) {
        JSAMPROW row = (JSAMPROW)(c->out +
                                  (size_t)c->cinfo.output_scanline *
                                  (size_t)ww * 3);
        jpeg_read_scanlines(&c->cinfo, &row, 1);
    }

    jpeg_finish_decompress(&c->cinfo);
    jpeg_destroy_decompress(&c->cinfo);
    c->created = 0;

    *rgb = c->out;
    *w   = ww;
    *h   = hh;
    free(c);
    return 0;
}

#else /* vendored stb */

/* --------------------------------------------------------------------- stb -- */

#include "stb_image.h"
#include "stb_image_write.h"

dyt_jpeg_backend_t dyt_jpeg_backend(void)      { return DYT_JPEG_STB; }
const char        *dyt_jpeg_backend_name(void){ return "stb"; }

/* stb writes through a callback; collect the bytes in a growable buffer. */
typedef struct {
    uint8_t *p;
    size_t   n, cap;
    int      oom;
} membuf;

static void mem_write(void *ctx, void *data, int size)
{
    membuf *m = ctx;
    size_t  need;

    if (size <= 0 || m->oom)
        return;

    need = m->n + (size_t)size;
    if (need > m->cap) {
        size_t   cap = m->cap ? m->cap : 65536;
        uint8_t *p;

        while (cap < need) cap *= 2;
        p = realloc(m->p, cap);
        if (!p) { m->oom = 1; return; }
        m->p   = p;
        m->cap = cap;
    }
    memcpy(m->p + m->n, data, (size_t)size);
    m->n += (size_t)size;
}

int dyt_jpeg_encode(const uint8_t *rgb, int w, int h, int quality,
                    uint8_t **out, size_t *out_len)
{
    membuf m;
    int    ok;

    if (!rgb || !out || !out_len || w <= 0 || h <= 0)
        return -1;
    if (quality < 1)   quality = 1;
    if (quality > 100) quality = 100;

    m.p = NULL; m.n = 0; m.cap = 0; m.oom = 0;

    ok = stbi_write_jpg_to_func(mem_write, &m, w, h, 3, rgb, quality);
    if (!ok || m.oom || !m.p) {
        free(m.p);
        return -1;
    }

    *out     = m.p;
    *out_len = m.n;
    return 0;
}

int dyt_jpeg_decode(const uint8_t *jpeg, size_t len,
                    uint8_t **rgb, int *w, int *h)
{
    int      ww, hh, comp;
    uint8_t *p;

    if (!jpeg || !rgb || !w || !h || len == 0)
        return -1;
    if (len > (size_t)INT_MAX)
        return -1;

    p = stbi_load_from_memory(jpeg, (int)len, &ww, &hh, &comp, 3);
    if (!p)
        return -1;
    if (ww <= 0 || hh <= 0) {
        stbi_image_free(p);
        return -1;
    }

    *rgb = p;
    *w   = ww;
    *h   = hh;
    return 0;
}

#endif /* DYT_HAVE_LIBJPEG */
