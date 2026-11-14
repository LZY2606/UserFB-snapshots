//=====================================================================
//
// motion.h - offline continuous-shutter (motion blur) rendering
//
// A motion_triangle describes one triangle whose three vertices move
// linearly in world space while the virtual shutter is open, t in
// [0,1].  Vertices with the same index in open[] and close[] are the
// same vertex at the two ends of the exposure.  A vertex RGB is
// constant during the exposure and taken from open[].
//
// motion_render integrates, per pixel, the colour actually visible
// over the whole exposure instead of drawing isolated instants.
// Visibility changes are found from the exact event times at which
// an edge crosses a pixel centre, a projected triangle flips, or two
// covering triangles exchange depth; no fixed time sampling is used.
//
//=====================================================================
#ifndef MOTION_H
#define MOTION_H

#include <stddef.h>
#include "mini3d.h"

#ifdef __cplusplus
extern "C" {
#endif

// One moving triangle: open = shutter-open endpoints, close =
// shutter-close endpoints.  open[k] and close[k] share vertex k.
typedef struct {
	vertex_t open[3];
	vertex_t close[3];
} motion_triangle;

// Render the exposure of triangles[] into rgb (width*height*3 doubles,
// row-major, time integrals in [0,1]) and then quantise the result
// once into device->framebuffer.  Returns 1 on success, 0 on an
// invalid pointer or allocation failure (nothing partial is
// published in that case).  The device, its matrices, render_state,
// texture and zbuffer, and the input array are never modified.
int motion_render(device_t *device, const motion_triangle *triangles,
	size_t count, double *rgb);

#ifdef __cplusplus
}
#endif

#endif /* MOTION_H */
