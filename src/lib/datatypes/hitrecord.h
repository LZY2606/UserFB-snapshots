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
	struct vector surfaceNormal;	//Shading normal at that point of intersection
	// Unit geometric normal in world space. Unlike surfaceNormal this is
	// never smoothed/interpolated and never flipped towards the incident
	// ray. It defines which side of the surface a hit belongs to.
	struct vector geometricNormal;
	// Conservative per-coordinate absolute position error of hitPoint.
	// Every component is non-negative and bounds the difference between
	// hitPoint and the exact intersection point.
	struct vector pointError;
	struct coord uv;				//UV barycentric coordinates for intersection point
	const struct bsdfNode *bsdf;	//Surface properties of the intersected object
	struct poly *polygon;			//ptr to polygon that was encountered
	float distance;					//Distance (t) along the original incident ray
	int instIndex;					//Instance index, negative if no intersection
};
