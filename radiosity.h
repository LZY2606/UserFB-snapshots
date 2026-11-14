//=====================================================================
//
// radiosity.h - hemicube visibility driven progressive radiosity
//
// C99 interface. The header is safe to include from C++ translation
// units. A scene is built once from immutable triangle patches, its
// transport matrix is computed with a five-face hemicube, and the
// progressive radiosity solution can then be advanced step by step.
//
//=====================================================================
#ifndef RADIOSITY_H
#define RADIOSITY_H

#include <stddef.h>
#include "mini3d.h"

#ifdef __cplusplus
extern "C" {
#endif

// A single diffuse triangle patch. Winding order defines the outward
// normal: normalize(cross(v[1] - v[0], v[2] - v[0])).
typedef struct radiosity_patch {
	point_t v[3];               // world space vertices, w == 1
	color_t reflectance;        // per channel in [0, 1)
	color_t emission;           // emitted radiosity, per channel >= 0
} radiosity_patch;

// Opaque scene holding the copied geometry, transport matrix and the
// progressive radiosity state.
typedef struct radiosity_scene radiosity_scene;

// Deep copy the patches and build the hemicube transport matrix.
// resolution: hemicube resolution, even and >= 4.
// offset: positive world distance pushed along the source normal.
// Returns NULL on invalid input, degenerate patches or allocation
// failure.
radiosity_scene *radiosity_create(const radiosity_patch *patches,
	size_t count, int resolution, float offset);

// Release all memory owned by the scene.
void radiosity_destroy(radiosity_scene *scene);

// Read the discrete form factor F[i][j]. F[i][i] is 0 and every row
// sums to at most 1. Returns 0 for out of range indices.
float radiosity_factor(const radiosity_scene *scene, size_t i, size_t j);

// Run up to shots shooting steps. Each step shoots the patch with the
// largest area * (U.r + U.g + U.b), ties resolved by smallest index,
// and stops early when no unshot energy remains. Returns the number of
// steps actually performed.
size_t radiosity_step(radiosity_scene *scene, size_t shots);

// Read back the state of patch i. Any of B, U, area may be NULL.
// Returns 1 on success, 0 when i is out of range.
int radiosity_get(const radiosity_scene *scene, size_t i,
	color_t *B, color_t *U, float *area);

// Index of the patch that performed the most recent shot, or SIZE_MAX
// when no shot has happened yet.
size_t radiosity_last_shooter(const radiosity_scene *scene);

// Restore B and U to the original emission and clear the shooter
// record. Geometry and the transport matrix are kept.
void radiosity_reset(radiosity_scene *scene);

// Draw the scene through the original color triangle pipeline, using
// the current radiosity B scaled by the positive exposure as a flat
// per-patch vertex color. Only the final byte colors are clamped. The
// caller's transform and render state are restored afterwards and the
// existing color/depth buffers are left untouched.
void radiosity_draw(const radiosity_scene *scene, device_t *device,
	float exposure);

#ifdef __cplusplus
}
#endif

#endif /* RADIOSITY_H */
