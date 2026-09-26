/*
 * surface.h — height-mapped mesh over a temperature plane, for the 3D
 * Analysis view.
 *
 * The vendor app's "3D Analysis" tab draws the thermal grid as a landscape:
 * temperature is height, the grid's own x/y are the ground plane.  This
 * module is the pure geometry behind that view — it turns the Celsius plane
 * into a triangle-list mesh (positions + finite-difference normals + an index
 * list) with no camera, no GL and no GUI.  The widget uploads what this
 * builds; a front end with no GL context still gets a valid mesh to describe
 * or test.
 *
 * Layout: vertex (i, j) is row-major, index j*w + i, matching the plane's own
 * order.  x runs left-to-right and y top-to-bottom, both mapped to [-1, 1] so
 * the mesh is device-independent and the widget picks the aspect; z is the
 * temperature in Celsius, so a caller can read a vertex's height straight
 * back.  The widget applies whatever vertical exaggeration it wants at draw
 * time.
 *
 * Normals come from central differences on the *normalised* height field
 * (z scaled to [0, 1] across the grid's finite range), with one-sided
 * differences at the border.  Normalising matters: with z in Celsius and x/y
 * in [-1, 1] a raw dz/dx would be hundreds, every normal would lie almost
 * flat, and the surface would shade like a wall rather than a landscape.
 * The cost is that the shading depends on the grid's min/max — a global
 * property — which is exactly what makes the picture legible.
 *
 * Non-finite samples (the pipeline's non-physical marker) get z == the grid's
 * finite minimum, so an unknown pixel sits at the surface's base instead of
 * poking through it; the normals treat it as that floor.
 *
 * Pure: no camera, no GUI, no allocation of its own.  The caller owns the
 * vertex and index buffers; use dyt_surface_vertex_count() and
 * dyt_surface_index_count() to size them.
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I. -c surface.c
 */
#ifndef DYT_SURFACE_H
#define DYT_SURFACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One mesh vertex.  z is the temperature in Celsius (the grid's finite floor
 * for a non-finite sample); the normal is a unit vector. */
typedef struct {
    float x, y, z;      /* position: x,y in [-1, 1]; z in Celsius */
    float nx, ny, nz;   /* unit normal */
} dyt_surface_vertex_t;

/* How many vertices a w*h grid needs. */
long dyt_surface_vertex_count(int w, int h);

/* How many indices the triangle list needs: (w-1)*(h-1)*6, two triangles per
 * grid cell.  0 when either dimension is below 2 (no cells to triangulate). */
long dyt_surface_index_count(int w, int h);

/* Fill `verts` (w*h entries) with positions and normals.  Returns 0 on
 * success, -1 on a bad argument. */
int dyt_surface_vertices(const float *temps, int w, int h,
                         dyt_surface_vertex_t *verts);

/* Fill `idx` (dyt_surface_index_count(w, h) entries) with the triangle list,
 * two counter-clockwise triangles per cell.  Returns 0 on success, -1 on a
 * bad argument. */
int dyt_surface_indices(int w, int h, uint32_t *idx);

/* Both at once: `verts` holds w*h entries, `idx` holds
 * dyt_surface_index_count(w, h).  Either output may be NULL to skip it.
 * Returns 0 on success, -1 on a bad argument (a NULL output that was asked
 * for, or w/h <= 0). */
int dyt_surface_build(const float *temps, int w, int h,
                      dyt_surface_vertex_t *verts, uint32_t *idx);

/* --------------------------------------------------------------- the view */

/* Column-major 4x4 model-view-projection for an orthographic orbit camera
 * looking at the mesh's centre (0, 0, 0.5).  `yaw` turns the mesh about the
 * height axis, `pitch` tilts it toward the viewer, `zoom` scales it (1.0 fits
 * the whole grid with margin), and `aspect` is the viewport's width/height so
 * a square grid stays square.  The mesh spans x,y in [-1, 1] and z in [0, 1].
 *
 * Orthographic, not perspective: a data surface is an overview, and the
 * parallel projection has no near plane to clip a peak through and no divide
 * to guard — which is also what lets dyt_surface_project() be total.
 *
 * The matrix is the same one the GL path uploads as a uniform and the
 * software path multiplies by hand, so the two renderers cannot disagree
 * about where a vertex lands. */
void dyt_surface_mvp(float yaw, float pitch, float zoom, float aspect,
                     float out[16]);

/* Transform one mesh-space point by `mvp`, writing clip-space coordinates.
 * With the orthographic matrix above there is no perspective divide to do:
 * map (cx, cy) from [-1, 1] to the viewport, and use cz for a painter's
 * sort.  Total — no input can make it fail. */
void dyt_surface_project(const float mvp[16], float x, float y, float z,
                         float *cx, float *cy, float *cz);

#ifdef __cplusplus
}
#endif

#endif /* DYT_SURFACE_H */
