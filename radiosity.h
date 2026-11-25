//=====================================================================
//
// radiosity.h - progressive radiosity over small triangle scenes
//
// A "patch" is one emissive/reflective triangle in world space.  The
// solver builds a transport matrix F[i][j] from patch i (source) to
// patch j (receiver) with a per-patch hemicube sampled through the
// original Mini3D transform / trapezoid / scanline pipeline, then
// performs progressive (shooting) radiosity with a float energy budget.
//
// The geometry is immutable once radiosity_create() returns.  There is
// no global scene state: every function takes an explicit scene handle.
//
// The interface is C99.  It can be consumed from C++ as-is (every
// declaration is inside an extern "C" block and radiosity_scene is an
// opaque type).
//
//=====================================================================
#ifndef RADIOSITY_H
#define RADIOSITY_H

#include <stddef.h>
#include "mini3d.h"

#ifdef __cplusplus
extern "C" {
#endif

// One surface patch: world-space triangle, per-channel reflectance in
// [0,1) and per-channel emitted radiosity (>= 0).  Winding defines the
// outward normal: n = normalize(cross(v1 - v0, v2 - v0)).
typedef struct {
	point_t v[3];
	color_t reflectance;
	color_t emission;
} radiosity_patch;

// Opaque solver state: geometry, transfer matrix and current B/U/area.
typedef struct radiosity_scene radiosity_scene;

// Build a scene from `count` patches.  The patches are deep-copied and
// the transfer matrix is computed with a hemicube of `resolution`
// pixels per full face (top face is resolution x resolution, side faces
// are resolution x resolution/2).  `offset` is a positive world-space
// distance used both to push the hemicube origin off its own patch and
// as the near clipping distance.  Returns NULL on invalid input,
// degenerate patches or allocation failure.
radiosity_scene *radiosity_create(const radiosity_patch *patches,
	size_t count, int resolution, float offset);

// Release a scene created by radiosity_create().
void radiosity_destroy(radiosity_scene *scene);

// Transfer coefficient from source patch i to receiver patch j, where
// F[i][j] is the fraction of the total energy shot by i that lands on
// j.  Returns 0.0f when either index is out of range or for F[i][i].
float radiosity_factor(const radiosity_scene *scene, size_t i, size_t j);

// Run at most `shots` shooting steps.  Every step picks the patch with
// the largest remaining unshot energy Ai*(Ur+Ug+Ub), ties resolved by
// the smallest index, and distributes it.  Returns the number of steps
// actually performed (0 when no unshot energy remains).
size_t radiosity_step(radiosity_scene *scene, size_t shots);

// Read back the state of patch i.  Returns 1 on success and writes the
// accumulated radiosity B, the unshot radiosity U and the patch area.
// Returns 0 for an out-of-range index.
int radiosity_get(const radiosity_scene *scene, size_t i,
	color_t *B, color_t *U, float *area);

// Index of the most recent shooting patch, or SIZE_MAX if no step has
// been performed yet (including after radiosity_reset()).
size_t radiosity_last_shooter(const radiosity_scene *scene);

// Restore B/U to the emission values and clear the shooting record.
// Geometry and the transfer matrix are preserved.
void radiosity_reset(radiosity_scene *scene);

// Draw the current B * exposure as flat vertex colors through the
// original triangle pipeline.  This only reads the scene; the caller's
// device transform / render state are restored before returning.  The
// caller's color and depth buffers are not cleared.
void radiosity_draw(const radiosity_scene *scene, device_t *device,
	float exposure);

#ifdef __cplusplus
}
#endif

#endif /* RADIOSITY_H */
