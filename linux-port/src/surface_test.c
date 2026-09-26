/*
 * surface_test.c — unit tests for surface.c.
 *
 * The risks worth testing are the ones that would put a wrong shape on
 * screen rather than crash: a vertex's height not matching the temperature it
 * came from, an index list that drops a cell or points outside the grid, a
 * flat grid shading like a slope, and a NaN sample leaking into a normal as a
 * NaN the renderer then has to cope with.
 *
 * The fixture is a small grid whose geometry is hand-checkable: a ramp in x,
 * a ramp in y, a flat sheet, and a single hot pixel, so the expected normal
 * direction of each is known without a reference image.
 *
 * build:  via the Makefile (make check)
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "surface.h"

static int fails;

static void ok(const char *what) { printf("  ok   %s\n", what); }

static void fail(const char *what, const char *detail)
{
    printf("  FAIL %-46s %s\n", what, detail);
    fails++;
}

static void intcheck(const char *what, long got, long want)
{
    if (got == want) {
        ok(what);
    } else {
        char d[96];
        snprintf(d, sizeof d, "got %ld want %ld", got, want);
        fail(what, d);
    }
}

static void floatcheck(const char *what, float got, float want, float eps)
{
    if (fabsf(got - want) <= eps) {
        ok(what);
    } else {
        char d[96];
        snprintf(d, sizeof d, "got %.5f want %.5f", (double)got, (double)want);
        fail(what, d);
    }
}

static void boolcheck(const char *what, int cond)
{
    if (cond)
        ok(what);
    else
        fail(what, "condition false");
}

/* --- fixture ------------------------------------------------------------ */

#define W 4
#define H 3
#define NPIX (W * H)
#define NIDX ((W - 1) * (H - 1) * 6)   /* 36 */

static void build_ramp_x(float *t)
{
    int x, y;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            t[y * W + x] = (float)x;      /* rises with x, flat in y */
}

static void build_ramp_y(float *t)
{
    int x, y;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            t[y * W + x] = (float)y;      /* rises with y, flat in x */
}

static void build_flat(float *t)
{
    int i;
    for (i = 0; i < NPIX; i++)
        t[i] = 25.0f;
}

/* --- counts ------------------------------------------------------------- */

static void test_counts(void)
{
    intcheck("counts: vertices 4x3", dyt_surface_vertex_count(W, H), NPIX);
    intcheck("counts: indices 4x3", dyt_surface_index_count(W, H), NIDX);

    /* A single row or column has no cells to triangulate. */
    intcheck("counts: 1x3 has no cells", dyt_surface_index_count(1, H), 0);
    intcheck("counts: 4x1 has no cells", dyt_surface_index_count(W, 1), 0);
    intcheck("counts: 2x2 has one cell", dyt_surface_index_count(2, 2), 6);

    /* Degenerate dimensions count nothing. */
    intcheck("counts: 0x3 has no vertices", dyt_surface_vertex_count(0, H), 0);
    intcheck("counts: 4x0 has no indices", dyt_surface_index_count(W, 0), 0);
}

/* --- vertices ----------------------------------------------------------- */

static void test_vertices(void)
{
    float                 t[NPIX];
    dyt_surface_vertex_t  v[NPIX];
    int                   i;

    build_ramp_x(t);
    intcheck("verts: ok", dyt_surface_vertices(t, W, H, v), 0);

    /* Height is the temperature, read straight back. */
    for (i = 0; i < NPIX; i++)
        if (fabsf(v[i].z - t[i]) > 1e-6f) {
            floatcheck("verts: z == temp", v[i].z, t[i], 1e-6f);
            break;
        }
    if (i == NPIX)
        ok("verts: z == temp");

    /* The ground plane is [-1, 1] on both axes. */
    floatcheck("verts: first x is -1", v[0].x, -1.0f, 1e-6f);
    floatcheck("verts: last x is +1", v[W - 1].x, 1.0f, 1e-6f);
    floatcheck("verts: first y is -1", v[0].y, -1.0f, 1e-6f);
    floatcheck("verts: last y is +1", v[(H - 1) * W].y, 1.0f, 1e-6f);
    floatcheck("verts: x step is 2/(W-1)", v[1].x - v[0].x, 2.0f / 3.0f, 1e-6f);

    intcheck("verts: NULL temps rejected",
             dyt_surface_vertices(NULL, W, H, v), -1);
    intcheck("verts: NULL verts rejected",
             dyt_surface_vertices(t, W, H, NULL), -1);
    intcheck("verts: bad w rejected",
             dyt_surface_vertices(t, 0, H, v), -1);
}

/* --- indices ------------------------------------------------------------ */

static void test_indices(void)
{
    uint32_t idx[NIDX];
    int      i, j, k = 0, split_ok = 1;

    intcheck("idx: ok", dyt_surface_indices(W, H, idx), 0);

    /* Every cell contributes its two triangles, split on the v01–v10
     * diagonal, so the quad is covered exactly once.  Checked against the
     * expected sequence rather than by counting, so a dropped or reordered
     * cell fails. */
    for (j = 0; j + 1 < H && split_ok; j++) {
        for (i = 0; i + 1 < W && split_ok; i++) {
            uint32_t v00 = (uint32_t)(j * W + i), v10 = v00 + 1;
            uint32_t v01 = (uint32_t)((j + 1) * W + i), v11 = v01 + 1;
            uint32_t want[6] = { v00, v01, v10, v10, v01, v11 };
            int      m;
            for (m = 0; m < 6; m++)
                if (idx[k + m] != want[m]) {
                    char d[96];
                    snprintf(d, sizeof d, "cell (%d,%d) slot %d got %u want %u",
                             i, j, m, idx[k + m], want[m]);
                    fail("idx: cell split", d);
                    split_ok = 0;
                    break;
                }
            k += 6;
        }
    }
    if (split_ok)
        ok("idx: cell split");

    /* Every index addresses a real vertex. */
    int in_range = 1;
    for (i = 0; i < NIDX; i++)
        if (idx[i] >= (uint32_t)NPIX)
            in_range = 0;
    boolcheck("idx: all in range", in_range);

    intcheck("idx: NULL rejected", dyt_surface_indices(W, H, NULL), -1);
    intcheck("idx: 1x3 is a no-op", dyt_surface_indices(1, H, idx), 0);
}

/* --- normals ------------------------------------------------------------ */

/* Every normal is a unit vector pointing out of the surface (nz > 0). */
static int normals_unit(const dyt_surface_vertex_t *v, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        double len = sqrt((double)v[i].nx * v[i].nx +
                          (double)v[i].ny * v[i].ny +
                          (double)v[i].nz * v[i].nz);
        if (fabs(len - 1.0) > 1e-4 || v[i].nz <= 0.0f)
            return 0;
    }
    return 1;
}

static void test_normals(void)
{
    float                t[NPIX];
    dyt_surface_vertex_t v[NPIX];
    int                  i;

    /* A flat sheet shades as flat: every normal is straight up. */
    build_flat(t);
    dyt_surface_vertices(t, W, H, v);
    int flat_ok = 1;
    for (i = 0; i < NPIX; i++)
        if (fabsf(v[i].nx) > 1e-5f || fabsf(v[i].ny) > 1e-5f ||
            fabsf(v[i].nz - 1.0f) > 1e-5f)
            flat_ok = 0;
    boolcheck("normals: flat sheet is (0,0,1)", flat_ok);

    /* Rising in +x tilts the normal toward -x. */
    build_ramp_x(t);
    dyt_surface_vertices(t, W, H, v);
    boolcheck("normals: +x ramp tilts -x", v[W].nx < -1e-4f);
    floatcheck("normals: +x ramp has no y tilt", v[W].ny, 0.0f, 1e-5f);
    boolcheck("normals: +x ramp still points up", v[W].nz > 0.0f);

    /* Rising in +y tilts the normal toward -y. */
    build_ramp_y(t);
    dyt_surface_vertices(t, W, H, v);
    boolcheck("normals: +y ramp tilts -y", v[W].ny < -1e-4f);
    floatcheck("normals: +y ramp has no x tilt", v[W].nx, 0.0f, 1e-5f);

    /* Mutation-verify: reversing the ramp reverses the tilt.  A sign error in
     * the finite difference would keep the magnitude and lose the direction,
     * which the flat and single-ramp checks above would not catch. */
    build_ramp_x(t);
    for (i = 0; i < NPIX; i++)
        t[i] = -t[i];
    dyt_surface_vertices(t, W, H, v);
    boolcheck("normals: -x ramp tilts +x", v[W].nx > 1e-4f);

    boolcheck("normals: unit length", normals_unit(v, NPIX));

    /* A single hot pixel gives its neighbours relief.  The peak itself is
     * symmetric, so its central difference is zero — the tilt shows one step
     * off the peak, which is exactly where a height map should show a slope.
     * A normal tilts *away* from a rise, so the left neighbour leans toward
     * -x and the right neighbour toward +x. */
    build_flat(t);
    t[1 * W + 1] = 40.0f;             /* one hot pixel at (1,1) */
    dyt_surface_vertices(t, W, H, v);
    boolcheck("normals: hot pixel tilts its left neighbour -x",
              v[1 * W + 0].nx < -1e-4f);
    boolcheck("normals: hot pixel tilts its right neighbour +x",
              v[1 * W + 2].nx > 1e-4f);
    boolcheck("normals: neighbours still point up",
              v[1 * W + 0].nz > 0.0f && v[1 * W + 2].nz > 0.0f);
}

/* --- non-finite samples ------------------------------------------------- */

static void test_nan(void)
{
    float                t[NPIX];
    dyt_surface_vertex_t v[NPIX];
    int                  i;

    /* A NaN sample sits at the grid's finite floor, not at 0 and not as a
     * NaN the renderer would have to guard. */
    build_ramp_x(t);                  /* floor is 0, at x == 0 */
    t[2 * W + 3] = NAN;               /* a NaN in the far corner */
    dyt_surface_vertices(t, W, H, v);
    floatcheck("nan: sits at the finite floor", v[2 * W + 3].z, 0.0f, 1e-6f);

    int all_finite = 1;
    for (i = 0; i < NPIX; i++)
        if (!isfinite(v[i].nx) || !isfinite(v[i].ny) ||
            !isfinite(v[i].nz) || !isfinite(v[i].z))
            all_finite = 0;
    boolcheck("nan: no NaN reaches the mesh", all_finite);
    boolcheck("nan: normals still unit length", normals_unit(v, NPIX));

    /* An all-NaN grid is a flat sheet at 0, not a field of NaNs. */
    for (i = 0; i < NPIX; i++)
        t[i] = NAN;
    dyt_surface_vertices(t, W, H, v);
    floatcheck("nan: all-NaN grid sits at 0", v[0].z, 0.0f, 1e-6f);
    floatcheck("nan: all-NaN grid is flat", v[0].nz, 1.0f, 1e-5f);
}

/* --- the view transform ------------------------------------------------- */

/* Project one mesh-space point and hand back the clip-space x, y. */
static void proj(const float m[16], float x, float y, float z,
                 float *cx, float *cy)
{
    dyt_surface_project(m, x, y, z, cx, cy, NULL);
}

static void test_view(void)
{
    float m[16], cx, cy;
    int   i, j;

    /* The mesh centre is the point the camera orbits, so it lands at the
     * origin of clip space for every camera.  A sign error in the -R*centre
     * translation would put it off-centre and be visible at once. */
    {
        const float yaws[]   = { 0.0f, 0.5f, 1.5f, -2.0f };
        const float pitches[] = { -0.5f, 0.0f, 0.6f };
        int bad = 0;
        for (i = 0; i < 4; i++)
            for (j = 0; j < 3; j++) {
                dyt_surface_mvp(yaws[i], pitches[j], 1.0f, 1.0f, m);
                proj(m, 0.0f, 0.0f, 0.5f, &cx, &cy);
                if (fabsf(cx) > 1e-5f || fabsf(cy) > 1e-5f)
                    bad = 1;
            }
        boolcheck("view: centre stays centred", !bad);
    }

    /* No grid corner is clipped at any camera angle: the ortho box is sized
     * to clear the rotated diagonal. */
    {
        const float yaws[]    = { 0.0f, 0.7f, 1.9f, 3.0f };
        const float pitches[] = { -0.8f, 0.0f, 0.9f };
        const float corners[8][3] = {
            {-1,-1,0}, {1,-1,0}, {-1,1,0}, {1,1,0},
            {-1,-1,1}, {1,-1,1}, {-1,1,1}, {1,1,1}
        };
        int bad = 0;
        for (i = 0; i < 4; i++)
            for (j = 0; j < 3; j++) {
                int c;
                dyt_surface_mvp(yaws[i], pitches[j], 1.0f, 1.0f, m);
                for (c = 0; c < 8; c++) {
                    proj(m, corners[c][0], corners[c][1], corners[c][2],
                         &cx, &cy);
                    if (fabsf(cx) > 1.0f || fabsf(cy) > 1.0f)
                        bad = 1;
                }
            }
        boolcheck("view: no corner is clipped", !bad);
    }

    /* Yaw turns about the height axis: a point on the +x edge swings onto the
     * +y axis by a quarter turn. */
    dyt_surface_mvp(0.0f, 0.0f, 1.0f, 1.0f, m);
    proj(m, 1.0f, 0.0f, 0.5f, &cx, &cy);
    boolcheck("view: +x edge is right of centre", cx > 1e-4f);
    boolcheck("view: +x edge is not up", fabsf(cy) < 1e-4f);

    dyt_surface_mvp((float)(M_PI / 2.0), 0.0f, 1.0f, 1.0f, m);
    proj(m, 1.0f, 0.0f, 0.5f, &cx, &cy);
    boolcheck("view: a quarter turn puts +x edge on +y",
              fabsf(cx) < 1e-4f && cy > 1e-4f);

    /* Aspect scales the horizontal axis only, so a square grid stays square
     * in a wide viewport. */
    dyt_surface_mvp(0.0f, 0.0f, 1.0f, 2.0f, m);
    proj(m, 1.0f, 0.0f, 0.5f, &cx, &cy);
    floatcheck("view: aspect halves clip x", cx, 1.0f / (2.0f * 1.8f), 1e-5f);

    /* Zoom scales both axes: twice the zoom, twice the extent. */
    dyt_surface_mvp(0.0f, 0.0f, 2.0f, 1.0f, m);
    proj(m, 1.0f, 0.0f, 0.5f, &cx, &cy);
    floatcheck("view: zoom doubles clip x", cx, 2.0f / 1.8f, 1e-5f);

    /* A bad zoom or aspect falls back to 1 rather than dividing by zero. */
    dyt_surface_mvp(0.0f, 0.0f, 0.0f, 0.0f, m);
    proj(m, 1.0f, 0.0f, 0.5f, &cx, &cy);
    floatcheck("view: zoom 0 falls back to 1", cx, 1.0f / 1.8f, 1e-5f);

    /* project() is total: a NULL matrix is a no-op, not a crash. */
    dyt_surface_project(NULL, 1.0f, 2.0f, 3.0f, &cx, &cy, NULL);
    ok("view: NULL matrix is a no-op");
}

/* --- build (both outputs at once) --------------------------------------- */

static void test_build(void)
{
    float                t[NPIX];
    dyt_surface_vertex_t v[NPIX];
    uint32_t             idx[NIDX];

    build_ramp_x(t);

    intcheck("build: both ok", dyt_surface_build(t, W, H, v, idx), 0);
    floatcheck("build: vertices written", v[3].z, 3.0f, 1e-6f);
    intcheck("build: indices written", (long)idx[NIDX - 1],
             (long)((H - 1) * W + (W - 1)));

    /* Either output may be skipped. */
    intcheck("build: verts only ok", dyt_surface_build(t, W, H, v, NULL), 0);
    intcheck("build: idx only ok", dyt_surface_build(t, W, H, NULL, idx), 0);

    intcheck("build: both NULL rejected",
             dyt_surface_build(t, W, H, NULL, NULL), -1);
    intcheck("build: NULL temps rejected",
             dyt_surface_build(NULL, W, H, v, idx), -1);
    intcheck("build: bad w rejected",
             dyt_surface_build(t, 0, H, v, idx), -1);
}

int main(void)
{
    printf("=== surface_test (mesh / normals / NaN) ===\n");

    test_counts();
    test_vertices();
    test_indices();
    test_normals();
    test_nan();
    test_view();
    test_build();

    if (fails) {
        printf("=== %d FAILURE(S) ===\n", fails);
        return 1;
    }
    printf("=== ALL PASS ===\n");
    return 0;
}
