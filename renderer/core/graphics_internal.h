#ifndef GRAPHICS_INTERNAL_H
#define GRAPHICS_INTERNAL_H

#include "graphics.h"

/*
 * fragment commit path shared by every primitive (triangles, ellipsoids)
 *
 * both primitive families perform the exact same per-fragment operations:
 * the <= depth comparison against the depth buffer, color saturation,
 * RGB alpha blending (leaving the target alpha byte untouched), and the
 * final color/depth buffer writes.  keeping these operations in one place
 * prevents the blending and visibility rules from diverging.
 */

/* returns non-zero if the fragment passes the shared depth comparison */
int graphics_test_depth(framebuffer_t *framebuffer, int index, float depth);

/* saturates, blends, and writes color and depth for an accepted fragment */
void graphics_commit_fragment(framebuffer_t *framebuffer, int index,
                              float depth, vec4_t color, int enable_blend);

#endif
