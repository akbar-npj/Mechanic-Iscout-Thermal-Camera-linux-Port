/*
 * frame_source.c — the two frame sources.  See frame_source.h for the why.
 *
 * The fixture path is the one with any subtlety, and it is deliberately a
 * transcription of what session_capture.c does with a live frame:
 *
 *     dyt_pipeline_frame()            raw payload -> temperatures
 *     dyt_visible_extract()           top half    -> grey plane
 *     dyt_session_process()           install temperatures
 *     dyt_session_process_visible()   install the grey plane
 *
 * Keeping the order and the 10-float-header skip identical is the point: if
 * the two paths ever disagree, an offline recording stops predicting a live
 * one and `--selftest` stops meaning anything.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dytjpeg.h"       /* the still container the gallery opens */
#include "frame_source.h"
#include "thermometry.h"   /* LUT_N */
#include "visible.h"

struct dyt_frame_source {
    dyt_session_t *sess;        /* borrowed */

    int            is_fixture;
    long long      limit;       /* fixture: frames to produce; <= 0 unbounded */
    long long      produced;

    /* fixture payload, and the geometry dyt_pipeline_resolve() gave it */
    uint16_t      *raw;         /* width*total samples */
    size_t         n_samples;
    int            width;
    int            total;       /* payload rows (visible + thermal) */
    int            plane_y;     /* visible rows above the thermal plane */
    int            plane_h;     /* thermal rows */
    dyt_pipeline_t pipe;
    float         *lut;         /* LUT_N, owned by the pipeline's contract */
    float         *temps;       /* pipe.out_n floats */

    /* scratch for the extracted grey plane and the rendered frame */
    uint8_t       *grey;        /* width*plane_y bytes */
    int            grey_cap;
    uint8_t       *rgb;         /* w*h*3 bytes, grown on demand */
    int            rgb_cap;
};

/* ------------------------------------------------------------------ helpers */

static int grow(uint8_t **buf, int *cap, int need)
{
    uint8_t *grown;

    if (need <= *cap)
        return 0;
    grown = realloc(*buf, (size_t)need);
    if (!grown)
        return -1;
    *buf = grown;
    *cap = need;
    return 0;
}

/* Read a whole file.  Returns the buffer (caller frees) and its size, or NULL
 * with a message on stderr. */
static uint8_t *read_file(const char *path, size_t *size_out)
{
    FILE    *f;
    long     sz;
    uint8_t *buf;

    f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "frame_source: cannot size %s\n", path);
        fclose(f);
        return NULL;
    }
    buf = malloc(sz ? (size_t)sz : 1);
    if (!buf) {
        fprintf(stderr, "frame_source: out of memory reading %s\n", path);
        fclose(f);
        return NULL;
    }
    if (sz && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "frame_source: short read on %s\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size_out = (size_t)sz;
    return buf;
}

/* ------------------------------------------------------------------- fixture */

/* Build a source over a payload already in memory, taking ownership of `raw`
 * (freed here on failure, and by dyt_frame_source_close() otherwise).  Both
 * openers below hand it a malloc'd buffer and stop worrying about it.
 *
 * `label` names the source in a diagnostic ("<path>"), because the geometry
 * mismatch message is the only clue a caller gets about which file was wrong. */
static dyt_frame_source_t *open_payload(dyt_session_t *sess, uint16_t *raw,
                                        size_t n_samples, int width,
                                        dyt_mode_t mode, dyt_plane_t plane,
                                        float t_amb, int sensor_mode,
                                        int fix_mode, long long limit,
                                        const char *label)
{
    dyt_frame_source_t *fs;
    int                 rc;

    if (!sess || !raw || width <= 0 || n_samples == 0) {
        free(raw);
        return NULL;
    }

    fs = calloc(1, sizeof *fs);
    if (!fs) {
        free(raw);
        return NULL;
    }

    fs->sess       = sess;
    fs->is_fixture = 1;
    fs->limit      = limit;
    fs->width      = width;
    fs->raw        = raw;
    fs->n_samples  = n_samples;

    fs->lut = malloc(LUT_N * sizeof(float));
    if (!fs->lut)
        goto fail;
    dyt_pipeline_init(&fs->pipe, mode, plane, t_amb, sensor_mode, fix_mode,
                      fs->lut);

    rc = dyt_pipeline_resolve(&fs->pipe, width, fs->n_samples);
    if (rc != 0) {
        fprintf(stderr, "frame_source: %s does not resolve as %dpx mode %#x "
                        "plane %s (%zu samples)\n", label, width,
                (unsigned)mode,
                plane == DYT_PLANE_BOTTOM_HALF ? "bottom-half" : "full",
                fs->n_samples);
        goto fail;
    }

    fs->total   = fs->pipe.total;
    fs->plane_y = fs->pipe.plane_y;
    fs->plane_h = fs->pipe.plane_h;

    fs->temps = malloc((size_t)fs->pipe.out_n * sizeof(float));
    if (!fs->temps)
        goto fail;
    if (fs->plane_y > 0) {
        fs->grey_cap = width * fs->plane_y;
        fs->grey     = malloc((size_t)fs->grey_cap);
        if (!fs->grey)
            goto fail;
    }

    /* The render is always source-sized: width x plane_h, three bytes a pixel. */
    fs->rgb_cap = width * fs->plane_h * 3;
    fs->rgb     = malloc((size_t)fs->rgb_cap);
    if (!fs->rgb)
        goto fail;

    return fs;

fail:
    fprintf(stderr, "frame_source: out of memory\n");
    free(fs->rgb);
    free(fs->grey);
    free(fs->temps);
    free(fs->lut);
    free(fs->raw);
    free(fs);
    return NULL;
}

dyt_frame_source_t *dyt_frame_source_open_fixture(
    dyt_session_t *sess, const char *path, int width, dyt_mode_t mode,
    dyt_plane_t plane, float t_amb, int sensor_mode, int fix_mode,
    long long limit)
{
    uint8_t *file;
    size_t   bytes;

    if (!sess || !path || width <= 0) {
        fprintf(stderr, "frame_source: bad fixture arguments\n");
        return NULL;
    }

    file = read_file(path, &bytes);
    if (!file)
        return NULL;

    /* The payload is little-endian uint16 and must be a whole number of rows,
     * so a truncated or mis-sized file fails here rather than converting
     * garbage. */
    if (bytes == 0 || bytes % 2 || bytes % ((size_t)width * 2)) {
        fprintf(stderr, "frame_source: %s is %zu bytes — not a whole number "
                        "of %d-pixel uint16 rows\n", path, bytes, width);
        free(file);
        return NULL;
    }

    return open_payload(sess, (uint16_t *)file, bytes / 2, width, mode, plane,
                        t_amb, sensor_mode, fix_mode, limit, path);
}

/* A saved still, opened as a frame source so the gallery can show it through
 * the same pipeline as a live frame rather than a second render path.
 *
 * The container's own geometry is used when it has one.  A vendor still has
 * none, so `fallback_width` (the width the app is configured with) is used
 * instead — the payload's row count follows from its length, exactly as it does
 * for a fixture file.  The payload is replayed, not consumed: a still is one
 * frame shown for as long as the caller looks at it. */
dyt_frame_source_t *dyt_frame_source_open_still(
    dyt_session_t *sess, const char *path, int fallback_width, dyt_mode_t mode,
    dyt_plane_t plane, float t_amb, int sensor_mode, int fix_mode)
{
    uint8_t *blob = NULL, *raw = NULL;
    size_t   bl = 0, rl = 0;
    int      width = 0, ar = 0, tr = 0;
    unsigned fl = 0;

    if (!sess || !path) {
        fprintf(stderr, "frame_source: bad still arguments\n");
        return NULL;
    }

    if (dyt_dyt_read(path, &blob, &bl, &raw, &rl, NULL, NULL) != 0) {
        fprintf(stderr, "frame_source: %s is not a DYT still\n", path);
        return NULL;
    }

    /* A vendor blob has no geometry record; then the width is the caller's. */
    if (dyt_dyt_blob_geometry(blob, bl, &width, &ar, &tr, &fl) != 1 ||
        width <= 0)
        width = fallback_width;
    free(blob);

    if (width <= 0) {
        fprintf(stderr, "frame_source: %s records no width and none was given\n",
                path);
        free(raw);
        return NULL;
    }
    if (rl == 0 || rl % 2) {
        fprintf(stderr, "frame_source: %s carries no thermal payload\n", path);
        free(raw);
        return NULL;
    }

    return open_payload(sess, (uint16_t *)raw, rl / 2, width, mode, plane,
                        t_amb, sensor_mode, fix_mode, 0, path);
}

/* ---------------------------------------------------------------------- live */

dyt_frame_source_t *dyt_frame_source_open_live(dyt_session_t *sess)
{
    dyt_frame_source_t *fs;

    if (!sess)
        return NULL;

    fs = calloc(1, sizeof *fs);
    if (!fs)
        return NULL;
    fs->sess = sess;
    return fs;
}

/* --------------------------------------------------------------------- next */

static dyt_fs_status_t next_fixture(dyt_frame_source_t *fs,
                                    const uint8_t **rgb, int *w, int *h)
{
    dyt_frame_info_t fi;
    int              npix, off;

    if (fs->limit > 0 && fs->produced >= fs->limit)
        return DYT_FS_END;

    if (dyt_pipeline_frame(&fs->pipe, fs->raw, fs->n_samples, fs->temps) != 0) {
        fprintf(stderr, "frame_source: the fixture frame did not convert\n");
        return DYT_FS_ERROR;
    }

    /* Same header skip session_capture.c does: mode 0x44c prefixes 10 floats. */
    npix = fs->width * fs->plane_h;
    off  = fs->pipe.out_n - npix;
    if (npix <= 0 || off < 0) {
        fprintf(stderr, "frame_source: inconsistent pipeline output\n");
        return DYT_FS_ERROR;
    }

    fi.temps  = fs->temps + off;
    fi.width  = fs->width;
    fi.height = fs->plane_h;
    if (dyt_session_process(fs->sess, &fi) != 0)
        return DYT_FS_ERROR;

    /* Hand the session its own copy of the payload, exactly as the live path
     * does (session_capture.c): a still is written from the GUI thread long
     * after the frame callback has returned, so the session must own it.  The
     * fixture has the whole payload in hand, so this keeps fixture mode a
     * faithful stand-in for live — the still writer and the recorder are
     * exercisable without a device. */
    if (dyt_session_process_raw(fs->sess, fs->raw, (int)fs->n_samples,
                                fs->width, fs->total) != 0) {
        fprintf(stderr, "frame_source: the fixture payload was not installed\n");
        return DYT_FS_ERROR;
    }

    /* plane_y > 0 and < total is exactly "the top half is a visible picture",
     * the same test session_capture.c applies to a live payload. */
    if (fs->plane_y > 0 && fs->plane_y < fs->total) {
        if (dyt_visible_extract(fs->raw, fs->width, fs->plane_y, fs->grey) == 0)
            dyt_session_process_visible(fs->sess, fs->grey, fs->width,
                                        fs->plane_y);
    }

    /* Size from the snapshot's factor, exactly as the live path does, so a
     * super-resolved frame is accommodated rather than refused. */
    {
        dyt_snapshot_t snap;
        int            need;

        if (dyt_session_snapshot(fs->sess, &snap, NULL, 0) == 0) {
            need = snap.width * snap.xform.sr *
                   snap.height * snap.xform.sr * 3;
            if (need > 0 && grow(&fs->rgb, &fs->rgb_cap, need) != 0)
                return DYT_FS_ERROR;
        }
    }

    if (dyt_session_render_rgb(fs->sess, fs->rgb, fs->rgb_cap, w, h) != 0) {
        fprintf(stderr, "frame_source: the fixture frame did not render\n");
        return DYT_FS_ERROR;
    }

    *rgb = fs->rgb;
    fs->produced++;
    return DYT_FS_FRAME;
}

static dyt_fs_status_t next_live(dyt_frame_source_t *fs,
                                 const uint8_t **rgb, int *w, int *h)
{
    dyt_snapshot_t snap;
    int            need, rw = 0, rh = 0;

    if (dyt_session_snapshot(fs->sess, &snap, NULL, 0) != 0)
        return DYT_FS_WAIT;          /* no frame has arrived yet */

    /* The device streams a flat 0x8000 filler for the first ~6 s.  Refusing to
     * render it here is what stops a recording from opening on the filler. */
    if (!snap.ready)
        return DYT_FS_WAIT;

    /* The snapshot reports the factor the render will use — 1, or 2 with
     * super-resolution on — so the buffer is sized for the frame before it is
     * asked for, and the render never has to report -2 here. */
    need = snap.width * snap.xform.sr * snap.height * snap.xform.sr * 3;
    if (need <= 0)
        return DYT_FS_WAIT;
    if (grow(&fs->rgb, &fs->rgb_cap, need) != 0)
        return DYT_FS_ERROR;

    if (dyt_session_render_rgb(fs->sess, fs->rgb, fs->rgb_cap, &rw, &rh) != 0)
        return DYT_FS_WAIT;

    *rgb = fs->rgb;
    *w   = rw;
    *h   = rh;
    fs->produced++;
    return DYT_FS_FRAME;
}

dyt_fs_status_t dyt_frame_source_next(dyt_frame_source_t *fs,
                                      const uint8_t **rgb, int *w, int *h)
{
    if (!fs || !rgb || !w || !h)
        return DYT_FS_ERROR;

    return fs->is_fixture ? next_fixture(fs, rgb, w, h)
                          : next_live(fs, rgb, w, h);
}

long long dyt_frame_source_produced(const dyt_frame_source_t *fs)
{
    return fs ? fs->produced : 0;
}

int dyt_frame_source_temps(dyt_frame_source_t *fs, float *out, int cap)
{
    dyt_snapshot_t snap;
    int            r;

    if (!fs || !out || cap <= 0)
        return -1;
    r = dyt_session_snapshot(fs->sess, &snap, out, cap);
    if (r == -1)
        return 0;                     /* no frame yet */
    if (r != 0)
        return -1;                    /* cap too small */
    return snap.width * snap.height;
}

void dyt_frame_source_close(dyt_frame_source_t *fs)
{
    if (!fs)
        return;
    free(fs->rgb);
    free(fs->grey);
    free(fs->temps);
    free(fs->lut);
    free(fs->raw);
    free(fs);
}
