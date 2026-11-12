#pragma once

// Internal transformation engine shared by the static Object::getGlobalTransform()
// path (ofbx.cpp) and the pose/skinning entry points (ofbx_pose.cpp).
// Everything here is pure math on const inputs: no Object, curve or scene state is
// ever mutated, so the same code can be used from interleaved queries on any scene.
//
// Matrices use the same column-major layout as ofbx::DMatrix (m[column * 4 + row]).

#include "ofbx.h"
#include <cmath>

namespace ofbx {
namespace pose_detail {

// One evaluated node: its full local matrix (pivots/offsets already baked in by
// Object::evalLocal), the raw Lcl Scaling components, and its InheritType (0..2).
struct NodeLocal
{
	DMatrix matrix;
	DVec3 scale;
	int inherit;
};


inline DMatrix makeIdentity()
{
	return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}


inline DMatrix multiply(const DMatrix& lhs, const DMatrix& rhs)
{
	DMatrix res;
	for (int col = 0; col < 4; ++col)
	{
		for (int row = 0; row < 4; ++row)
		{
			double v = 0;
			for (int k = 0; k < 4; ++k)
			{
				v += lhs.m[k * 4 + row] * rhs.m[col * 4 + k];
			}
			res.m[col * 4 + row] = v;
		}
	}
	return res;
}


inline DVec3 transformPoint(const DMatrix& m, const DVec3& p)
{
	return
	{
		m.m[0] * p.x + m.m[4] * p.y + m.m[8]  * p.z + m.m[12],
		m.m[1] * p.x + m.m[5] * p.y + m.m[9]  * p.z + m.m[13],
		m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14],
	};
}


inline DMatrix diagonal(double x, double y, double z)
{
	DMatrix m = makeIdentity();
	m.m[0] = x;
	m.m[5] = y;
	m.m[10] = z;
	return m;
}


// Drops the translation column. The input is always an affine matrix.
inline DMatrix linearPart(const DMatrix& m)
{
	DMatrix r = m;
	r.m[12] = r.m[13] = r.m[14] = 0;
	return r;
}


inline bool isFinite(double v)
{
	return std::isfinite(v);
}


inline bool finiteMatrix(const DMatrix& m)
{
	for (double v : m.m)
	{
		if (!std::isfinite(v)) return false;
	}
	return true;
}


// Supported local transforms are finite TRS with strictly positive scaling.
// Zero scale, reflections and hand authored shear are out of scope.
inline bool supportedScale(const DVec3& s)
{
	return isFinite(s.x) && isFinite(s.y) && isFinite(s.z)
		&& s.x > 0.0 && s.y > 0.0 && s.z > 0.0;
}


// Full inverse of a 4x4 affine matrix through Gauss-Jordan elimination.
// Returns false (and leaves `out` untouched) when the matrix is singular.
inline bool invertAffine(const DMatrix& in, DMatrix& out)
{
	double a[4][8];
	for (int col = 0; col < 4; ++col)
	{
		for (int row = 0; row < 4; ++row)
		{
			a[row][col] = in.m[col * 4 + row];
			a[row][col + 4] = (row == col) ? 1.0 : 0.0;
		}
	}

	for (int pivot = 0; pivot < 4; ++pivot)
	{
		int best = pivot;
		for (int row = pivot + 1; row < 4; ++row)
		{
			double d = a[row][pivot];
			if (d != d || d - d != 0) return false;
			if (d < 0) d = -d;
			double b = a[best][pivot];
			if (b < 0) b = -b;
			if (d > b) best = row;
		}
		double pv = a[best][pivot];
		if (pv != pv || pv - pv != 0 || pv == 0.0) return false;
		if (best != pivot)
		{
			for (int col = 0; col < 8; ++col)
			{
				double tmp = a[pivot][col];
				a[pivot][col] = a[best][col];
				a[best][col] = tmp;
			}
		}
		for (int col = 0; col < 8; ++col) a[pivot][col] /= pv;
		for (int row = 0; row < 4; ++row)
		{
			if (row == pivot) continue;
			double f = a[row][pivot];
			if (f == 0.0) continue;
			for (int col = 0; col < 8; ++col) a[row][col] -= f * a[pivot][col];
		}
	}

	DMatrix res;
	for (int col = 0; col < 4; ++col)
	{
		for (int row = 0; row < 4; ++row)
		{
			res.m[col * 4 + row] = a[row][col + 4];
		}
	}
	if (!finiteMatrix(res)) return false;
	out = res;
	return true;
}


// The single inheritance rule used by every code path in the library.
//
// L = T * Rlin * S is the child local matrix (T holds the translation column with
// all pivot/offset effects, Rlin is the pure local rotation product, S = diag(scale)).
// The parent contributes its complete world matrix Gp and its own raw local scale
// vector Sp. Following the Autodesk "Transformations" example:
//
//   RrSs (0): child rotation is applied before the inherited scale,
//             linear = Rlin * Gp.linear * S
//   RSrs (1): child rotation is applied after the inherited scale,
//             linear = Gp.linear * Rlin * S  (plain parent-world * local)
//   Rrs  (2): only the direct parent's own local scale is compensated,
//             linear = Rlin * Gp.linear * Sp^-1 * S; scales inherited from
//             earlier ancestors live inside Gp and are preserved.
//
// Translation is identical for every mode: the child local origin (pivots and
// offsets included) is transformed by the parent's *full* world matrix, so parent
// scale is never removed from the translation.
//
// Gp may carry shear from anisotropic scaling; it is always treated as an opaque
// 3x3, never decomposed by reading column lengths.
inline DMatrix inheritCompose(const DMatrix& parent_world, const DVec3& parent_local_scale, const NodeLocal& child)
{
	const DMatrix s = diagonal(child.scale.x, child.scale.y, child.scale.z);
	const DMatrix s_inv = diagonal(1.0 / child.scale.x, 1.0 / child.scale.y, 1.0 / child.scale.z);

	const DMatrix lin = linearPart(child.matrix);
	const DMatrix r_lin = multiply(lin, s_inv); // L = Rlin * S, hence Rlin = L * S^-1
	const DMatrix gp_lin = linearPart(parent_world);

	DMatrix clin;
	switch (child.inherit)
	{
		default:
		case 0: // RrSs
			clin = multiply(multiply(r_lin, gp_lin), s);
			break;
		case 1: // RSrs
			clin = multiply(multiply(gp_lin, r_lin), s);
			break;
		case 2: // Rrs
		{
			const DMatrix sp_inv = diagonal(
				1.0 / parent_local_scale.x,
				1.0 / parent_local_scale.y,
				1.0 / parent_local_scale.z);
			clin = multiply(multiply(multiply(r_lin, gp_lin), sp_inv), s);
			break;
		}
	}

	DMatrix res = clin;
	const DVec3 origin = {child.matrix.m[12], child.matrix.m[13], child.matrix.m[14]};
	const DVec3 t = transformPoint(parent_world, origin);
	res.m[12] = t.x;
	res.m[13] = t.y;
	res.m[14] = t.z;
	res.m[3] = res.m[7] = res.m[11] = 0;
	res.m[15] = 1;
	return res;
}

} // namespace pose_detail
} // namespace ofbx
