/*
 * view_model.c — see view_model.h.  No drawing, no toolkit, no device.
 */
#include "view_model.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "dytjpeg.h"
#include "jpeg.h"

/* ------------------------------------------------------------ small helpers */

/* Append to a bounded buffer, always NUL-terminating, and keep counting past
 * the end so the caller can tell how much was lost.  `n` must be >= 1. */
static void app(char *out, size_t n, size_t *off, const char *s)
{
    size_t len;

    if (!s)
        return;

    len = strlen(s);
    if (*off < n) {
        size_t room = n - 1 - *off;
        size_t cp   = len < room ? len : room;
        memcpy(out + *off, s, cp);
        out[*off + cp] = '\0';
    }
    *off += len;
}

static void appf(char *out, size_t n, size_t *off, const char *fmt, ...)
{
    char    b[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    app(out, n, off, b);
}

/* ---------------------------------------------------------------- polling */

int dyt_vm_grab(dyt_session_t *s, dyt_snapshot_t *snap, dyt_vm_scratch_t *scr)
{
    int attempt;

    if (!s || !snap || !scr)
        return 0;

    /* A one-element seed, so the buffer is never NULL: dyt_session_snapshot()
     * only takes its too-small path when temps_out is non-NULL, and an empty
     * allocation's data() may be NULL — which would silently skip the copy and
     * report success with an unsized plane. */
    if (scr->cap <= 0) {
        scr->temps = (float *)malloc(sizeof(float));
        if (!scr->temps)
            return 0;
        scr->cap = 1;
    }

    /* The session reports the new geometry on -2 precisely so this retry is
     * possible; four attempts is generous for a two-step resize. */
    for (attempt = 0; attempt < 4; attempt++) {
        long need;
        float *grown;
        int    rc = dyt_session_snapshot(s, snap, scr->temps, scr->cap);

        if (rc == 0)
            return 1;
        if (rc == -1)
            return 0;                       /* no frame yet */

        need = (long)snap->width * snap->height;
        if (need <= 0 || need > (long)(1 << 28))
            return 0;

        grown = (float *)realloc(scr->temps, (size_t)need * sizeof(float));
        if (!grown)
            return 0;
        scr->temps = grown;
        scr->cap   = (int)need;
    }
    return 0;
}

void dyt_vm_scratch_free(dyt_vm_scratch_t *scr)
{
    if (!scr)
        return;
    free(scr->temps);
    scr->temps = NULL;
    scr->cap   = 0;
}

/* ------------------------------------------------------------------ text */

int dyt_vm_temp(const dyt_snapshot_t *snap, float celsius, char *out, size_t n)
{
    if (!snap || !out || n == 0)
        return -1;

    if (dyt_temp_format(snap->unit, celsius, out, n) < 0)
        snprintf(out, n, "---");
    return 0;
}

int dyt_vm_status_line(const dyt_snapshot_t *snap, dyt_mode_t mode,
                       char *out, size_t n)
{
    size_t      off = 0;
    const char *mm;

    if (!snap || !out || n == 0)
        return -1;

    out[0] = '\0';

    mm = snap->xform.flip_h && snap->xform.flip_v ? "HV" :
         snap->xform.flip_h ? "H" : snap->xform.flip_v ? "V" : "-";

    app(out, n, &off,
        mode == DYT_MODE_44C  ? "mode 0x44c" :
        mode == DYT_MODE_1000 ? "mode 1000" : "mode ?");

    /* Fusion sits next to the palette because it is a view mode.  A pattern
     * that cannot be honoured on this frame — the AD output mode has no
     * visible half — is called out rather than left to look like it works. */
    appf(out, n, &off, " | fusion %s", snap->fusion_name);
    if (snap->fusion_dx || snap->fusion_dy)
        appf(out, n, &off, " %+d,%+d", snap->fusion_dx, snap->fusion_dy);
    if (!snap->fusion_active && snap->fusion != DYT_FUSION_INFRARED)
        app(out, n, &off, " (no visible plane)");

    appf(out, n, &off, " | %s %d/%d", snap->palette_name,
        snap->palette + 1, snap->palette_n);

    appf(out, n, &off, " | %s",
        dyt_unit_suffix(snap->unit) ? dyt_unit_suffix(snap->unit) : "?");

    appf(out, n, &off, " | x%d%s | %ld frames", snap->xform.zoom, mm,
        (long)snap->seq);

    return (int)off;
}

int dyt_vm_readout_line(const dyt_snapshot_t *snap, const char *msg,
                        char *out, size_t n)
{
    size_t off = 0;

    if (!snap || !out || n == 0)
        return -1;

    out[0] = '\0';

    switch (snap->tool) {
    case DYT_TOOL_POINT: {
        char t[32];
        dyt_vm_temp(snap, snap->point_c, t, sizeof t);
        appf(out, n, &off, "point %s", snap->point_ok ? t : "--");
        break;
    }
    case DYT_TOOL_LINE:
        appf(out, n, &off, "line (%d,%d)-(%d,%d)",
             snap->p0.x, snap->p0.y, snap->p1.x, snap->p1.y);
        break;
    case DYT_TOOL_BOX:
        appf(out, n, &off, "box (%d,%d)-(%d,%d) n=%d",
             snap->p0.x, snap->p0.y, snap->p1.x, snap->p1.y, snap->roi.n);
        break;
    case DYT_TOOL_NONE:
    default:
        app(out, n, &off, "tool: none (p point, l line, b box, n clear)");
        break;
    }

    if (snap->alarm_on) {
        appf(out, n, &off, "  |  alarm %s %.1f..%.1f",
             dyt_alarm_name(snap->alarm),
             (double)dyt_temp_convert(snap->unit, snap->alarm_lo),
             (double)dyt_temp_convert(snap->unit, snap->alarm_hi));
    }
    if (snap->iso_on)
        appf(out, n, &off, "  |  iso %ld px", snap->iso.count);
    if (msg && msg[0])
        appf(out, n, &off, "  |  %s", msg);

    return (int)off;
}

/* ------------------------------------------------------------- colour bar */

int dyt_vm_bar_index(int row, int rows)
{
    float u;
    int   idx;

    if (rows <= 0 || row < 0 || row >= rows)
        return -1;

    u   = rows > 1 ? 1.f - (float)row / (float)(rows - 1) : 1.f;
    idx = (int)(u * (DYT_PALETTE_N - 1) + 0.5f);
    if (idx < 0)
        idx = 0;
    if (idx > DYT_PALETTE_N - 1)
        idx = DYT_PALETTE_N - 1;
    return idx;
}

int dyt_vm_bar_label(const dyt_snapshot_t *snap, int which, char *out, size_t n)
{
    float c;

    if (!snap || !out || n == 0)
        return -1;

    switch (which) {
    case 0:  c = snap->hi; break;
    case 1:  c = (snap->lo + snap->hi) * 0.5f; break;
    case 2:  c = snap->lo; break;
    default: return -1;
    }
    return dyt_vm_temp(snap, c, out, n);
}

/* ----------------------------------------------------------- device panel */

/* A parameter's effective value: the runtime override if this session sent
 * one, else the value read at start-up, with a trailing `*` flagging the
 * override so the panel never silently shows a superseded stored value. */
static void pv(const float *override_v, const int *override_on,
               dyt_order_type_t type, float stored, const char *fmt,
               char *out, size_t n)
{
    int  on = override_on && override_on[type];
    char core[24];

    snprintf(core, sizeof core, fmt, (double)(on ? override_v[type] : stored));
    snprintf(out, n, "%s%s", core, on ? "*" : "");
}

int dyt_vm_info(const dyt_device_info_t *d, const float *override_v,
                const int *override_on, dyt_vm_info_t *out)
{
    int  any_over = 0, t;
    char b[DYT_VM_INFO_LINE_CAP];

    if (!d || !out)
        return -1;

    memset(out, 0, sizeof *out);

    snprintf(out->line[out->n++], DYT_VM_INFO_LINE_CAP, "serial  %s",
             d->have_sn ? d->sn_str : "(read failed)");

    if (d->have_usn) {
        if (d->usn_len >= 0) {
            snprintf(out->line[out->n++], DYT_VM_INFO_LINE_CAP,
                     "user    %s%s  (key %u)", d->usn_str,
                     d->usn_variant ? "  (variant C)" : "",
                     (unsigned)d->usn_key);
        } else {
            snprintf(out->line[out->n++], DYT_VM_INFO_LINE_CAP,
                     "user    (raw read; no key)");
        }
    }

    for (t = DYT_ORDER_REFLECTED; t <= DYT_ORDER_DISTANCE; t++)
        if (override_on && override_on[t])
            any_over = 1;

    if ((d->radio.ok & DYT_RADIO_REFLECTED) ||
        (override_on && override_on[DYT_ORDER_REFLECTED]) ||
        (d->radio.ok & DYT_RADIO_AMBIENT) ||
        (override_on && override_on[DYT_ORDER_AMBIENT])) {
        char a[32], c[32];
        pv(override_v, override_on, DYT_ORDER_REFLECTED,
           dyt_radiometry_reflected_c(&d->radio), "%.2f", a, sizeof a);
        pv(override_v, override_on, DYT_ORDER_AMBIENT,
           dyt_radiometry_ambient_c(&d->radio), "%.2f", c, sizeof c);
        snprintf(out->line[out->n++], DYT_VM_INFO_LINE_CAP,
                 "refl %s C   amb %s C", a, c);
    }

    if ((d->radio.ok & DYT_RADIO_EMISSIVITY) ||
        (override_on && override_on[DYT_ORDER_EMISSIVITY]) ||
        (d->radio.ok & DYT_RADIO_DISTANCE) ||
        (override_on && override_on[DYT_ORDER_DISTANCE])) {
        char e[32], m[32];
        pv(override_v, override_on, DYT_ORDER_EMISSIVITY,
           dyt_radiometry_emissivity(&d->radio), "%.4f", e, sizeof e);
        pv(override_v, override_on, DYT_ORDER_DISTANCE,
           dyt_radiometry_distance_m(&d->radio), "%.4f", m, sizeof m);
        snprintf(out->line[out->n++], DYT_VM_INFO_LINE_CAP,
                 "emis %s   dist %s m", e, m);
    }

    snprintf(b, sizeof b, "params %d/%d slots%s", d->params_read, DYT_PARAM_N,
             any_over ? "   (* = set this session)" : "");
    snprintf(out->line[out->n++], DYT_VM_INFO_LINE_CAP, "%s", b);

    out->any_override = any_over;
    return out->n;
}

/* -------------------------------------------------------- parameter ladder
 *
 * Short and widely spaced on purpose: this is a proving ground for "does the
 * write reach the device", not a full parameter editor.
 */
static const float kEmisVals[] = { 1.00f, 0.95f, 0.90f, 0.80f, 0.50f, 0.10f };
static const float kAmbVals[]  = { 20.f, 25.f, 30.f, 40.f, 60.f };
static const float kReflVals[] = { 20.f, 25.f, 30.f, 40.f, 100.f };
static const float kDistVals[] = { 0.10f, 0.50f, 1.00f, 2.00f, 5.00f, 10.00f };

static const dyt_vm_ladder_t kLadders[] = {
    { DYT_ORDER_EMISSIVITY, "emissivity", 6, kEmisVals },
    { DYT_ORDER_AMBIENT,    "ambient",    5, kAmbVals  },
    { DYT_ORDER_REFLECTED,  "reflected",  5, kReflVals },
    { DYT_ORDER_DISTANCE,   "distance",   6, kDistVals },
};

int dyt_vm_ladder_count(void)
{
    return (int)(sizeof kLadders / sizeof kLadders[0]);
}

const dyt_vm_ladder_t *dyt_vm_ladder_at(int i)
{
    if (i < 0 || i >= dyt_vm_ladder_count())
        return NULL;
    return &kLadders[i];
}

const dyt_vm_ladder_t *dyt_vm_ladder(dyt_order_type_t type)
{
    int i;
    for (i = 0; i < dyt_vm_ladder_count(); i++)
        if (kLadders[i].type == type)
            return &kLadders[i];
    return NULL;
}

int dyt_vm_param_format(dyt_order_type_t type, float v, char *out, size_t n)
{
    if (!out || n == 0)
        return -1;

    if (type == DYT_ORDER_EMISSIVITY)
        snprintf(out, n, "%.2f", (double)v);
    else if (type == DYT_ORDER_DISTANCE)
        snprintf(out, n, "%.2f m", (double)v);
    else
        snprintf(out, n, "%.1f C", (double)v);
    return 0;
}

int dyt_vm_param_key(int key, dyt_order_type_t armed_type, int armed_rung,
                     dyt_vm_param_event_t *ev)
{
    const dyt_vm_ladder_t *L;
    dyt_order_type_t       type;

    if (!ev)
        return 0;
    ev->action = DYT_VM_PARAM_NONE;
    ev->type   = (dyt_order_type_t)0;
    ev->value  = 0.f;
    ev->rung   = armed_rung;

    type = (key == 'e') ? DYT_ORDER_EMISSIVITY :
           (key == 'A') ? DYT_ORDER_AMBIENT    :
           (key == 'R') ? DYT_ORDER_REFLECTED  :
           (key == 'D') ? DYT_ORDER_DISTANCE   : (dyt_order_type_t)0;

    if (type) {
        L = dyt_vm_ladder(type);
        if (!L)
            return 0;
        /* Re-pressing the same key advances the candidate; a different
         * parameter's key starts its own ladder at the first rung. */
        ev->rung   = (armed_type == type) ? (armed_rung + 1) % L->n : 0;
        ev->type   = type;
        ev->value  = L->vals[ev->rung];
        ev->action = DYT_VM_PARAM_ARMED;
        return 1;
    }

    if (!armed_type)
        return 0;                           /* nothing armed: not ours */

    if (key == 'q')
        return 0;                           /* never swallow quit */

    L = dyt_vm_ladder(armed_type);
    if (!L)
        return 0;

    if (key == 'y' || key == 'Y') {
        int rung = (armed_rung >= 0 && armed_rung < L->n) ? armed_rung : 0;
        ev->action = DYT_VM_PARAM_SEND;
        ev->type   = armed_type;
        ev->value  = L->vals[rung];
        ev->rung   = rung;
        return 1;
    }

    if (key == 'n' || key == 'N' || key == 27) {
        ev->action = DYT_VM_PARAM_CANCEL;
        ev->type   = armed_type;
        return 1;
    }

    /* Any other key while armed is ignored, so a stray palette key cannot
     * slip past a pending confirmation. */
    ev->action = DYT_VM_PARAM_SWALLOW;
    ev->type   = armed_type;
    return 1;
}

/* --------------------------------------------------------- measurement UI */

int dyt_vm_tool_mouse(dyt_session_t *s, dyt_vm_pointer_t *p,
                      dyt_vm_mouse_ev_t ev, dyt_tool_t tool,
                      const dyt_view_transform_t *xform,
                      int src_w, int src_h, int dst_w, int dst_h,
                      int x, int y)
{
    int sx = 0, sy = 0, ok;

    if (!s || !p)
        return 0;

    /* Kept even when the tool places nothing: the hover readout needs the
     * pointer position on every move, not only on a drag. */
    p->x = x;
    p->y = y;

    if (tool == DYT_TOOL_NONE)
        return 0;

    if (ev == DYT_VM_MOUSE_UP) {
        p->dragging = 0;
        return 0;
    }

    ok = xform && dyt_view_transform_map(xform, src_w, src_h, dst_w, dst_h,
                                         x, y, &sx, &sy) == 0;

    if (ev == DYT_VM_MOUSE_DOWN) {
        p->dragging = 1;
        if (ok) {
            dyt_session_set_point(s, 0, sx, sy);
            dyt_session_set_point(s, 1, sx, sy);
            return 1;
        }
        return 0;
    }

    if (ev == DYT_VM_MOUSE_MOVE && p->dragging) {
        if (ok) {
            dyt_session_set_point(s, 1, sx, sy);
            return 1;
        }
    }
    return 0;
}

/* --------------------------------------------------------------- overlays */

long dyt_vm_apply_isotherm(uint8_t *bgr, int w, int h,
                           const float *temps, int temps_n,
                           float lo, float hi)
{
    long dimmed = 0;
    int  yy;

    if (!bgr || !temps || w <= 0 || h <= 0 || temps_n < w * h)
        return -1;

    for (yy = 0; yy < h; yy++) {
        uint8_t *row = bgr + (size_t)yy * (size_t)w * 3;
        int      xx;

        for (xx = 0; xx < w; xx++) {
            float t      = temps[(size_t)yy * (size_t)w + xx];
            int   inside = (t == t) && t >= lo && t <= hi;

            if (!inside) {
                row[xx * 3 + 0] = (uint8_t)(row[xx * 3 + 0] / 2);
                row[xx * 3 + 1] = (uint8_t)(row[xx * 3 + 1] / 2);
                row[xx * 3 + 2] = (uint8_t)(row[xx * 3 + 2] / 2);
                dimmed++;
            }
        }
    }
    return dimmed;
}

/* ----------------------------------------------------------------- output */

int dyt_vm_write_still(dyt_session_t *s, const char *path, char *msg, size_t n)
{
    dyt_snapshot_t  snap;
    uint8_t        *rgb = NULL;
    uint16_t       *raw = NULL;
    uint8_t        *jpg = NULL;
    size_t          jlen = 0;
    int             w = 0, h = 0, nn;
    uint8_t         blob[DYT_DYT_BLOB_SIZE];
    unsigned        flags;
    int             rc = -1;

#define FAIL(why) do { if (msg) snprintf(msg, n, "%s", (why)); goto done; } while (0)

    if (!s || !path)
        FAIL("still: bad argument");

    if (dyt_session_snapshot(s, &snap, NULL, 0) != 0 || !snap.ready)
        FAIL("still: no live frame yet");
    if (!snap.have_raw)
        FAIL("still: no raw payload");

    rgb = (uint8_t *)malloc((size_t)snap.width * snap.height * 3);
    if (!rgb)
        FAIL("still: out of memory");

    if (dyt_session_render_rgb(s, rgb, snap.width * snap.height * 3,
                               &w, &h) != 0)
        FAIL("still: render failed");

    if (dyt_jpeg_encode(rgb, w, h, 85, &jpg, &jlen) != 0)
        FAIL("still: JPEG encode failed");

    /* The geometry the still records is the *payload's*, so a reader knows how
     * to interpret the raw samples: the thermal plane is height rows of it,
     * and anything above that is the visible half. */
    flags = snap.raw_total_rows > snap.height ? DYT_DYT_FLAG_DUAL_HALF : 0u;
    if (dyt_dyt_blob_init(blob, sizeof blob, snap.width, snap.height,
                          snap.raw_total_rows, flags) != 0)
        FAIL("still: bad geometry");

    raw = (uint16_t *)malloc((size_t)snap.raw_n * sizeof(uint16_t));
    if (!raw)
        FAIL("still: out of memory");

    nn = dyt_session_raw(s, raw, snap.raw_n);
    if (nn != snap.raw_n)
        FAIL("still: raw payload changed under us");

    if (dyt_dyt_write(path, jpg, jlen, blob, sizeof blob,
                      (const uint8_t *)raw,
                      (size_t)snap.raw_n * sizeof(uint16_t)) != 0) {
        if (msg)
            snprintf(msg, n, "cannot write %s", path);
        goto done;
    }

    if (msg)
        snprintf(msg, n, "wrote %s  (%dx%d, %zu raw bytes)", path,
                 snap.width, snap.raw_total_rows,
                 (size_t)snap.raw_n * sizeof(uint16_t));
    rc = 0;

done:
    free(rgb);
    free(raw);
    free(jpg);
    return rc;
#undef FAIL
}

/* -------------------------------------------------------------- utilities */

int dyt_vm_exe_dir(char *out, size_t n)
{
    char    buf[4096];
    ssize_t len;
    char   *slash;

    if (!out || n == 0)
        return -1;
    out[0] = '\0';

    len = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (len <= 0)
        return -1;
    buf[len] = '\0';

    slash = strrchr(buf, '/');
    if (!slash)
        return -1;
    *slash = '\0';

    snprintf(out, n, "%s", buf);
    return 0;
}

int dyt_vm_find_palette_dir(const char *dir_opt, char *out, size_t n)
{
    char        cand[4096];
    char        ed[2048];
    const char *fixed[] = { "palettes", "../palettes" };
    int         i;

    if (!out || n == 0)
        return 0;
    out[0] = '\0';

    if (dir_opt && dir_opt[0]) {
        if (access(dir_opt, R_OK) == 0) {
            snprintf(out, n, "%s", dir_opt);
            return 1;
        }
    }

    for (i = 0; i < 2; i++) {
        if (access(fixed[i], R_OK) == 0) {
            snprintf(out, n, "%s", fixed[i]);
            return 1;
        }
    }

    if (dyt_vm_exe_dir(ed, sizeof ed) == 0) {
        for (i = 0; i < 2; i++) {
            snprintf(cand, sizeof cand, "%s/%s", ed, fixed[i]);
            if (access(cand, R_OK) == 0) {
                snprintf(out, n, "%s", cand);
                return 1;
            }
        }
    }
    return 0;
}

int dyt_vm_timestamp(char *out, size_t n)
{
    time_t    now;
    struct tm tm;

    if (!out || n == 0)
        return -1;

    now = time(NULL);
    if (!localtime_r(&now, &tm))
        return -1;
    if (strftime(out, n, "%Y%m%d-%H%M%S", &tm) == 0)
        return -1;
    return 0;
}
