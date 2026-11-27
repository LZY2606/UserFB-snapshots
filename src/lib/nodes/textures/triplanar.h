//
//  triplanar.h
//  c-ray
//
//  Copyright © 2020-2025 Valtteri Koskivuori. All rights reserved.
//

#pragma once

struct node_storage;
struct colorNode;

// Triplanar projection of a source color node. `world_to_texture` is a
// row-major affine matrix mapping world hit points to texture space; pass
// NULL (or an all-zero matrix) for identity. `sharpness` is clamped to
// [1, 64], pass 0 for the default of 1.
const struct colorNode *newTriplanarTexture(const struct node_storage *s, const struct colorNode *source, const float world_to_texture[4][4], float sharpness);
