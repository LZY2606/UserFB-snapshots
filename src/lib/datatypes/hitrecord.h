//
//  hitrecord.h
//  c-ray
//
//  Created by Valtteri Koskivuori on 06/12/2020.
//  Copyright © 2020-2025 Valtteri Koskivuori. All rights reserved.
//

#pragma once

#include <common/vector.h>
#include "lightray.h"

struct hitRecord {
	struct lightRay *incident;		//Incident ray
	struct vector hitPoint;			//Hit point vector in world space
	struct vector surfaceNormal;	//Surface normal at that point of intersection, possibly smoothed and oriented toward the incident ray
	struct vector geometricNormal;	//Unit, outward-facing geometric normal in world space. Never smoothed or flipped.
	struct vector pointError;		//Per-axis conservative upper bounds on the world-space position error of hitPoint, always >= 0
	struct coord uv;				//UV barycentric coordinates for intersection point
	const struct bsdfNode *bsdf;	//Surface properties of the intersected object
	struct poly *polygon;			//ptr to polygon that was encountered
	float distance;					//Ray parameter t of the hit on the *original* (world-space) ray
	int instIndex;					//Instance index, negative if no intersection
};
