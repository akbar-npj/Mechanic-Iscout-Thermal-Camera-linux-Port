/*
 * view_model.c — see view_model.h.  No drawing, no toolkit, no device.
 */
#include "view_model.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
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

    /* Super-resolution, shown only when it is on: it is an addition to what is
     * displayed rather than a mode of its own, so an "sr:off" on every line
     * would be noise.  A mode that cannot be honoured on this frame — the
     * visible plane is not on screen, or there is no visible half at all — is
     * called out for the same reason the fusion pattern is. */
    if (snap->sr != DYT_SR_OFF) {
        appf(out, n, &off, " | sr:%s", snap->sr_name);
        if (snap->sr_active)
            appf(out, n, &off, " x%d", snap->xform.sr);
        else
            app(out, n, &off, " (inactive)");
    }

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
    case DYT_TOOL_POLYGON:
        /* The closed flag is worth showing: it is the difference between "the
         * next click extends this" and "the next click starts over". */
        appf(out, n, &off, "polygon %d pts%s n=%d",
             snap->poly_n, snap->poly_closed ? " (closed)" : "",
             snap->roi.n);
        break;
    case DYT_TOOL_NONE:
    default:
        app(out, n, &off,
            "tool: none (p point, l line, b box, o polygon, n clear)");
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

/* One ROI statistic, or "--" when the region had no finite samples
 * (measure.h) — never a plausible-looking 0 C. */
static void stat_str(const dyt_snapshot_t *snap, float c, char *out, size_t n)
{
    if (c == c)
        dyt_vm_temp(snap, c, out, n);
    else
        snprintf(out, n, "--");
}

int dyt_vm_hover_label(const dyt_snapshot_t *snap, float celsius,
                       int x, int y, char *out, size_t n)
{
    char   t[32];
    size_t off = 0;

    if (!snap || !out || n == 0)
        return -1;

    out[0] = '\0';

    if (celsius == celsius)
        dyt_vm_temp(snap, celsius, t, sizeof t);
    else
        snprintf(t, sizeof t, "---");

    appf(out, n, &off, "%s  (%d,%d)", t, x, y);
    return (int)off;
}

int dyt_vm_roi_label(const dyt_snapshot_t *snap, char *out, size_t n)
{
    char   a[32], b[32], c[32], d[32];
    size_t off = 0;

    if (!snap || !out || n == 0)
        return -1;

    out[0] = '\0';

    stat_str(snap, snap->roi.min,    a, sizeof a);
    stat_str(snap, snap->roi.max,    b, sizeof b);
    stat_str(snap, snap->roi.mean,   c, sizeof c);
    stat_str(snap, snap->roi.median, d, sizeof d);

    appf(out, n, &off, "min %s  max %s  avg %s  med %s", a, b, c, d);
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

float dyt_vm_bar_frac(float c, float lo, float hi)
{
    float span = hi - lo;
    float u;

    if (!(span > 0.0f))          /* flat or inverted: the middle, not a divide */
        return 0.5f;

    u = (c - lo) / span;
    if (u < 0.0f)
        u = 0.0f;
    if (u > 1.0f)
        u = 1.0f;
    return u;
}

float dyt_vm_bar_temp(float frac, float lo, float hi)
{
    if (frac < 0.0f)
        frac = 0.0f;
    if (frac > 1.0f)
        frac = 1.0f;
    return lo + frac * (hi - lo);
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
        if (!ok)
            return 0;
        /* The polygon is placed a click at a time, not dragged: each press
         * adds a vertex, and the shape is measured as soon as it has three.
         * A press after the outline was closed begins a new one (the session
         * decides that, so both front ends get the same gesture). */
        if (tool == DYT_TOOL_POLYGON)
            return dyt_session_polygon_add(s, sx, sy) == 0;
        dyt_session_set_point(s, 0, sx, sy);
        dyt_session_set_point(s, 1, sx, sy);
        return 1;
    }

    /* Only the drag tools move point 1: for the polygon a move has nothing to
     * place, and the front end draws its own rubber band to the cursor. */
    if (ev == DYT_VM_MOUSE_MOVE && p->dragging && tool != DYT_TOOL_POLYGON) {
        if (ok) {
            dyt_session_set_point(s, 1, sx, sy);
            return 1;
        }
    }
    return 0;
}

int dyt_vm_alarm_band(const dyt_snapshot_t *snap,
                      float *lo, float *hi, float *hyst)
{
    float span;

    if (!snap || !lo || !hi || !hyst)
        return -1;

    span = snap->hi - snap->lo;
    if (!(span > 0.0f))          /* flat or inverted: fall back to one degree */
        span = 1.0f;

    *lo   = snap->lo + 0.30f * span;
    *hi   = snap->hi - 0.30f * span;
    *hyst = 0.10f * span;
    return 0;
}

float dyt_vm_alarm_clamp_setpoint(float v)
{
    /* isfinite() covers NaN and both infinities in one test: none of the
     * three is a temperature a user could have meant to type, and every
     * comparison against a NaN is false, which would leave an armed alarm
     * that can never fire. */
    if (!isfinite(v))
        return DYT_ALARM_SETPOINT_DEFAULT;
    if (v < DYT_ALARM_SETPOINT_MIN)
        return DYT_ALARM_SETPOINT_MIN;
    if (v > DYT_ALARM_SETPOINT_MAX)
        return DYT_ALARM_SETPOINT_MAX;
    return v;
}

/* --------------------------------------------------------------- overlays */

long dyt_vm_apply_isotherm_scaled(uint8_t *bgr, int w, int h,
                                  const float *temps, int temps_n,
                                  int tw, int th, float lo, float hi)
{
    long dimmed = 0;
    int  yy;

    if (!bgr || !temps || w <= 0 || h <= 0 || tw <= 0 || th <= 0 ||
        temps_n < tw * th)
        return -1;

    /* The image may be a magnification of the temperature plane — the
     * super-resolution render is 2x — so each image pixel samples the
     * temperature it was magnified from.  With tw == w and th == h the
     * division is exact and this is the plain pass. */
    for (yy = 0; yy < h; yy++) {
        uint8_t *row = bgr + (size_t)yy * (size_t)w * 3;
        int      ty  = (int)((long long)yy * th / h);
        int      xx;

        if (ty >= th) ty = th - 1;

        for (xx = 0; xx < w; xx++) {
            int   tx     = (int)((long long)xx * tw / w);
            float t;
            int   inside;

            if (tx >= tw) tx = tw - 1;

            t      = temps[(size_t)ty * (size_t)tw + tx];
            inside = (t == t) && t >= lo && t <= hi;

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

long dyt_vm_apply_isotherm(uint8_t *bgr, int w, int h,
                           const float *temps, int temps_n,
                           float lo, float hi)
{
    return dyt_vm_apply_isotherm_scaled(bgr, w, h, temps, temps_n,
                                        w, h, lo, hi);
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

    /* The embedded JPEG is the picture as displayed, so with super-resolution
     * on it is 2x the temperature plane — sized from the snapshot's own factor
     * rather than assumed, exactly as the frame sources size theirs.  The
     * container below still records the *payload's* native geometry, so a
     * vendor tool re-renders the raw data at full resolution. */
    {
        int f = snap.xform.sr >= 1 ? snap.xform.sr : 1;

        rgb = (uint8_t *)malloc((size_t)snap.width * f *
                                (size_t)snap.height * f * 3);
        if (!rgb)
            FAIL("still: out of memory");

        if (dyt_session_render_rgb(s, rgb, snap.width * f * snap.height * f * 3,
                                   &w, &h) != 0)
            FAIL("still: render failed");
    }

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

/* ------------------------------------------------- capture and recording */

int dyt_vm_capture_name(char *out, size_t n, const char *dir, const char *ts,
                        const char *ext)
{
    int need;

    if (!out || n == 0 || !ts || !ts[0] || !ext || !ext[0])
        return -1;

    if (dir && dir[0])
        need = snprintf(out, n, "%s/dyt_%s.%s", dir, ts, ext);
    else
        need = snprintf(out, n, "dyt_%s.%s", ts, ext);

    return (need > 0 && (size_t)need < n) ? 0 : -1;
}

int dyt_vm_default_capture_dir(char *out, size_t n)
{
    static const char *const vars[] = { "XDG_PICTURES_DIR", "HOME" };
    struct stat              st;

    if (!out || n == 0)
        return -1;

    /* A candidate is used only when it names a directory that is really there:
     * an XDG variable pointing at a path that was never created would send the
     * stills somewhere the file dialog cannot show, which is worse than the
     * fallback. */
    for (size_t i = 0; i < sizeof vars / sizeof vars[0]; i++) {
        const char *v = getenv(vars[i]);
        int         need;

        if (!v || !v[0])
            continue;
        if (stat(v, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        need = snprintf(out, n, "%s", v);
        if (need > 0 && (size_t)need < n)
            return 0;
    }

    /* The working directory is what every caller defaulted to before this
     * existed, so it stays the last resort rather than a failure. */
    if (n < 2)
        return -1;
    out[0] = '.';
    out[1] = '\0';
    return 0;
}

long long dyt_vm_free_bytes(const char *path)
{
    struct statvfs vfs;

    if (!path || !path[0])
        return -1;
    if (statvfs(path, &vfs) != 0)
        return -1;

    /* f_bavail, not f_bfree: the difference is the root reserve, which an
     * unprivileged writer cannot use, so counting it would report room that is
     * not there and turn the guard into a lie. */
    return (long long)vfs.f_bavail * (long long)vfs.f_frsize;
}

int dyt_vm_disk_room(const char *path, long long need_bytes,
                     long long *free_out)
{
    const long long free_b = dyt_vm_free_bytes(path);

    if (free_out)
        *free_out = free_b;
    if (free_b < 0)
        return -1;
    return free_b >= need_bytes ? 1 : 0;
}

int dyt_vm_elapsed(double seconds, char *out, size_t n)
{
    long long s;

    if (!out || n == 0)
        return -1;

    /* A clock that has not started yet — or one fed a NaN — must not print a
     * negative time. */
    if (!(seconds > 0.0))
        s = 0;
    else if (seconds > 359999.0)
        s = 359999;                 /* 99:59:59, so the field cannot grow */
    else
        s = (long long)seconds;

    if (s < 3600)
        snprintf(out, n, "%lld:%02lld", s / 60, s % 60);
    else
        snprintf(out, n, "%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
    return 0;
}

int dyt_vm_rec_label(double seconds, long long frames, char *out, size_t n)
{
    char t[16];

    if (!out || n == 0)
        return -1;
    if (dyt_vm_elapsed(seconds, t, sizeof t) != 0)
        return -1;

    return snprintf(out, n, "REC %s  %lld frame%s", t, frames,
                    frames == 1 ? "" : "s");
}

/* ---------------------------------------------------------------- gallery */

/* Case-insensitive equality, for an extension match. */
static int ci_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb)
            return 0;
    }
    return *a == '\0' && *b == '\0';
}

dyt_vm_item_kind_t dyt_vm_item_kind(const char *name)
{
    size_t l;

    if (!name)
        return DYT_VM_ITEM_NONE;
    l = strlen(name);

    /* `.dyt.jpg` is longer than `.mp4`, and neither is a suffix of the other,
     * so the order here does not matter — but the `>` not `>=` does: a file
     * named exactly ".mp4" has no name and is not one of ours. */
    if (l > 4 && ci_eq(name + l - 4, ".mp4"))
        return DYT_VM_ITEM_CLIP;
    if (l > 8 && ci_eq(name + l - 8, ".dyt.jpg"))
        return DYT_VM_ITEM_STILL;

    return DYT_VM_ITEM_NONE;
}

/* Newest first, then by name, so the order is stable. */
static int item_before(const dyt_vm_item_t *a, const dyt_vm_item_t *b)
{
    if (a->mtime != b->mtime)
        return a->mtime > b->mtime;
    return strcmp(a->name, b->name) < 0;
}

static void item_sort(dyt_vm_item_t *v, int n)
{
    int i;

    /* Insertion sort: a gallery is tens of files, not thousands, and this
     * keeps the comparison in one place instead of a qsort trampoline. */
    for (i = 1; i < n; i++) {
        dyt_vm_item_t key = v[i];
        int           j   = i - 1;
        while (j >= 0 && item_before(&key, &v[j])) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = key;
    }
}

int dyt_vm_scan(const char *dir, dyt_vm_item_t *out, int cap)
{
    const char  *d = (dir && dir[0]) ? dir : ".";
    DIR         *dp;
    dyt_vm_item_t *v = NULL;
    int          n = 0, cap_v = 0, i, rc = -1;

    if (cap < 0 || (cap > 0 && !out))
        return -1;

    dp = opendir(d);
    if (!dp)
        return -1;

    for (struct dirent *e; (e = readdir(dp)) != NULL;) {
        dyt_vm_item_kind_t kind = dyt_vm_item_kind(e->d_name);
        struct stat        st;
        char               full[DYT_VM_PATH_CAP];
        int                need;

        if (kind == DYT_VM_ITEM_NONE)
            continue;
        if (snprintf(full, sizeof full, "%s/%s", d, e->d_name) >=
                (int)sizeof full)
            continue;                       /* a name too long to address */
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
            continue;                       /* a directory named *.mp4, etc. */

        if (n == cap_v) {
            int            ncap = cap_v ? cap_v * 2 : 16;
            dyt_vm_item_t *grow = realloc(v, (size_t)ncap * sizeof *grow);
            if (!grow)
                goto done;
            v      = grow;
            cap_v  = ncap;
        }
        need = snprintf(v[n].name, sizeof v[n].name, "%s", e->d_name);
        if (need <= 0 || need >= (int)sizeof v[n].name)
            continue;                       /* cannot name it: skip, not fail */
        snprintf(v[n].path, sizeof v[n].path, "%s", full);
        v[n].mtime = (long long)st.st_mtime;
        v[n].bytes = (long long)st.st_size;
        v[n].kind  = kind;
        n++;
    }

    item_sort(v, n);

    for (i = 0; i < n && i < cap; i++)
        out[i] = v[i];
    rc = n;                                 /* found, not merely written */

done:
    closedir(dp);
    free(v);
    return rc;
}

int dyt_vm_still_info(const char *path, dyt_vm_still_info_t *info)
{
    uint8_t *blob = NULL, *raw = NULL, *jpg = NULL;
    size_t   bl = 0, rl = 0, jl = 0;
    int      w = 0, ar = 0, tr = 0;
    unsigned fl = 0;
    int      rc;

    if (!path || !info)
        return -1;

    memset(info, 0, sizeof *info);

    if (dyt_dyt_read(path, &blob, &bl, &raw, &rl, &jpg, &jl) != 0)
        return -1;

    info->raw_bytes  = (long long)rl;
    info->n_samples  = (int)(rl / 2);
    info->jpeg_bytes = (long long)jl;

    if (dyt_dyt_blob_geometry(blob, bl, &w, &ar, &tr, &fl) == 1) {
        info->have_thermal = 1;
        info->width        = w;
        info->active_rows  = ar;
        info->total_rows   = tr;
        info->flags        = fl;
    }

    rc = 0;
    free(blob);
    free(raw);
    free(jpg);
    return rc;
}

/* A file size a person reads at a glance. */
static void human_size(long long bytes, char *out, size_t n)
{
    if (bytes >= (1LL << 20))
        snprintf(out, n, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else
        snprintf(out, n, "%lld KB", (bytes + 512) / 1024);
}

void dyt_vm_gallery_init(dyt_vm_gallery_t *g)
{
    if (!g)
        return;
    memset(g, 0, sizeof *g);
    g->sel = -1;
}

int dyt_vm_gallery_load(dyt_vm_gallery_t *g, const char *dir)
{
    char keep[DYT_VM_NAME_CAP];
    int  had = 0, n, i;

    if (!g)
        return -1;

    /* Remember what was highlighted, by name, before the list is replaced. */
    if (g->sel >= 0 && g->sel < g->n) {
        snprintf(keep, sizeof keep, "%s", g->items[g->sel].name);
        had = 1;
    }

    n = dyt_vm_scan(dir, g->items, DYT_VM_GALLERY_MAX);
    if (n < 0) {
        g->n   = 0;
        g->sel = -1;
        return -1;
    }
    g->n = n > DYT_VM_GALLERY_MAX ? DYT_VM_GALLERY_MAX : n;

    g->sel = g->n ? 0 : -1;
    if (had) {
        for (i = 0; i < g->n; i++) {
            if (strcmp(g->items[i].name, keep) == 0) {
                g->sel = i;
                break;
            }
        }
    }
    return g->n;
}

void dyt_vm_gallery_move(dyt_vm_gallery_t *g, int delta)
{
    if (!g || g->n <= 0) {
        if (g)
            g->sel = -1;
        return;
    }
    if (g->sel < 0)
        g->sel = 0;
    else
        g->sel = (int)(((long long)g->sel + delta) % g->n);
    if (g->sel < 0)
        g->sel += g->n;
}

void dyt_vm_gallery_set_sel(dyt_vm_gallery_t *g, int idx)
{
    if (!g || g->n <= 0) {
        if (g)
            g->sel = -1;
        return;
    }
    if (idx < 0)
        idx = 0;
    if (idx >= g->n)
        idx = g->n - 1;
    g->sel = idx;
}

const dyt_vm_item_t *dyt_vm_gallery_sel(const dyt_vm_gallery_t *g)
{
    if (!g || g->sel < 0 || g->sel >= g->n)
        return NULL;
    return &g->items[g->sel];
}

int dyt_vm_gallery_label(const dyt_vm_gallery_t *g, char *out, size_t n)
{
    const dyt_vm_item_t *it;
    char                 sz[32];

    if (!g || !out || n == 0)
        return -1;

    if (g->n == 0)
        return snprintf(out, n, "gallery: no saved stills or clips");

    it = dyt_vm_gallery_sel(g);
    if (!it)
        return snprintf(out, n, "gallery: %d item(s), none selected", g->n);

    human_size(it->bytes, sz, sizeof sz);
    return snprintf(out, n, "gallery %d/%d  %s  %s", g->sel + 1, g->n,
                    it->name, sz);
}

/* ------------------------------------------------------------- view keys */

/* Which super-resolution mode a key selects, or DYT_SR_OFF when the key is not
 * one of them.  One function so the binding and the notice cannot disagree
 * about which letters are SR: "z" is the vendor's own plane, "Z" the thermal
 * extension (sr.h), tied together by case as h/H are. */
static dyt_sr_t sr_key_mode(int key)
{
    if (key == 'z')
        return DYT_SR_VISIBLE;
    if (key == 'Z')
        return DYT_SR_THERMAL;
    return DYT_SR_OFF;
}

int dyt_vm_view_key(dyt_session_t *s, int key)
{
    dyt_sr_t sr = sr_key_mode(key);

    if (!s)
        return 0;

    /* Super-resolution: one feature with two planes.  The mode is a single
     * value, so turning one on turns the other off — both describe the same 2x
     * render, and there is no coherent reading of "both". */
    if (sr != DYT_SR_OFF) {
        dyt_session_set_sr(s, dyt_session_get_sr(s) == sr ? DYT_SR_OFF : sr);
        return 1;
    }

    /* Palette: the ten digits pick one directly, and the two brackets either
     * side of the keyboard's full stop step through all of them. */
    if (key >= '1' && key <= '9') {
        dyt_session_set_palette(s, key - '1');
        return 1;
    }
    if (key == '0') {
        dyt_session_set_palette(s, 9);
        return 1;
    }

    switch (key) {
    case '.': dyt_session_cycle_palette(s, +1);          return 1;
    case ',': dyt_session_cycle_palette(s, -1);          return 1;
    case 'u': dyt_session_cycle_unit(s);                 return 1;
    case 't': dyt_session_toggle_range(s);               return 1;
    case 'h': dyt_session_toggle_flip_h(s);              return 1;
    case 'H': dyt_session_toggle_flip_v(s);              return 1;
    case '+':
    case '=': dyt_session_zoom(s, +1);                   return 1;
    case '-':
    case '_': dyt_session_zoom(s, -1);                   return 1;
    case 'f': dyt_session_cycle_fusion(s, +1);           return 1;
    case '[': dyt_session_adjust_fusion_align(s, -1, 0); return 1;
    case ']': dyt_session_adjust_fusion_align(s, +1, 0); return 1;
    case ';': dyt_session_adjust_fusion_align(s, 0, -1); return 1;
    case '\'': dyt_session_adjust_fusion_align(s, 0, +1); return 1;
    default: break;
    }
    return 0;
}

/* The notice a super-resolution key press deserves, or 0 when the key was not
 * one of them.  See view_model.h for why the wording lives here. */
int dyt_vm_sr_notice(int key, const dyt_snapshot_t *snap, char *out, size_t n)
{
    if (sr_key_mode(key) == DYT_SR_OFF)
        return 0;
    if (!snap || !out || n == 0)
        return -1;

    /* No upscaler was installed (no runtime in this build, or no model found),
     * so the session refused the mode rather than remembering it.  Say so: the
     * status line stays silent while the mode is off, and a key that does
     * nothing at all is the one thing this notice exists to prevent. */
    if (!snap->sr_cap) {
        snprintf(out, n, "super-resolution: no model loaded");
        return 1;
    }

    if (snap->sr == DYT_SR_OFF) {
        snprintf(out, n, "sr:off");
        return 1;
    }

    if (snap->sr_active) {
        snprintf(out, n, "sr:%s x%d", snap->sr_name, snap->xform.sr);
        return 1;
    }

    /* Selected but not taking effect.  Name the one thing to change, in the
     * same order sr_factor_locked() checks it, so the advice matches the
     * refusal. */
    if (snap->width != DYT_SR_IN_W || snap->height != DYT_SR_IN_H)
        snprintf(out, n, "sr:%s needs a %dx%d frame", snap->sr_name,
                 DYT_SR_IN_W, DYT_SR_IN_H);
    else if (snap->sr == DYT_SR_VISIBLE &&
             snap->fusion == DYT_FUSION_INFRARED)
        snprintf(out, n, "sr:%s needs a fusion pattern that shows it",
                 snap->sr_name);
    else if (snap->sr == DYT_SR_VISIBLE && !snap->have_visible)
        snprintf(out, n, "sr:%s needs a visible half", snap->sr_name);
    else
        snprintf(out, n, "sr:%s inactive", snap->sr_name);
    return 1;
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

/* One candidate path — a directory or a file: readable?  On success copies it
 * to `out`. */
static int readable_path(const char *p, char *out, size_t n)
{
    if (!p || !p[0] || access(p, R_OK) != 0)
        return 0;
    snprintf(out, n, "%s", p);
    return 1;
}

/* The installed layout: <datadir>/dytqt/<leaf> under each XDG data directory.
 * $XDG_DATA_DIRS when set and non-empty, else the spec's default of
 * /usr/local/share:/usr/share — which is where `make install` puts the data for
 * both prefixes that matter.  This is what lets a packaged dytqt find the
 * vendor palettes (and the super-resolution model) from any cwd; without it an
 * installed binary silently falls back to the six built-in ramps. */
int dyt_vm_find_data_dir(const char *leaf, char *out, size_t n)
{
    const char *dirs;
    const char *p;

    if (!out || n == 0)
        return 0;
    out[0] = '\0';

    /* A leaf, not a path: no separators, or the search could escape dytqt/. */
    if (!leaf || !leaf[0] || strchr(leaf, '/') || strcmp(leaf, ".") == 0 ||
        strcmp(leaf, "..") == 0)
        return 0;

    dirs = getenv("XDG_DATA_DIRS");
    if (!dirs || !dirs[0])
        dirs = "/usr/local/share:/usr/share";

    for (p = dirs; *p; ) {
        const char *sep = strchr(p, ':');
        size_t      len = sep ? (size_t)(sep - p) : strlen(p);
        char        cand[4096];
        int         need = (int)strlen(leaf) + (int)sizeof "/dytqt/" + 1;

        if (len > 0 && len + (size_t)need < sizeof cand) {
            snprintf(cand, sizeof cand, "%.*s/dytqt/%s", (int)len, p, leaf);
            if (readable_path(cand, out, n))
                return 1;
        }
        if (!sep)
            break;
        p = sep + 1;
    }
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

    /* An explicit request wins. */
    if (readable_path(dir_opt, out, n))
        return 1;

    /* Then beside the cwd and the executable, so a source-tree run keeps using
     * the tree's own palettes rather than an installed copy. */
    for (i = 0; i < 2; i++)
        if (readable_path(fixed[i], out, n))
            return 1;

    if (dyt_vm_exe_dir(ed, sizeof ed) == 0) {
        for (i = 0; i < 2; i++) {
            snprintf(cand, sizeof cand, "%s/%s", ed, fixed[i]);
            if (readable_path(cand, out, n))
                return 1;
        }
    }

    /* Finally the installed location. */
    return dyt_vm_find_data_dir("palettes", out, n);
}

int dyt_vm_find_model(const char *path_opt, char *out, size_t n)
{
    char        cand[4096];
    char        ed[2048];
    const char *fixed[] = { "models/zoom2.mnn", "../models/zoom2.mnn" };
    int         i;

    if (!out || n == 0)
        return 0;
    out[0] = '\0';

    /* An explicit request wins. */
    if (readable_path(path_opt, out, n))
        return 1;

    /* Then beside the cwd and the executable, so a source-tree run uses the
     * tree's own model rather than an installed copy. */
    for (i = 0; i < 2; i++)
        if (readable_path(fixed[i], out, n))
            return 1;

    if (dyt_vm_exe_dir(ed, sizeof ed) == 0) {
        for (i = 0; i < 2; i++) {
            snprintf(cand, sizeof cand, "%s/%s", ed, fixed[i]);
            if (readable_path(cand, out, n))
                return 1;
        }
    }

    /* Finally the installed location: <datadir>/dytqt/models/zoom2.mnn, which
     * is where `make install` puts it (the palettes' sibling). */
    if (dyt_vm_find_data_dir("models", cand, sizeof cand)) {
        /* Sized for the suffix so the append cannot be truncated — `cand` is
         * already at its limit when the data directory is a long one. */
        char full[sizeof cand + sizeof "/zoom2.mnn"];

        snprintf(full, sizeof full, "%s/zoom2.mnn", cand);
        if (readable_path(full, out, n))
            return 1;
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
