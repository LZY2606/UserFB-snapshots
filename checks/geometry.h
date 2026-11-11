//----------------------------------------------------------------------------
// Tiny, explicit geometry inputs for the compositing regression.
//
// These coordinates are the only geometry the rasterizer is fed.  The
// expected covers below are geometric facts, not values taken from the
// library:
//
//  * The half-pixel rectangle [2,6] x [3.5,5.5] covers exactly half of the
//    top and bottom pixel rows (cover 128) and the full middle row
//    (cover 255).  Every covered cell uses one of these explicit constants.
//
//  * The sloped triangle is compared against independently computed
//    polygon-vs-pixel-square intersection areas (see reference.h); the
//    rasterizer's own covers are never treated as ground truth.
//----------------------------------------------------------------------------
#ifndef CHECKS_GEOMETRY_H
#define CHECKS_GEOMETRY_H

#include "reference.h"

namespace geom
{
    // Half-pixel rectangle, in pixel coordinates.
    inline ref::polygon half_pixel_rectangle()
    {
        ref::polygon p;
        p.push_back((ref::dpoint){ 2.0, 3.5 });
        p.push_back((ref::dpoint){ 6.0, 3.5 });
        p.push_back((ref::dpoint){ 6.0, 5.5 });
        p.push_back((ref::dpoint){ 2.0, 5.5 });
        return p;
    }

    // Explicit per-row cover constants for the rectangle cells.
    enum rect_cover_e
    {
        rect_edge_cover = 128,
        rect_full_cover = 255
    };

    inline int rectangle_cover(int /*cx*/, int cy)
    {
        return (cy == 4) ? rect_full_cover : rect_edge_cover;
    }

    // Sloped triangle with two anti-aliased edges.
    inline ref::polygon sloped_triangle()
    {
        ref::polygon p;
        p.push_back((ref::dpoint){ 2.0, 1.0 });
        p.push_back((ref::dpoint){ 9.0, 2.0 });
        p.push_back((ref::dpoint){ 4.0, 8.0 });
        return p;
    }

    // Bounding boxes (inclusive pixel ranges) used to size the surfaces.
    enum
    {
        rect_w = 10, rect_h = 9,
        tri_w  = 12, tri_h  = 10
    };
}

#endif
