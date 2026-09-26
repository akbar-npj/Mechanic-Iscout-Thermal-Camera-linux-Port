/*
 * surface.c — height-mapped mesh over a temperature plane (see surface.h).
 *
 * Three small passes over the caller's grid; nothing allocated, nothing
 * retained.  The pure-C, caller-owns-scratch discipline is the same as
 * measure.c and compare.c.
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I. -c surface.c
 */
#include <math.h>
#include <stddef.h>

#include "surface.h"

long dyt_surface_vertex_count(int w, int h)
{
    if (w <= 0 || h <= 0)
        return 0;
    return (long)w * (long)h;
}

long dyt_surface_index_count(int w, int h)
{
    if (w <= 1 || h <= 1)
        return 0;
    return (long)(w - 1) * (long)(h - 1) * 6;
}

/* The grid's finite floor, used as the height of a non-finite sample.  When
 * nothing is finite the floor is 0, so the mesh is a flat sheet rather than
 * a field of NaNs.  Returns 0 if any finite sample was seen, -1 otherwise. */
static int finite_floor(const float *temps, size_t npix, float *out)
{
    float  floor = 0.0f;
    int    seen  = 0;
    size_t i;

    for (i = 0; i < npix; i++) {
        if (!isfinite(temps[i]))
            continue;
        if (!seen || temps[i] < floor)
            floor = temps[i];
        seen = 1;
    }
    *out = floor;
    return seen ? 0 : -1;
}

/* The grid's finite span, for normalising heights.  *range is 0 when nothing
 * is finite or every finite sample is equal (a flat sheet). */
static void finite_span(const float *temps, size_t npix, float *lo, float *range)
{
    float  zmin = 0.0f, zmax = 0.0f;
    int    seen = 0;
    size_t i;

    for (i = 0; i < npix; i++) {
        float z = temps[i];
        if (!isfinite(z))
            continue;
        if (!seen || z < zmin) zmin = z;
        if (!seen || z > zmax) zmax = z;
        seen = 1;
    }
    *lo    = seen ? zmin : 0.0f;
    *range = seen ? zmax - zmin : 0.0f;
}

/* The normalised height at (i, j): 0..1 across the finite range, with a
 * non-finite sample sitting at the floor.  A flat grid is all zeros, so
 * every normal comes out (0, 0, 1). */
static double hnorm(const float *temps, int w, float floor,
                    float zlo, float zrange, int i, int j)
{
    float z = temps[(size_t)j * (size_t)w + (size_t)i];
    if (!isfinite(z))
        z = floor;
    return zrange > 0.0f ? (double)((z - zlo) / zrange) : 0.0;
}

int dyt_surface_vertices(const float *temps, int w, int h,
                         dyt_surface_vertex_t *verts)
{
    float  floor, zlo, zrange;
    double dx, dy;
    size_t npix;
    int    i, j;

    if (!temps || !verts || w <= 0 || h <= 0)
        return -1;

    npix = (size_t)w * (size_t)h;
    finite_floor(temps, npix, &floor);
    finite_span(temps, npix, &zlo, &zrange);

    dx = (w > 1) ? 2.0 / (double)(w - 1) : 1.0;
    dy = (h > 1) ? 2.0 / (double)(h - 1) : 1.0;

    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            size_t                k = (size_t)j * (size_t)w + (size_t)i;
            float                 z = temps[k];
            dyt_surface_vertex_t *v = &verts[k];
            int                   i0 = i > 0 ? i - 1 : i;
            int                   i1 = i < w - 1 ? i + 1 : i;
            int                   j0 = j > 0 ? j - 1 : j;
            int                   j1 = j < h - 1 ? j + 1 : j;
            double                dzdx, dzdy, nx, ny, nz, len;

            v->x = (w > 1) ? (float)(2.0 * (double)i / (double)(w - 1) - 1.0)
                           : 0.0f;
            v->y = (h > 1) ? (float)(2.0 * (double)j / (double)(h - 1) - 1.0)
                           : 0.0f;
            v->z = isfinite(z) ? z : floor;

            /* Central difference inside, one-sided at the border.  x runs
             * with i, y with j; the surface normal is (-dz/dx, -dz/dy, 1)
             * normalised. */
            dzdx = (hnorm(temps, w, floor, zlo, zrange, i1, j) -
                    hnorm(temps, w, floor, zlo, zrange, i0, j)) /
                   ((double)(i1 - i0) * dx);
            dzdy = (hnorm(temps, w, floor, zlo, zrange, i, j1) -
                    hnorm(temps, w, floor, zlo, zrange, i, j0)) /
                   ((double)(j1 - j0) * dy);
            nx = -dzdx;
            ny = -dzdy;
            nz = 1.0;
            len = sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 0.0) {
                v->nx = (float)(nx / len);
                v->ny = (float)(ny / len);
                v->nz = (float)(nz / len);
            } else {
                v->nx = 0.0f;
                v->ny = 0.0f;
                v->nz = 1.0f;
            }
        }
    }
    return 0;
}

int dyt_surface_indices(int w, int h, uint32_t *idx)
{
    int i, j;
    long k = 0;

    if (!idx || w <= 0 || h <= 0)
        return -1;

    /* Each cell splits along its v01–v10 diagonal into two triangles, so the
     * quad is covered exactly once.  The winding is consistent across the
     * grid; a renderer that culls can flip it, and one that does not never
     * notices. */
    for (j = 0; j + 1 < h; j++) {
        for (i = 0; i + 1 < w; i++) {
            uint32_t v00 = (uint32_t)((size_t)j * (size_t)w + (size_t)i);
            uint32_t v10 = v00 + 1;
            uint32_t v01 = (uint32_t)((size_t)(j + 1) * (size_t)w + (size_t)i);
            uint32_t v11 = v01 + 1;

            idx[k++] = v00; idx[k++] = v01; idx[k++] = v10;
            idx[k++] = v10; idx[k++] = v01; idx[k++] = v11;
        }
    }
    return 0;
}

int dyt_surface_build(const float *temps, int w, int h,
                      dyt_surface_vertex_t *verts, uint32_t *idx)
{
    if (!temps || w <= 0 || h <= 0)
        return -1;
    if (!verts && !idx)
        return -1;
    if (verts && dyt_surface_vertices(temps, w, h, verts) != 0)
        return -1;
    if (idx && dyt_surface_indices(w, h, idx) != 0)
        return -1;
    return 0;
}

/* --------------------------------------------------------------- the view */

/* Half the world extent the ortho box maps to NDC ±1.  1.8 clears the
 * diagonal of the unit-ish mesh box (the far corner is sqrt(1² + 1² + 1²) ≈
 * 1.73) at every camera angle, so no grid corner is ever clipped. */
#define DYT_SURFACE_HALF 1.8f

void dyt_surface_mvp(float yaw, float pitch, float zoom, float aspect,
                     float out[16])
{
    float cz, sz, cp, sp, z, sx, sy, sz_scale;
    /* The rotation R = Rx(pitch) * Rz(yaw), row-major.  The mesh centre is
     * (0, 0, 0.5), so the view matrix translates it to the origin before
     * rotating: V = R * T(-centre), whose translation column is -R*centre. */
    float r00, r01, r02, r10, r11, r12, r20, r21, r22;
    float t0, t1, t2;

    if (!out)
        return;

    if (!(zoom > 0.0f))
        zoom = 1.0f;
    if (!(aspect > 0.0f))
        aspect = 1.0f;

    cz = cosf(yaw);   sz = sinf(yaw);
    cp = cosf(pitch); sp = sinf(pitch);

    r00 = cz;        r01 = -sz;       r02 = 0.0f;
    r10 = cp * sz;   r11 = cp * cz;   r12 = -sp;
    r20 = sp * sz;   r21 = sp * cz;   r22 = cp;

    t0 = -r02 * 0.5f;
    t1 = -r12 * 0.5f;
    t2 = -r22 * 0.5f;

    /* The projection is diagonal, so P*V just scales V's rows.  sx carries
     * the aspect so a square grid stays square in a non-square viewport. */
    z       = DYT_SURFACE_HALF / zoom;
    sx      = 1.0f / (z * aspect);
    sy      = 1.0f / z;
    sz_scale = 1.0f / (2.0f * z);

    out[0]  = sx * r00; out[4]  = sx * r01; out[8]  = sx * r02; out[12] = sx * t0;
    out[1]  = sy * r10; out[5]  = sy * r11; out[9]  = sy * r12; out[13] = sy * t1;
    out[2]  = sz_scale * r20; out[6]  = sz_scale * r21;
    out[10] = sz_scale * r22; out[14] = sz_scale * t2;
    out[3]  = 0.0f;     out[7]  = 0.0f;     out[11] = 0.0f;     out[15] = 1.0f;
}

void dyt_surface_project(const float mvp[16], float x, float y, float z,
                         float *cx, float *cy, float *cz)
{
    if (!mvp)
        return;
    if (cx)
        *cx = mvp[0] * x + mvp[4] * y + mvp[8]  * z + mvp[12];
    if (cy)
        *cy = mvp[1] * x + mvp[5] * y + mvp[9]  * z + mvp[13];
    if (cz)
        *cz = mvp[2] * x + mvp[6] * y + mvp[10] * z + mvp[14];
}
