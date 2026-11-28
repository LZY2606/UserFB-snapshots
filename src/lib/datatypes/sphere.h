//
//  sphere.h
//  c-ray
//
//  Created by Valtteri Koskivuori on 28/02/2015.
//  Copyright © 2015-2025 Valtteri Koskivuori. All rights reserved.
//

#pragma once

#include <v.h>
#include <datatypes/lightray.h>
#include <datatypes/hitrecord.h>

struct sphere {
	float radius;
};

bool rayIntersectsWithSphere(const struct lightRay *ray, const struct sphere *sphere, struct hitRecord *isect);
