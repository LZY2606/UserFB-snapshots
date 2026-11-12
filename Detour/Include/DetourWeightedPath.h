//
// Copyright (c) 2009-2010 Mikko Mononen memon@inside.org
//
// This software is provided 'as-is', without any express or implied
// warranty.  In no event will the authors be held liable for any damages
// arising from the use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.
//

#ifndef DETOURWEIGHTEDPATH_H
#define DETOURWEIGHTEDPATH_H

#include "DetourStatus.h"

/// Result of a continuous weighted corridor straightening query
/// (#dtNavMeshQuery::findWeightedPath).
///
/// The arrays are owned by the caller. On failure the counts and costs are
/// cleared; the array contents are unspecified.
/// @ingroup detour
struct dtWeightedPathResult
{
	/// Caller-provided polyline points. [(x, y, z) * maxPoints]
	/// Always holds pathSize+1 entries on a completed query: the start point,
	/// one crossing point per portal in corridor order, and the end point.
	double* points;
	int maxPoints;		///< Capacity of the points array.
	int pointCount;		///< Number of points written.

	/// Caller-provided dual certificate vectors, one per corridor polygon.
	/// [(x, y, z) * maxDual] Each vector has Euclidean norm no greater than
	/// the area cost of its polygon.
	double* dual;
	int maxDual;		///< Capacity of the dual array.
	int dualCount;		///< Number of vectors written.

	/// Weighted length of the returned polyline (upper bound).
	double upperCost;
	/// Dual lower bound on the global minimum of the corridor problem.
	double lowerCost;
};

#endif // DETOURWEIGHTEDPATH_H
