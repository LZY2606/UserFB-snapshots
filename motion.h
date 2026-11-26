#ifndef MOTION_H
#define MOTION_H

#include <stddef.h>
#include "mini3d.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	vertex_t open[3];
	vertex_t close[3];
} motion_triangle;

/*
 * Integrate the continuously exposed image over t in [0,1].
 * rgb must contain width*height*3 doubles.  Returns 1 on success and
 * 0 when pointers are invalid or temporary allocation fails.
 */
int motion_render(device_t *device, const motion_triangle *triangles,
	size_t count, double *rgb);

#ifdef __cplusplus
}
#endif

#endif
