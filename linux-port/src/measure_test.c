/*
 * measure_test.c — unit tests for measure.c.
 *
 * The risks worth testing are the ones that would put a wrong number on
 * screen rather than crash: a non-finite sample counted as a reading, a
 * rectangle that is half outside the image silently shrinking instead of
 * being clipped, a median taken over the wrong sample count, and a line
 * profile whose x-axis silently shortens when it leaves the image.
 *
 * The fixture is a deterministic 8x4 ramp, temps[y*8+x] = 10 + y*8 + x, so
 * every statistic below can be worked out by hand.
 *
 * build:  via the Makefile (make check)
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "measure.h"

static int fails;

static void ok(const char *what) { printf("  ok   %s\n", what); }

static void fail(const char *what, const char *detail)
{
    printf("  FAIL %-46s %s\n", what, detail);
    fails++;
}

static int near(float a, float b, float eps) { return fabsf(a - b) <= eps; }

static void intcheck(const char *what, int got, int want)
{
    if (got == want) {
        ok(what);
    } else {
        char d[96];
        snprintf(d, sizeof d, "got %d want %d", got, want);
        fail(what, d);
    }
}

static void floatcheck(const char *what, float got, float want, float eps)
{
    if (near(got, want, eps)) {
        ok(what);
    } else {
        char d[96];
        snprintf(d, sizeof d, "got %.4f want %.4f", (double)got, (double)want);
        fail(what, d);
    }
}

static void nancheck(const char *what, float got)
{
    if (isnan(got))
        ok(what);
    else {
        char d[96];
        snprintf(d, sizeof d, "got %.4f want NaN", (double)got);
        fail(what, d);
    }
}

/* --- fixture ------------------------------------------------------------ */

#define W 8
#define H 4
#define NPIX (W * H)

static void build_ramp(float *t)
{
    int x, y;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            t[y * W + x] = 10.0f + (float)(y * W + x);   /* 10 .. 41 */
}

/* --- point probe -------------------------------------------------------- */

static void test_point(void)
{
    float t[NPIX], v;

    build_ramp(t);

    intcheck("point: origin", dyt_measure_point(t, W, H, 0, 0, &v), 0);
    floatcheck("point: origin == 10", v, 10.0f, 1e-6f);

    intcheck("point: last pixel", dyt_measure_point(t, W, H, 7, 3, &v), 0);
    floatcheck("point: last == 41", v, 41.0f, 1e-6f);

    intcheck("point: x out of range rejected",
             dyt_measure_point(t, W, H, 8, 0, &v), -1);
    intcheck("point: y out of range rejected",
             dyt_measure_point(t, W, H, 0, 4, &v), -1);
    intcheck("point: negative rejected",
             dyt_measure_point(t, W, H, -1, 0, &v), -1);

    t[5] = NAN;
    intcheck("point: NaN sample rejected",
             dyt_measure_point(t, W, H, 5, 0, &v), -1);

    intcheck("point: NULL plane rejected",
             dyt_measure_point(NULL, W, H, 0, 0, &v), -1);
    intcheck("point: NULL out rejected",
             dyt_measure_point(t, W, H, 0, 0, NULL), -1);
}

/* --- rectangle ROI ------------------------------------------------------ */

static void test_roi(void)
{
    float t[NPIX], scratch[NPIX];
    dyt_roi_stats_t s;

    build_ramp(t);

    /* Whole image: 10..41, mean of an arithmetic series, even count. */
    intcheck("roi: full image",
             dyt_measure_roi(t, W, H, 0, 0, W - 1, H - 1, scratch, NPIX, &s), 0);
    intcheck("roi: full n == 32", s.n, 32);
    floatcheck("roi: full min == 10", s.min, 10.0f, 1e-6f);
    floatcheck("roi: full max == 41", s.max, 41.0f, 1e-6f);
    floatcheck("roi: full mean == 25.5", s.mean, 25.5f, 1e-6f);
    floatcheck("roi: full median == 25.5", s.median, 25.5f, 1e-6f);
    intcheck("roi: full min at (0,0)", s.min_x, 0);
    intcheck("roi: full min at (0,0) y", s.min_y, 0);
    intcheck("roi: full max at (7,3)", s.max_x, 7);
    intcheck("roi: full max at (7,3) y", s.max_y, 3);

    /* 2x2 sub-rectangle: 20,21 / 28,29. */
    intcheck("roi: sub-rect",
             dyt_measure_roi(t, W, H, 2, 1, 3, 2, scratch, NPIX, &s), 0);
    intcheck("roi: sub n == 4", s.n, 4);
    floatcheck("roi: sub min == 20", s.min, 20.0f, 1e-6f);
    floatcheck("roi: sub max == 29", s.max, 29.0f, 1e-6f);
    floatcheck("roi: sub mean == 24.5", s.mean, 24.5f, 1e-6f);
    floatcheck("roi: sub median == 24.5", s.median, 24.5f, 1e-6f);

    /* Corners in any order must give the identical rectangle. */
    intcheck("roi: reversed corners accepted",
             dyt_measure_roi(t, W, H, 3, 2, 2, 1, scratch, NPIX, &s), 0);
    intcheck("roi: reversed n == 4", s.n, 4);
    floatcheck("roi: reversed min == 20", s.min, 20.0f, 1e-6f);
    floatcheck("roi: reversed max == 29", s.max, 29.0f, 1e-6f);

    /* Half outside the image: clipped to 0..1 x 0..1 = {10,11,18,19}. */
    intcheck("roi: clipped",
             dyt_measure_roi(t, W, H, -5, -5, 1, 1, scratch, NPIX, &s), 0);
    intcheck("roi: clipped n == 4", s.n, 4);
    floatcheck("roi: clipped min == 10", s.min, 10.0f, 1e-6f);
    floatcheck("roi: clipped max == 19", s.max, 19.0f, 1e-6f);

    /* Entirely outside: a valid empty selection, not an error. */
    intcheck("roi: fully outside is not an error",
             dyt_measure_roi(t, W, H, -10, -10, -5, -5, scratch, NPIX, &s), 0);
    intcheck("roi: fully outside n == 0", s.n, 0);
    nancheck("roi: fully outside min is NaN", s.min);
    nancheck("roi: fully outside mean is NaN", s.mean);

    /* Scratch too small: report the requirement, do not write past the end. */
    intcheck("roi: small scratch -> -2",
             dyt_measure_roi(t, W, H, 0, 0, W - 1, H - 1, scratch, 2, &s), -2);
    intcheck("roi: small scratch reports need", s.n, NPIX);

    /* No scratch: everything but the median. */
    intcheck("roi: NULL scratch ok",
             dyt_measure_roi(t, W, H, 0, 0, W - 1, H - 1, NULL, 0, &s), 0);
    floatcheck("roi: NULL scratch still means", s.mean, 25.5f, 1e-6f);
    nancheck("roi: NULL scratch median is NaN", s.median);

    /* A NaN pixel is skipped, not counted, and cannot become the minimum. */
    build_ramp(t);
    t[0] = NAN;                       /* would have been the minimum */
    intcheck("roi: NaN skipped",
             dyt_measure_roi(t, W, H, 0, 0, W - 1, H - 1, scratch, NPIX, &s), 0);
    intcheck("roi: NaN not counted", s.n, NPIX - 1);
    floatcheck("roi: NaN cannot be the min", s.min, 11.0f, 1e-6f);
    intcheck("roi: new min is at (1,0)", s.min_x, 1);

    /* All-NaN region: no reading at all. */
    {
        float nanimg[4];
        int   i;
        for (i = 0; i < 4; i++) nanimg[i] = NAN;
        intcheck("roi: all-NaN region ok",
                 dyt_measure_roi(nanimg, 2, 2, 0, 0, 1, 1, scratch, 4, &s), 0);
        intcheck("roi: all-NaN n == 0", s.n, 0);
        nancheck("roi: all-NaN median is NaN", s.median);
    }

    intcheck("roi: NULL plane rejected",
             dyt_measure_roi(NULL, W, H, 0, 0, 1, 1, scratch, NPIX, &s), -1);
    intcheck("roi: NULL out rejected",
             dyt_measure_roi(t, W, H, 0, 0, 1, 1, scratch, NPIX, NULL), -1);
}

/* --- line profile ------------------------------------------------------- */

static void test_line(void)
{
    float t[NPIX], prof[64];
    int   n;

    build_ramp(t);

    n = dyt_measure_line(t, W, H, 0, 0, 7, 0, prof, 64);
    intcheck("line: horizontal count == 8", n, 8);
    floatcheck("line: horizontal start", prof[0], 10.0f, 1e-6f);
    floatcheck("line: horizontal end", prof[7], 17.0f, 1e-6f);

    n = dyt_measure_line(t, W, H, 0, 0, 0, 3, prof, 64);
    intcheck("line: vertical count == 4", n, 4);
    floatcheck("line: vertical start", prof[0], 10.0f, 1e-6f);
    floatcheck("line: vertical end", prof[3], 34.0f, 1e-6f);

    n = dyt_measure_line(t, W, H, 0, 0, 7, 3, prof, 64);
    intcheck("line: diagonal count == 8", n, 8);
    floatcheck("line: diagonal start", prof[0], 10.0f, 1e-6f);
    floatcheck("line: diagonal end", prof[7], 41.0f, 1e-6f);

    /* A single point is a one-sample profile, not a division by zero. */
    n = dyt_measure_line(t, W, H, 2, 2, 2, 2, prof, 64);
    intcheck("line: degenerate count == 1", n, 1);
    floatcheck("line: degenerate value", prof[0], 10.0f + 2 * W + 2, 1e-6f);

    /* Cap too small: the negated requirement comes back, nothing is written. */
    n = dyt_measure_line(t, W, H, 0, 0, 7, 0, prof, 3);
    intcheck("line: small cap -> -8", n, -8);

    /* Leaving the image keeps the spacing and writes NaN. */
    {
        float small[16];
        int   x, y;
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++)
                small[y * 4 + x] = (float)(y * 4 + x);

        n = dyt_measure_line(small, 4, 4, -2, 0, 5, 0, prof, 64);
        intcheck("line: off-image count still 8", n, 8);
        nancheck("line: off-image left is NaN", prof[0]);
        nancheck("line: off-image left is NaN (2)", prof[1]);
        floatcheck("line: first on-image sample", prof[2], 0.0f, 1e-6f);
        floatcheck("line: last on-image sample", prof[5], 3.0f, 1e-6f);
        nancheck("line: off-image right is NaN", prof[6]);
        nancheck("line: off-image right is NaN (2)", prof[7]);
    }

    intcheck("line: NULL plane rejected",
             dyt_measure_line(NULL, W, H, 0, 0, 1, 1, prof, 64), -1);
    intcheck("line: NULL out rejected",
             dyt_measure_line(t, W, H, 0, 0, 1, 1, NULL, 64), -1);
}

/* --- polygon ROI -------------------------------------------------------- */

static void test_polygon(void)
{
    float t[NPIX], scratch[NPIX];
    dyt_roi_stats_t s;
    dyt_point_t tri[3], rect[4], ell[6], rev[3], far[3];

    build_ramp(t);

    /* Right triangle (0,0) (4,0) (0,4): rows 0..3 cover 4,3,2,1 pixels, so
     * 10 in all — 10,11,12,13 / 18,19,20 / 26,27 / 34. */
    tri[0].x = 0; tri[0].y = 0;
    tri[1].x = 4; tri[1].y = 0;
    tri[2].x = 0; tri[2].y = 4;

    intcheck("poly: triangle",
             dyt_measure_polygon(t, W, H, tri, 3, scratch, NPIX, &s), 0);
    intcheck("poly: triangle n == 10", s.n, 10);
    floatcheck("poly: triangle min == 10", s.min, 10.0f, 1e-6f);
    floatcheck("poly: triangle max == 34", s.max, 34.0f, 1e-6f);
    floatcheck("poly: triangle mean == 19", s.mean, 19.0f, 1e-6f);
    floatcheck("poly: triangle median == 18.5", s.median, 18.5f, 1e-6f);
    intcheck("poly: triangle min at (0,0)", s.min_x, 0);
    intcheck("poly: triangle min at (0,0) y", s.min_y, 0);
    intcheck("poly: triangle max at (0,3)", s.max_x, 0);
    intcheck("poly: triangle max at (0,3) y", s.max_y, 3);

    /* Winding must not matter: even-odd is orientation-independent. */
    rev[0] = tri[2]; rev[1] = tri[1]; rev[2] = tri[0];
    intcheck("poly: reversed winding",
             dyt_measure_polygon(t, W, H, rev, 3, scratch, NPIX, &s), 0);
    intcheck("poly: reversed n == 10", s.n, 10);
    floatcheck("poly: reversed mean == 19", s.mean, 19.0f, 1e-6f);
    floatcheck("poly: reversed median == 18.5", s.median, 18.5f, 1e-6f);

    /* A rectangle outline covers the same pixels as the box tool does for the
     * box it encloses: (2,1) (4,1) (4,3) (2,3) is the box (2,1)-(3,2), i.e.
     * 20,21 / 28,29.  This is the convention stated in measure.h. */
    rect[0].x = 2; rect[0].y = 1;
    rect[1].x = 4; rect[1].y = 1;
    rect[2].x = 4; rect[2].y = 3;
    rect[3].x = 2; rect[3].y = 3;

    intcheck("poly: rectangle == box",
             dyt_measure_polygon(t, W, H, rect, 4, scratch, NPIX, &s), 0);
    intcheck("poly: rectangle n == 4", s.n, 4);
    floatcheck("poly: rectangle min == 20", s.min, 20.0f, 1e-6f);
    floatcheck("poly: rectangle max == 29", s.max, 29.0f, 1e-6f);
    floatcheck("poly: rectangle mean == 24.5", s.mean, 24.5f, 1e-6f);
    floatcheck("poly: rectangle median == 24.5", s.median, 24.5f, 1e-6f);

    /* A concave L: a 3-wide top bar (row 0) over a 1-wide left column (rows
     * 1 and 2) — 10,11,12 / 18 / 26.  A convex-only fill would have swept in
     * (1,1) and (2,1) and reported a different count. */
    ell[0].x = 0; ell[0].y = 0;
    ell[1].x = 3; ell[1].y = 0;
    ell[2].x = 3; ell[2].y = 1;
    ell[3].x = 1; ell[3].y = 1;
    ell[4].x = 1; ell[4].y = 3;
    ell[5].x = 0; ell[5].y = 3;

    intcheck("poly: concave L",
             dyt_measure_polygon(t, W, H, ell, 6, scratch, NPIX, &s), 0);
    intcheck("poly: concave n == 5", s.n, 5);
    floatcheck("poly: concave min == 10", s.min, 10.0f, 1e-6f);
    floatcheck("poly: concave max == 26", s.max, 26.0f, 1e-6f);
    floatcheck("poly: concave mean == 15.4", s.mean, 15.4f, 1e-6f);
    floatcheck("poly: concave median == 12", s.median, 12.0f, 1e-6f);

    /* Hanging off the left edge: only the part inside the image is counted,
     * and the fractional crossing at row 1 (x = 2/3) must round inward. */
    far[0].x = -4; far[0].y = 0;
    far[1].x = 3;  far[1].y = 0;
    far[2].x = -4; far[2].y = 3;

    intcheck("poly: clipped",
             dyt_measure_polygon(t, W, H, far, 3, scratch, NPIX, &s), 0);
    intcheck("poly: clipped n == 4", s.n, 4);
    floatcheck("poly: clipped min == 10", s.min, 10.0f, 1e-6f);
    floatcheck("poly: clipped max == 18", s.max, 18.0f, 1e-6f);
    floatcheck("poly: clipped mean == 12.75", s.mean, 12.75f, 1e-6f);

    /* Entirely outside: a valid empty selection, not an error. */
    far[0].x = -20; far[0].y = -20;
    far[1].x = -10; far[1].y = -20;
    far[2].x = -20; far[2].y = -10;
    intcheck("poly: fully outside is not an error",
             dyt_measure_polygon(t, W, H, far, 3, scratch, NPIX, &s), 0);
    intcheck("poly: fully outside n == 0", s.n, 0);
    nancheck("poly: fully outside min is NaN", s.min);
    nancheck("poly: fully outside mean is NaN", s.mean);
    intcheck("poly: fully outside has no extreme", s.min_x, -1);

    /* A NaN pixel inside the outline is skipped, not counted. */
    build_ramp(t);
    t[0] = NAN;                       /* would have been the minimum */
    intcheck("poly: NaN skipped",
             dyt_measure_polygon(t, W, H, tri, 3, scratch, NPIX, &s), 0);
    intcheck("poly: NaN not counted", s.n, 9);
    floatcheck("poly: NaN cannot be the min", s.min, 11.0f, 1e-6f);
    intcheck("poly: new min is at (1,0)", s.min_x, 1);
    build_ramp(t);

    /* No scratch: everything but the median. */
    intcheck("poly: NULL scratch ok",
             dyt_measure_polygon(t, W, H, tri, 3, NULL, 0, &s), 0);
    floatcheck("poly: NULL scratch still means", s.mean, 19.0f, 1e-6f);
    nancheck("poly: NULL scratch median is NaN", s.median);

    /* Too little scratch: the requirement comes back after the fill, and the
     * statistics are not handed over with it. */
    intcheck("poly: small scratch -> -2",
             dyt_measure_polygon(t, W, H, tri, 3, scratch, 4, &s), -2);
    intcheck("poly: small scratch reports need", s.n, 10);

    intcheck("poly: two vertices rejected",
             dyt_measure_polygon(t, W, H, tri, 2, scratch, NPIX, &s), -1);
    intcheck("poly: NULL verts rejected",
             dyt_measure_polygon(t, W, H, NULL, 3, scratch, NPIX, &s), -1);
    intcheck("poly: NULL plane rejected",
             dyt_measure_polygon(NULL, W, H, tri, 3, scratch, NPIX, &s), -1);
    intcheck("poly: NULL out rejected",
             dyt_measure_polygon(t, W, H, tri, 3, scratch, NPIX, NULL), -1);

    /* Too many vertices is refused rather than truncated — silently dropping
     * one would move the boundary the user drew. */
    {
        dyt_point_t many[DYT_POLYGON_MAX_VTX + 1];
        int i;
        for (i = 0; i <= DYT_POLYGON_MAX_VTX; i++) {
            many[i].x = i % W;
            many[i].y = i % H;
        }
        intcheck("poly: too many vertices rejected",
                 dyt_measure_polygon(t, W, H, many, DYT_POLYGON_MAX_VTX + 1,
                                     scratch, NPIX, &s), -1);
    }
}

/* --- isotherm / area check ---------------------------------------------- */

static void test_isotherm(void)
{
    float t[NPIX];
    dyt_isotherm_t iso;

    build_ramp(t);

    /* The ramp holds 10..41 exactly once each; 20..29 is ten of them. */
    intcheck("iso: band [20,29]",
             dyt_measure_isotherm(t, W, H, 20.0f, 29.0f, &iso), 0);
    intcheck("iso: count == 10", (int)iso.count, 10);
    intcheck("iso: total == 32", (int)iso.total, 32);
    floatcheck("iso: fraction == 10/32", iso.fraction, 10.0f / 32.0f, 1e-6f);
    floatcheck("iso: min == 20", iso.min, 20.0f, 1e-6f);
    floatcheck("iso: max == 29", iso.max, 29.0f, 1e-6f);

    /* Band above everything: no pixels, no reading, but a valid fraction. */
    intcheck("iso: empty band ok",
             dyt_measure_isotherm(t, W, H, 100.0f, 200.0f, &iso), 0);
    intcheck("iso: empty count == 0", (int)iso.count, 0);
    floatcheck("iso: empty fraction == 0", iso.fraction, 0.0f, 1e-6f);
    nancheck("iso: empty min is NaN", iso.min);
    nancheck("iso: empty max is NaN", iso.max);

    /* Band covering everything. */
    intcheck("iso: full band",
             dyt_measure_isotherm(t, W, H, -100.0f, 100.0f, &iso), 0);
    intcheck("iso: full count == 32", (int)iso.count, 32);
    floatcheck("iso: full fraction == 1", iso.fraction, 1.0f, 1e-6f);

    /* A NaN pixel is excluded from both the count and the total. */
    t[3] = NAN;
    intcheck("iso: NaN excluded",
             dyt_measure_isotherm(t, W, H, -100.0f, 100.0f, &iso), 0);
    intcheck("iso: NaN total == 31", (int)iso.total, 31);
    intcheck("iso: NaN count == 31", (int)iso.count, 31);

    intcheck("iso: hi < lo rejected",
             dyt_measure_isotherm(t, W, H, 30.0f, 20.0f, &iso), -1);
    intcheck("iso: NULL plane rejected",
             dyt_measure_isotherm(NULL, W, H, 0.0f, 1.0f, &iso), -1);
    intcheck("iso: NULL out rejected",
             dyt_measure_isotherm(t, W, H, 0.0f, 1.0f, NULL), -1);
}

int main(void)
{
    printf("=== measure_test (point / ROI / line / polygon / isotherm) ===\n");

    test_point();
    test_roi();
    test_line();
    test_polygon();
    test_isotherm();

    if (fails) {
        printf("=== %d FAILURE(S) ===\n", fails);
        return 1;
    }
    printf("=== ALL PASS ===\n");
    return 0;
}
