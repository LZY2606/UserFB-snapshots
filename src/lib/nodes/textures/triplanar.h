//
//  triplanar.h
//  c-ray
//
//  Created by Valtteri Koskivuori on 14/11/2026.
//  Copyright © 2026 Valtteri Koskivuori. All rights reserved.
//

#pragma once

struct node_storage;
struct colorNode;

const struct colorNode *newTriplanar(const struct node_storage *s,
									 const struct colorNode *source,
									 const float world_to_texture[4][4],
									 float sharpness);
