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

#include "DetourWeightedPath.h"
#include "DetourNavMeshQuery.h"
#include "DetourNavMesh.h"
#include "DetourAlloc.h"

#include <math.h>
#include <float.h>

namespace
{
	inline void vcopy(double* dst, const double* src)
	{
		dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
	}

	inline void vsub(double* dst, const double* a, const double* b)
	{
		dst[0] = a[0]-b[0]; dst[1] = a[1]-b[1]; dst[2] = a[2]-b[2];
	}

	inline double vdot(const double* a, const double* b)
	{
		return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
	}

	inline void vlerp(double* dst, const double* a, const double* b, const double t)
	{
		dst[0] = a[0] + (b[0]-a[0])*t;
		dst[1] = a[1] + (b[1]-a[1])*t;
		dst[2] = a[2] + (b[2]-a[2])*t;
	}

	// Portal parameterization p = a + t*h where h = b - a, t in [0,1].
	inline void vparam(double* dst, const double* a, const double* h, const double t)
	{
		dst[0] = a[0] + h[0]*t;
		dst[1] = a[1] + h[1]*t;
		dst[2] = a[2] + h[2]*t;
	}

	inline bool finiteDouble(const double x)
	{
		return x > -DBL_MAX && x < DBL_MAX;
	}

	inline double clampUnit(const double t)
	{
		if (t < 0.0) return 0.0;
		if (t > 1.0) return 1.0;
		return t;
	}

	// Clears the public result counters and costs after a failed query.
	void clearResult(dtWeightedPathResult* result)
	{
		result->pointCount = 0;
		result->dualCount = 0;
		result->upperCost = 0.0;
		result->lowerCost = 0.0;
	}

	// Exact weighted length of the polyline. Zero-length segments contribute
	// zero and never require a normalized (NaN) direction.
	double polylineCost(const double* points, const int nseg, const double* weights)
	{
		double cost = 0.0;
		double d[3];
		for (int i = 0; i < nseg; ++i)
		{
			vsub(d, &points[(i+1)*3], &points[i*3]);
			cost += weights[i] * sqrt(vdot(d, d));
		}
		return cost;
	}

	// Smoothed weighted length used by the primal engine; the published upper
	// bound always uses the exact polylineCost().
	double smoothedCost(const double* t, const int m,
						const double* portalA, const double* portalH,
						const double* start, const double* end,
						const double* weights, const int npoly,
						const double delta)
	{
		double prev[3], cur[3], d[3];
		vcopy(prev, start);
		double cost = 0.0;
		for (int j = 0; j < m; ++j)
		{
			vparam(cur, &portalA[j*3], &portalH[j*3], t[j]);
			vsub(d, cur, prev);
			cost += weights[j] * sqrt(vdot(d, d) + delta);
			vcopy(prev, cur);
		}
		vsub(d, end, prev);
		cost += weights[npoly-1] * sqrt(vdot(d, d) + delta);
		return cost;
	}

	// Gradient of the smoothed weighted objective with respect to the portal
	// parameters t. The caller-provided points buffer (npoly+1 entries) is
	// used to materialize the polyline. The delta smoothing keeps the
	// gradient finite at zero-length segments; exact kink resolution is
	// handled by the continuation schedule and golden-section sweeps.
	void smoothedGradient(const double* t, const int m,
						  const double* portalA, const double* portalH,
						  const double* start, const double* end,
						  const double* weights, const int npoly,
						  const double delta, double* points, double* grad)
	{
		vcopy(&points[0], start);
		for (int j = 0; j < m; ++j)
			vparam(&points[(j+1)*3], &portalA[j*3], &portalH[j*3], t[j]);
		vcopy(&points[npoly*3], end);

		double d0[3], d1[3];
		for (int j = 0; j < m; ++j)
		{
			vsub(d0, &points[(j+1)*3], &points[j*3]);
			const double s0 = sqrt(vdot(d0, d0) + delta);
			double g = weights[j] * vdot(&portalH[j*3], d0) / s0;
			if (j + 1 < npoly)
			{
				vsub(d1, &points[(j+2)*3], &points[(j+1)*3]);
				const double s1 = sqrt(vdot(d1, d1) + delta);
				g -= weights[j+1] * vdot(&portalH[j*3], d1) / s1;
			}
			grad[j] = g;
		}
	}

	// Minimizes the two weighted segment lengths touching portal j over t in
	// [0,1]. Golden-section search on the convex 1D function itself is used so
	// a zero-length (kinked) touching segment cannot stall the sweep.
	double minimizePortal(const int j, const double* points, const double* weights,
						  const double* portalA, const double* portalH)
	{
		const double* a = &portalA[j*3];
		const double* h = &portalH[j*3];
		const double* prev = &points[j*3];
		const double* next = &points[(j+2)*3];
		const double w0 = weights[j];
		const double w1 = weights[j+1];

		double p[3], d0[3], d1[3];
		double lo = 0.0, hi = 1.0;
		const double gr = 0.6180339887498949;
		double x1 = hi - gr*(hi-lo);
		double x2 = lo + gr*(hi-lo);
		double f1 = 0.0, f2 = 0.0;
		for (int probe = 0; probe < 2; ++probe)
		{
			const double x = probe == 0 ? x1 : x2;
			vparam(p, a, h, x);
			vsub(d0, p, prev);
			vsub(d1, next, p);
			const double f = w0*sqrt(vdot(d0, d0)) + w1*sqrt(vdot(d1, d1));
			if (probe == 0) f1 = f; else f2 = f;
		}
		for (int iter = 0; iter < 48; ++iter)
		{
			if (f1 < f2)
			{
				hi = x2; x2 = x1; f2 = f1;
				x1 = hi - gr*(hi-lo);
				vparam(p, a, h, x1);
				vsub(d0, p, prev); vsub(d1, next, p);
				f1 = w0*sqrt(vdot(d0, d0)) + w1*sqrt(vdot(d1, d1));
			}
			else
			{
				lo = x1; x1 = x2; f1 = f2;
				x2 = lo + gr*(hi-lo);
				vparam(p, a, h, x2);
				vsub(d0, p, prev); vsub(d1, next, p);
				f2 = w0*sqrt(vdot(d0, d0)) + w1*sqrt(vdot(d1, d1));
			}
		}
		return 0.5 * (lo + hi);
	}

	// Exact hard-min dual lower bound for a feasible vector set:
	// D = q_{n-1}.p_n - q_0.p_0 + sum_j min(d_j.a_j, d_j.b_j), d_j = q_{j-1}-q_j.
	double exactDual(const double* q, const double* points,
					 const double* portalA, const double* portalB,
					 const int npoly)
	{
		const int nportal = npoly - 1;
		double dval = vdot(&q[(npoly-1)*3], &points[npoly*3]) - vdot(&q[0], &points[0]);
		double diff[3];
		for (int j = 0; j < nportal; ++j)
		{
			diff[0] = q[j*3+0] - q[(j+1)*3+0];
			diff[1] = q[j*3+1] - q[(j+1)*3+1];
			diff[2] = q[j*3+2] - q[(j+1)*3+2];
			const double ca = vdot(diff, &portalA[j*3]);
			const double cb = vdot(diff, &portalB[j*3]);
			dval += ca < cb ? ca : cb;
		}
		return dval;
	}

	// Numerically stable soft minimum: softmin_mu(x,y) <= min(x,y), and it
	// converges to min(x,y) as mu -> 0.
	double softMin(const double x, const double y, const double mu)
	{
		const double mn = x < y ? x : y;
		return mn - mu * log1p(exp(-fabs(y-x)/mu));
	}

	// Smoothed dual objective (maximized). It is a smooth under-approximation
	// of the exact hard-min bound; the published lower bound always uses
	// exactDual() on the returned vectors.
	double smoothedDualValue(const double* q, const double* points,
							 const double* portalA, const double* portalB,
							 const int npoly, const double mu)
	{
		const int nportal = npoly - 1;
		double dval = vdot(&q[(npoly-1)*3], &points[npoly*3]) - vdot(&q[0], &points[0]);
		for (int j = 0; j < nportal; ++j)
		{
			double c[3];
			c[0] = q[j*3+0] - q[(j+1)*3+0];
			c[1] = q[j*3+1] - q[(j+1)*3+1];
			c[2] = q[j*3+2] - q[(j+1)*3+2];
			const double x = vdot(c, &portalA[j*3]);
			const double y = vdot(c, &portalB[j*3]);
			dval += softMin(x, y, mu);
		}
		return dval;
	}

	// Ascent gradient of the smoothed dual objective. Verified against finite
	// differences: the softmin gradient in c = q_j - q_{j+1} is
	// s*a + (1-s)*b with s = sigmoid((y-x)/mu), applied + on q_j and - on
	// q_{j+1}, together with the endpoint terms.
	void smoothedDualGrad(const double* q, const double* points,
						  const double* portalA, const double* portalB,
						  const int npoly, const double mu, double* g)
	{
		const int nv = 3 * npoly;
		for (int i = 0; i < nv; ++i) g[i] = 0.0;

		// Endpoint terms of D: -q_0.p_0 + q_{n-1}.p_n.
		g[0] -= points[0]; g[1] -= points[1]; g[2] -= points[2];
		g[(npoly-1)*3+0] += points[npoly*3+0];
		g[(npoly-1)*3+1] += points[npoly*3+1];
		g[(npoly-1)*3+2] += points[npoly*3+2];

		for (int j = 0; j < npoly-1; ++j)
		{
			double c[3];
			c[0] = q[j*3+0] - q[(j+1)*3+0];
			c[1] = q[j*3+1] - q[(j+1)*3+1];
			c[2] = q[j*3+2] - q[(j+1)*3+2];
			const double* a = &portalA[j*3];
			const double* b = &portalB[j*3];
			const double x = vdot(c, a);
			const double y = vdot(c, b);
			const double z = (y - x) / mu;
			double s;
			if (z >= 0.0) s = 1.0 / (1.0 + exp(-z));
			else { const double ez = exp(z); s = ez / (1.0 + ez); }
			for (int k = 0; k < 3; ++k)
			{
				const double gk = s*a[k] + (1.0-s)*b[k];
				g[j*3+k] += gk;
				g[(j+1)*3+k] -= gk;
			}
		}
	}

	// Projects every vector q_i back onto its norm ball |q_i| <= w_i.
	void projectDualBalls(double* q, const double* weights, const int npoly)
	{
		for (int i = 0; i < npoly; ++i)
		{
			const double nrm = sqrt(vdot(&q[i*3], &q[i*3]));
			const double radius = weights[i];
			if (nrm > radius && nrm > 0.0)
			{
				const double sc = radius / nrm;
				q[i*3+0] *= sc; q[i*3+1] *= sc; q[i*3+2] *= sc;
			}
		}
	}

	// Continuation FISTA maximizing the smoothed dual over the product of
	// norm balls. The smoothing parameter is annealed geometrically so early
	// stages move quickly and late stages refine to the exact piecewise-linear
	// optimum. Every published bound is the exact hard-min value.
	//
	// q is the working vector set (feasible on entry); bestDual keeps the
	// feasible vectors attaining the largest exact bound seen. Returns the
	// number of iterations consumed.
	int dualAscent(const double* points, const double* portalA, const double* portalB,
				   const double* weights, const int npoly,
				   const int iters, const double upperCost,
				   const double absoluteTolerance,
				   double* q, double* bestDual,
				   double* yk, double* xPrev, double* grad, double& bestD)
	{
		if (iters <= 0) return 0;
		const int nv = 3 * npoly;
		for (int i = 0; i < nv; ++i)
		{
			yk[i] = q[i];
			xPrev[i] = q[i];
		}

		// Geometric continuation: mu starts at an O(1) scale and shrinks by
		// a fixed ratio until it is a small fraction of the requested
		// tolerance. The number of stages follows from that range, and the
		// available iterations are distributed evenly across them.
		const double muStart = 0.5;
		const double muRatio = 0.33;
		const double muTarget = absoluteTolerance / (double)(8 * (npoly > 1 ? npoly : 2));
		int stages = 1;
		{
			double m = muStart;
			while (m > muTarget && stages < 16) { m *= muRatio; ++stages; }
		}
		const int perStage = iters / stages;
		int used = 0;
		double mu = muStart;

		for (int stage = 0; stage < stages && used < iters; ++stage)
		{
			const int budget = (stage == stages-1) ? (iters - used) : perStage;
			double momentum = 1.0;
			double lip = 1.0;
			for (int k = 0; k < budget; ++k)
			{
				smoothedDualGrad(yk, points, portalA, portalB, npoly, mu, grad);
				const double fY = smoothedDualValue(yk, points, portalA, portalB, npoly, mu);
				double step = 1.0 / lip;
				for (int back = 0; back < 40; ++back)
				{
					for (int i = 0; i < nv; ++i) q[i] = yk[i] + step*grad[i];
					projectDualBalls(q, weights, npoly);
					double dist2 = 0.0, quad = 0.0;
					for (int i = 0; i < nv; ++i)
					{
						const double dz = q[i] - yk[i];
						dist2 += dz*dz;
						quad += grad[i]*dz;
					}
					const double fQ = smoothedDualValue(q, points, portalA, portalB, npoly, mu);
					// Ascent sufficient-increase condition.
					if (fQ >= fY + quad - 0.5*lip*dist2 - 1e-12*(1.0+fabs(fY)))
						break;
					lip *= 2.0;
					step = 1.0 / lip;
				}

				const double nextMomentum = 0.5*(1.0 + sqrt(1.0 + 4.0*momentum*momentum));
				const double gamma = (momentum - 1.0) / nextMomentum;
				for (int i = 0; i < nv; ++i)
					yk[i] = q[i] + gamma*(q[i] - xPrev[i]);
				projectDualBalls(yk, weights, npoly);
				for (int i = 0; i < nv; ++i) xPrev[i] = q[i];
				momentum = nextMomentum;
				lip *= 0.95;
				if (lip < 1e-12) lip = 1e-12;

				// Track the best exact certificate at every step; the hard-min
				// evaluation is cheap and the best vectors are retained.
				const double dval = exactDual(q, points, portalA, portalB, npoly);
				if (dval > bestD)
				{
					bestD = dval;
					for (int i = 0; i < nv; ++i) bestDual[i] = q[i];
				}
				if (upperCost - bestD <= absoluteTolerance)
					return used + k + 1;
			}
			used += budget;

			if (upperCost - bestD <= absoluteTolerance)
				return used;

			// Refine the smoothing and restart momentum.
			mu *= muRatio;
			if (mu < muTarget) mu = muTarget;
			momentum = 1.0;
			for (int i = 0; i < nv; ++i)
			{
				xPrev[i] = q[i];
				yk[i] = q[i];
			}
		}
		return used;
	}
}

dtStatus dtNavMeshQuery::findWeightedPath(const float* startPos, const float* endPos,
										  const dtPolyRef* path, int pathSize,
										  const dtQueryFilter* filter,
										  double absoluteTolerance, int maxIterations,
										  dtWeightedPathResult* result) const
{
	// --- Parameter validation -------------------------------------------------
	if (!result)
		return DT_FAILURE | DT_INVALID_PARAM;
	clearResult(result);
	if (!result->points || !result->dual)
		return DT_FAILURE | DT_INVALID_PARAM;

	if (!startPos || !endPos || !path || !filter || pathSize < 1 || maxIterations < 0)
		return DT_FAILURE | DT_INVALID_PARAM;
	if (!finiteDouble(absoluteTolerance) || absoluteTolerance <= 0.0)
		return DT_FAILURE | DT_INVALID_PARAM;
	for (int k = 0; k < 3; ++k)
	{
		if (!finiteDouble((double)startPos[k]) || !finiteDouble((double)endPos[k]))
			return DT_FAILURE | DT_INVALID_PARAM;
	}
	if (!m_nav)
		return DT_FAILURE | DT_INVALID_PARAM;

	const int npoly = pathSize;
	const int nportal = npoly - 1;
	const int npoints = npoly + 1;
	// Guards against integer overflow in the size calculations below.
	if (npoly > (1 << 28))
		return DT_FAILURE | DT_INVALID_PARAM;

	if (result->maxPoints < npoints || result->maxDual < npoly)
		return DT_FAILURE | DT_BUFFER_TOO_SMALL;

	const int arrays = 9;
	double** blocks = (double**)dtAlloc(sizeof(double*) * arrays, DT_ALLOC_TEMP);
	if (!blocks)
		return DT_FAILURE | DT_OUT_OF_MEMORY;
	for (int i = 0; i < arrays; ++i) blocks[i] = 0;

	blocks[0] = (double*)dtAlloc(sizeof(double) * 3 * npoints, DT_ALLOC_TEMP);            // best points
	blocks[1] = (double*)dtAlloc(sizeof(double) * (npoly > 0 ? npoly : 1), DT_ALLOC_TEMP); // weights
	blocks[2] = (double*)dtAlloc(sizeof(double) * 3 * (nportal > 0 ? nportal : 1), DT_ALLOC_TEMP); // portal a
	blocks[3] = (double*)dtAlloc(sizeof(double) * 3 * (nportal > 0 ? nportal : 1), DT_ALLOC_TEMP); // portal b
	blocks[4] = (double*)dtAlloc(sizeof(double) * 3 * (npoly > 0 ? npoly : 1), DT_ALLOC_TEMP);    // published dual
	blocks[5] = (double*)dtAlloc(sizeof(double) * 3 * (npoly > 0 ? npoly : 1), DT_ALLOC_TEMP);    // dual working set
	blocks[6] = (double*)dtAlloc(sizeof(double) * 3 * (npoly > 0 ? npoly : 1), DT_ALLOC_TEMP);    // FISTA extrapolation
	blocks[7] = (double*)dtAlloc(sizeof(double) * 3 * (npoly > 0 ? npoly : 1), DT_ALLOC_TEMP);    // FISTA previous iterate
	blocks[8] = (double*)dtAlloc(sizeof(double) * 3 * (npoly > 0 ? npoly : 1), DT_ALLOC_TEMP);    // dual gradient

	bool oom = false;
	for (int i = 0; i < arrays; ++i)
	{
		if (!blocks[i]) oom = true;
	}

	const dtMeshTile** tiles = (const dtMeshTile**)dtAlloc(sizeof(dtMeshTile*) * npoly, DT_ALLOC_TEMP);
	const dtPoly** polys = (const dtPoly**)dtAlloc(sizeof(dtPoly*) * npoly, DT_ALLOC_TEMP);
	if (!tiles || !polys) oom = true;

	if (oom)
	{
		dtFree(tiles);
		dtFree(polys);
		for (int i = 0; i < arrays; ++i) dtFree(blocks[i]);
		dtFree(blocks);
		return DT_FAILURE | DT_OUT_OF_MEMORY;
	}

	double* const bestPoints = blocks[0];
	double* const weights = blocks[1];
	double* const portalA = blocks[2];
	double* const portalB = blocks[3];
	double* const dual = blocks[4];
	double* const dualWork = blocks[5];
	double* const dualYk = blocks[6];
	double* const dualPrev = blocks[7];
	double* const dualGrad = blocks[8];

	// --- Validate corridor and collect weights and portals --------------------
	dtStatus status = DT_SUCCESS;
	for (int i = 0; i < npoly; ++i)
	{
		if (dtStatusFailed(m_nav->getTileAndPolyByRef(path[i], &tiles[i], &polys[i])))
		{
			status = DT_FAILURE | DT_INVALID_PARAM;
			break;
		}
		// Ground polygons only; off-mesh connections are rejected.
		if (polys[i]->getType() != DT_POLYTYPE_GROUND)
		{
			status = DT_FAILURE | DT_INVALID_PARAM;
			break;
		}
		if (!filter->passFilter(path[i], tiles[i], polys[i]))
		{
			status = DT_FAILURE | DT_INVALID_PARAM;
			break;
		}
		const double w = (double)filter->getAreaCost((int)polys[i]->getArea());
		if (!finiteDouble(w) || w <= 0.0)
		{
			status = DT_FAILURE | DT_INVALID_PARAM;
			break;
		}
		weights[i] = w;
	}

	// Build the n-1 portal segments from the real from->to links, including
	// the bmin/bmax clipping of tile boundary links. This mirrors
	// dtNavMeshQuery::getPortalPoints() for ground polygons and must not be
	// replaced by the unclipped cross-tile edge.
	for (int j = 0; j < nportal && dtStatusSucceed(status); ++j)
	{
		const dtMeshTile* fromTile = tiles[j];
		const dtPoly* fromPoly = polys[j];
		const dtLink* link = 0;
		for (unsigned int k = fromPoly->firstLink; k != DT_NULL_LINK; k = fromTile->links[k].next)
		{
			if (fromTile->links[k].ref == path[j+1])
			{
				link = &fromTile->links[k];
				break;
			}
		}
		if (!link)
		{
			status = DT_FAILURE | DT_INVALID_PARAM;
			break;
		}

		const int v0 = fromPoly->verts[link->edge];
		const int v1 = fromPoly->verts[(link->edge+1) % (int)fromPoly->vertCount];
		double left[3], right[3], baseLeft[3], baseRight[3];
		left[0] = baseLeft[0] = fromTile->verts[v0*3+0];
		left[1] = baseLeft[1] = fromTile->verts[v0*3+1];
		left[2] = baseLeft[2] = fromTile->verts[v0*3+2];
		right[0] = baseRight[0] = fromTile->verts[v1*3+0];
		right[1] = baseRight[1] = fromTile->verts[v1*3+1];
		right[2] = baseRight[2] = fromTile->verts[v1*3+2];

		if (link->side != 0xff && (link->bmin != 0 || link->bmax != 255))
		{
			const double tmin = (double)link->bmin / 255.0;
			const double tmax = (double)link->bmax / 255.0;
			vlerp(left, baseLeft, baseRight, tmin);
			vlerp(right, baseLeft, baseRight, tmax);
		}

		vcopy(&portalA[j*3], left);
		vcopy(&portalB[j*3], right);
	}

	if (dtStatusFailed(status))
	{
		dtFree(tiles);
		dtFree(polys);
		for (int i = 0; i < arrays; ++i) dtFree(blocks[i]);
		dtFree(blocks);
		return status;
	}

	double start[3], end[3];
	for (int k = 0; k < 3; ++k)
	{
		start[k] = (double)startPos[k];
		end[k] = (double)endPos[k];
	}

	// Portal direction vectors h_j = b_j - a_j.
	double* portalH = (double*)dtAlloc(sizeof(double) * 3 * (nportal > 0 ? nportal : 1), DT_ALLOC_TEMP);
	double* t = 0;
	double* tPrev = 0;
	double* tY = 0;
	double* grad = 0;
	if (!portalH ||
		(nportal > 0 &&
		 (!(t = (double*)dtAlloc(sizeof(double) * nportal, DT_ALLOC_TEMP)) ||
		  !(tPrev = (double*)dtAlloc(sizeof(double) * nportal, DT_ALLOC_TEMP)) ||
		  !(tY = (double*)dtAlloc(sizeof(double) * nportal, DT_ALLOC_TEMP)) ||
		  !(grad = (double*)dtAlloc(sizeof(double) * nportal, DT_ALLOC_TEMP)))))
	{
		dtFree(portalH); dtFree(t); dtFree(tPrev); dtFree(tY); dtFree(grad);
		dtFree(tiles);
		dtFree(polys);
		for (int i = 0; i < arrays; ++i) dtFree(blocks[i]);
		dtFree(blocks);
		return DT_FAILURE | DT_OUT_OF_MEMORY;
	}
	for (int j = 0; j < nportal; ++j)
	{
		portalH[j*3+0] = portalB[j*3+0] - portalA[j*3+0];
		portalH[j*3+1] = portalB[j*3+1] - portalA[j*3+1];
		portalH[j*3+2] = portalB[j*3+2] - portalA[j*3+2];
	}

	// --- Initial feasible polyline: start, portal midpoints, end -------------
	bestPoints[0] = start[0]; bestPoints[1] = start[1]; bestPoints[2] = start[2];
	bestPoints[npoly*3+0] = end[0];
	bestPoints[npoly*3+1] = end[1];
	bestPoints[npoly*3+2] = end[2];
	for (int j = 0; j < nportal; ++j)
	{
		t[j] = tPrev[j] = tY[j] = 0.5;
		vlerp(&bestPoints[(j+1)*3], &portalA[j*3], &portalB[j*3], 0.5);
	}

	double* workPoints = (double*)dtAlloc(sizeof(double) * 3 * npoints, DT_ALLOC_TEMP);
	if (!workPoints)
	{
		dtFree(portalH); dtFree(t); dtFree(tPrev); dtFree(tY); dtFree(grad);
		dtFree(tiles);
		dtFree(polys);
		for (int i = 0; i < arrays; ++i) dtFree(blocks[i]);
		dtFree(blocks);
		return DT_FAILURE | DT_OUT_OF_MEMORY;
	}

	double upperCost = polylineCost(bestPoints, npoly, weights);

	// --- Primal phase --------------------------------------------------------
	// Continuation FISTA on a smoothed convex objective, annealed toward the
	// exact weighted length, with periodic exact golden-section sweeps that
	// resolve kinks (zero-length segments, endpoint optima, coincident
	// points). The certified gap is the only success criterion; a cost stall
	// merely hands the remaining budget to the dual phase.
	bool converged = false;
	int primalIters = 0;

	if (nportal == 0)
	{
		// Single polygon: the straight segment is trivially optimal.
		converged = true;
	}
	else
	{
		// Continuation FISTA on the smoothed convex objective. Visible
		// smoothing early on lets the sequence cross a kinked ridge where
		// two coincident portal points must move jointly; coordinate descent
		// alone cannot cross such a ridge, so golden-section sweeps are only
		// applied as an exact polish after FISTA finishes.
		const double deltas[] = {
			3e-1, 3e-2, 3e-3, 3e-4, 3e-5, 3e-7, 3e-9, 3e-11, 3e-13, 1e-16
		};
		const int nStages = (int)(sizeof(deltas)/sizeof(deltas[0]));
		// The continuation needs enough iterations per stage to cross kinked
		// ridges (two coincident portal points moving jointly); the dual
		// phase returns as soon as its gap is met, so give the primal the
		// larger share while keeping both phases within the total budget.
		int primalBudget = maxIterations * 3 / 4;
		if (primalBudget < nStages * 64) primalBudget = maxIterations;
		if (primalBudget > maxIterations) primalBudget = maxIterations;
		const int perStage = primalBudget / nStages;
		double lip = 1.0;
		double momentum = 1.0;
		double bestCost = upperCost;

		for (int stage = 0; stage < nStages && primalIters < primalBudget && !converged; ++stage)
		{
			const double delta = deltas[stage];
			const int stageBudget = (stage == nStages-1)
				? (primalBudget - primalIters)
				: perStage;
			for (int k = 0; k < stageBudget && primalIters < primalBudget; ++k, ++primalIters)
			{
				// FISTA step on the smoothed objective with backtracking.
				smoothedGradient(tY, nportal, portalA, portalH, start, end,
								 weights, npoly, delta, workPoints, grad);
				const double fY = smoothedCost(tY, nportal, portalA, portalH,
											   start, end, weights, npoly, delta);
				double step = 1.0 / lip;
				for (int back = 0; back < 40; ++back)
				{
					double dist2 = 0.0, quad = 0.0;
					for (int j = 0; j < nportal; ++j)
					{
						t[j] = clampUnit(tY[j] - step * grad[j]);
						const double dz = t[j] - tY[j];
						dist2 += dz*dz;
						quad += grad[j]*dz;
					}
					const double fT = smoothedCost(t, nportal, portalA, portalH,
												   start, end, weights, npoly, delta);
					if (fT <= fY + quad + 0.5*lip*dist2 + 1e-12*(1.0+fY))
						break;
					lip *= 2.0;
					step = 1.0 / lip;
				}

				// Materialize the candidate and snapshot the best exact point.
				// The FISTA iterates t/tPrev/tY keep evolving independently;
				// rolling them back to the best point would pin the sequence
				// on a kink and prevent a joint ridge crossing.
				workPoints[0] = start[0]; workPoints[1] = start[1]; workPoints[2] = start[2];
				workPoints[npoly*3+0] = end[0];
				workPoints[npoly*3+1] = end[1];
				workPoints[npoly*3+2] = end[2];
				for (int j = 0; j < nportal; ++j)
					vparam(&workPoints[(j+1)*3], &portalA[j*3], &portalH[j*3], t[j]);
				const double candCost = polylineCost(workPoints, npoly, weights);
				if (candCost < bestCost)
				{
					bestCost = candCost;
					for (int i = 0; i < npoints; ++i)
						bestPoints[i*3+0] = workPoints[i*3+0],
						bestPoints[i*3+1] = workPoints[i*3+1],
						bestPoints[i*3+2] = workPoints[i*3+2];
				}

				const double nextMomentum = 0.5*(1.0 + sqrt(1.0 + 4.0*momentum*momentum));
				const double gamma = (momentum - 1.0) / nextMomentum;
				for (int j = 0; j < nportal; ++j)
					tY[j] = clampUnit(t[j] + gamma*(t[j] - tPrev[j]));
				for (int j = 0; j < nportal; ++j) tPrev[j] = t[j];
				momentum = nextMomentum;
				lip *= 0.9;
				if (lip < 1e-12) lip = 1e-12;
			}
		}

		// Exact polish: cyclic golden-section coordinate descent from the
		// best FISTA polyline. A move is applied only when it strictly lowers
		// the exact cost; at a kink the 1D minimum equals the current value
		// and must not overwrite the continuation's joint ridge crossing.
		for (int sweep = 0; sweep < 4; ++sweep)
		{
			for (int j = 0; j < nportal; ++j)
			{
				const double before = polylineCost(bestPoints, npoly, weights);
				const double tj = minimizePortal(j, bestPoints, weights,
												 portalA, portalH);
				double candidate[3];
				vparam(candidate, &portalA[j*3], &portalH[j*3], tj);
				double saved[3];
				vcopy(saved, &bestPoints[(j+1)*3]);
				vcopy(&bestPoints[(j+1)*3], candidate);
				const double after = polylineCost(bestPoints, npoly, weights);
				if (after < before)
				{
					t[j] = tj;
				}
				else
				{
					vcopy(&bestPoints[(j+1)*3], saved);
				}
			}
		}
	}

	// Exact upper bound from the best feasible polyline.
	upperCost = polylineCost(bestPoints, npoly, weights);

	// --- Initial primal certificate ------------------------------------------
	// q_i = w_i * unit(p_{i+1}-p_i) (zero for a zero segment), so each
	// |q_i| <= w_i by construction.
	{
		double d[3];
		for (int i = 0; i < npoly; ++i)
		{
			vsub(d, &bestPoints[(i+1)*3], &bestPoints[i*3]);
			const double len = sqrt(vdot(d, d));
			if (len > 0.0)
			{
				const double s = weights[i] / len;
				dual[i*3+0] = d[0]*s; dual[i*3+1] = d[1]*s; dual[i*3+2] = d[2]*s;
			}
			else
			{
				dual[i*3+0] = dual[i*3+1] = dual[i*3+2] = 0.0;
			}
		}
	}
	// Exact bound of the primal certificate. The all-zero certificate is
	// always feasible, so a negative value is replaced by zero vectors whose
	// bound is exactly zero.
	double lowerCost = exactDual(dual, bestPoints, portalA, portalB, npoly);
	if (lowerCost < 0.0)
	{
		for (int i = 0; i < 3*npoly; ++i) dual[i] = 0.0;
		lowerCost = 0.0;
	}
	if (upperCost - lowerCost <= absoluteTolerance)
		converged = true;

	// --- Dual phase ----------------------------------------------------------
	// The vectors are free within their norm balls, so projected continuation
	// FISTA strengthens a weak (e.g. kinked) certificate without changing the
	// feasible polyline. It shares the same iteration budget; with no budget
	// left, the primal certificate above is all that is published.
	if (!converged && primalIters < maxIterations)
	{
		for (int i = 0; i < 3*npoly; ++i) dualWork[i] = dual[i];
		double bestD = lowerCost;
		dualAscent(bestPoints, portalA, portalB, weights, npoly,
				   maxIterations - primalIters, upperCost, absoluteTolerance,
				   dualWork, dual, dualYk, dualPrev, dualGrad, bestD);
		lowerCost = bestD;
		if (upperCost - lowerCost <= absoluteTolerance)
			converged = true;
	}

	// Final exact recomputation of both bounds from the published data.
	upperCost = polylineCost(bestPoints, npoly, weights);
	double finalD = exactDual(dual, bestPoints, portalA, portalB, npoly);
	if (finalD < 0.0)
	{
		// The all-zero certificate is always feasible and evaluates to
		// exactly zero, keeping lowerCost reproducible from the vectors.
		for (int i = 0; i < 3*npoly; ++i) dual[i] = 0.0;
		finalD = 0.0;
	}
	if (finalD > upperCost)
	{
		// Round-off guard only: the exact bound of feasible vectors cannot
		// exceed the optimum, so it cannot legitimately exceed upperCost.
		finalD = upperCost;
	}
	lowerCost = finalD;
	if (upperCost - lowerCost <= absoluteTolerance)
		converged = true;

	// Publish the complete feasible solution in every non-failing case.
	for (int i = 0; i < npoints; ++i)
	{
		result->points[i*3+0] = bestPoints[i*3+0];
		result->points[i*3+1] = bestPoints[i*3+1];
		result->points[i*3+2] = bestPoints[i*3+2];
	}
	for (int i = 0; i < npoly; ++i)
	{
		result->dual[i*3+0] = dual[i*3+0];
		result->dual[i*3+1] = dual[i*3+1];
		result->dual[i*3+2] = dual[i*3+2];
	}
	result->pointCount = npoints;
	result->dualCount = npoly;
	result->upperCost = upperCost;
	result->lowerCost = lowerCost;

	dtFree(workPoints);
	dtFree(portalH);
	dtFree(t);
	dtFree(tPrev);
	dtFree(tY);
	dtFree(grad);
	dtFree(tiles);
	dtFree(polys);
	for (int i = 0; i < arrays; ++i) dtFree(blocks[i]);
	dtFree(blocks);

	return converged ? DT_SUCCESS : DT_IN_PROGRESS;
}
