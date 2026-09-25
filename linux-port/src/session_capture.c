/*
 * session_capture.c — the capture -> session adapter.  See session_capture.h.
 *
 * The two things worth knowing about this file:
 *
 *  1. It is the only place that knows how to get from "a raw dual-half
 *     payload" to "a grey plane the session can fuse".  The thermal plane's
 *     offset comes from dyt_capture_plane_geometry(); the rows *above* it are
 *     the visible picture, and their height is therefore plane_y
 *     (RE Docs 04 §4.10).  In the AD output mode plane_y is 0, so nothing is
 *     extracted and the session keeps rendering plain thermal.
 *
 *  2. It grows its scratch buffer only when the geometry changes.  The frame
 *     callback must not allocate per frame; one realloc on the first frame
 *     (or after a mode switch) is the whole cost.
 */
#include <stdlib.h>
#include <string.h>

#include "session_capture.h"
#include "visible.h"

struct dyt_session_capture {
    dyt_session_t *sess;        /* borrowed */
    dyt_capture_t *cap;         /* borrowed; may be NULL (no visible half) */

    uint8_t       *grey;        /* scratch for the extracted visible plane */
    int            grey_cap;
};

dyt_session_capture_t *dyt_session_capture_create(dyt_session_t *s)
{
    dyt_session_capture_t *sc;

    if (!s)
        return NULL;

    sc = calloc(1, sizeof *sc);
    if (!sc)
        return NULL;

    sc->sess = s;
    return sc;
}

void dyt_session_capture_free(dyt_session_capture_t *sc)
{
    if (!sc)
        return;
    free(sc->grey);
    free(sc);
}

int dyt_session_capture_set_capture(dyt_session_capture_t *sc, dyt_capture_t *c)
{
    if (!sc || !c)
        return -1;
    sc->cap = c;
    return 0;
}

void dyt_session_capture_on_frame(const float *temps, int n, int width,
                                  int active_height, void *user)
{
    dyt_session_capture_t *sc = user;
    dyt_frame_info_t fi;
    int npix, off;

    if (!sc || !sc->sess || !temps || width <= 0 || active_height <= 0)
        return;

    npix = width * active_height;
    off  = n - npix;            /* mode 0x44c prefixes a 10-float header */
    if (npix <= 0 || off < 0)
        return;

    fi.temps  = temps + off;
    fi.width  = width;
    fi.height = active_height;
    if (dyt_session_process(sc->sess, &fi) != 0)
        return;

    /* Everything that needs the raw payload.  Read on this thread: the
     * pointer is only valid inside the frame callback, which is where we are
     * (capture.h documents that contract).  nraw is the whole payload, so the
     * guard also catches a stale/partial staging buffer. */
    if (sc->cap) {
        const uint16_t *raw;
        int total = 0, plane_y = 0, plane_h = 0, nraw = 0;

        dyt_capture_plane_geometry(sc->cap, &total, &plane_y, &plane_h);
        raw = dyt_capture_last_raw(sc->cap, &nraw);

        if (raw && total > 0 && nraw == total * width) {
            /* Hand the session its own copy, so a still can later be written
             * from the GUI thread.  This happens in both output modes — the
             * AD mode has no visible half but still has a payload. */
            dyt_session_process_raw(sc->sess, raw, nraw, width, total);

            /* plane_y > 0 and plane_y < total is exactly "there are visible
             * rows above the thermal plane" (RE Docs 04 §4.10). */
            if (plane_y > 0 && plane_y < total) {
                int want = width * plane_y;

                if (sc->grey_cap < want) {
                    uint8_t *grown = realloc(sc->grey, (size_t)want);
                    if (!grown)
                        return;     /* no plane this frame; thermal still ok */
                    sc->grey     = grown;
                    sc->grey_cap = want;
                }

                if (dyt_visible_extract(raw, width, plane_y, sc->grey) == 0)
                    dyt_session_process_visible(sc->sess, sc->grey, width,
                                                plane_y);
            }
        }
    }
}
