// Numeric regression tests for linear affine instance motion on the CPU TLAS/BLAS path.
//
// The BLAS triangles are static; a motion-enabled BLASInstance interpolates its local-to-world
// matrix elementwise between a shutter-open and a shutter-close pose at the ray's 'time' in [0, 1].
// Expected geometry is always derived independently by intersecting the explicitly transformed
// world-space triangle, never by comparing two motion code paths against each other.
//
// The file is windowless and uses C++20 only; baseline/run_target.sh compiles it both as a
// scalar build (TINYBVH_NO_SIMD) and as a native SIMD build.

#define TINYBVH_IMPLEMENTATION
#include "tiny_bvh.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace tinybvh;

namespace {

constexpr float EPS = 1e-5f;
constexpr double DEPS = 1e-11;

// -----------------------------------------------------------------------------------------------
// Small independent math helpers, kept separate from the library implementation under test.
// -----------------------------------------------------------------------------------------------

struct RefMat { float m[16]; };

// Matrix entry at row r, column c. TinyBVH stores 16 cells in row-major order (m[row*4+col]).
inline float MCell( const RefMat& M, int r, int c ) { return M.m[r * 4 + c]; }

inline RefMat LerpMat( const RefMat& A, const RefMat& B, float t )
{
	RefMat R;
	const float a = 1.0f - t;
	for (int i = 0; i < 16; i++) R.m[i] = a * A.m[i] + t * B.m[i];
	return R;
}

inline bvhvec3 XformPoint( const bvhvec3& p, const RefMat& M )
{
	return bvhvec3(
		MCell( M, 0, 0 ) * p.x + MCell( M, 0, 1 ) * p.y + MCell( M, 0, 2 ) * p.z + MCell( M, 0, 3 ),
		MCell( M, 1, 0 ) * p.x + MCell( M, 1, 1 ) * p.y + MCell( M, 1, 2 ) * p.z + MCell( M, 1, 3 ),
		MCell( M, 2, 0 ) * p.x + MCell( M, 2, 1 ) * p.y + MCell( M, 2, 2 ) * p.z + MCell( M, 2, 3 ) );
}

struct RefHit { bool hit; float t, u, v; };

// Independent Moeller-Trumbore against an explicit world-space triangle. A hit is in the
// half-open range [0, tMax), matching the library's convention. Barycentrics are those of the
// world triangle; t is the parameter of the original world ray O + t D, even when D is not unit.
inline RefHit RefIntersect( const bvhvec3& O, const bvhvec3& D, const bvhvec3& v0,
	const bvhvec3& v1, const bvhvec3& v2, float tMax )
{
	const bvhvec3 e1 = v1 - v0, e2 = v2 - v0;
	const bvhvec3 h = tinybvh_cross( D, e2 );
	const float a = tinybvh_dot( e1, h );
	if (a == 0.0f) return { false, 0, 0, 0 };
	const float f = 1.0f / a;
	const bvhvec3 s = O - v0;
	const float u = f * tinybvh_dot( s, h );
	if (u < 0.0f || u > 1.0f) return { false, 0, 0, 0 };
	const bvhvec3 q = tinybvh_cross( s, e1 );
	const float v = f * tinybvh_dot( D, q );
	if (v < 0.0f || u + v > 1.0f) return { false, 0, 0, 0 };
	const float t = f * tinybvh_dot( e2, q );
	if (!(t >= 0.0f && t < tMax)) return { false, 0, 0, 0 };
	return { true, t, u, v };
}

// The single local triangle used throughout the float tests.
const std::array<bvhvec4, 3> LOCAL_TRI = { bvhvec4( -1, -1, 1, 0 ), bvhvec4( 1, -1, 1, 0 ), bvhvec4( 0, 1, 1, 0 ) };

inline RefMat OpenPose()
{
	// diagonal scale (1, 1, 2), translation (-4, 0, 4).
	RefMat M{};
	M.m[0] = 1; M.m[5] = 1; M.m[10] = 2; M.m[3] = -4; M.m[7] = 0; M.m[11] = 4; M.m[15] = 1;
	return M;
}
inline RefMat ClosePose()
{
	// diagonal scale (3, 1, 4), translation (4, 0, 6).
	RefMat M{};
	M.m[0] = 3; M.m[5] = 1; M.m[10] = 4; M.m[3] = 4; M.m[7] = 0; M.m[11] = 6; M.m[15] = 1;
	return M;
}

inline void CopyToInstance( BLASInstance& inst, const RefMat& M )
{
	for (int i = 0; i < 16; i++) inst.transform[i] = M.m[i];
}
inline void CopyEndToInstance( BLASInstance& inst, const RefMat& M )
{
	for (int i = 0; i < 16; i++) inst.transformEnd[i] = M.m[i];
}

// Intersect the explicitly transformed reference triangle with the given world ray.
inline RefHit ReferenceMotionHit( const bvhvec3& O, const bvhvec3& D, float time, float tMax )
{
	const RefMat M = LerpMat( OpenPose(), ClosePose(), time );
	const bvhvec3 w0 = XformPoint( bvhvec3( LOCAL_TRI[0] ), M );
	const bvhvec3 w1 = XformPoint( bvhvec3( LOCAL_TRI[1] ), M );
	const bvhvec3 w2 = XformPoint( bvhvec3( LOCAL_TRI[2] ), M );
	return RefIntersect( O, D, w0, w1, w2, tMax );
}

inline bool Approx( float a, float b, float eps = EPS ) { return std::fabs( a - b ) <= eps; }

// Snapshot every caller-visible field that must be left untouched by a query.
struct Snap { bvhvec3 O, D, rD; uint32_t mask; float time; uint32_t instIdx; };
inline Snap Take( const Ray& r ) { return { r.O, r.D, r.rD, r.mask, r.time, r.instIdx }; }
inline bool Same( const Ray& r, const Snap& s )
{
	return Approx( r.O.x, s.O.x, 0.0f ) && Approx( r.O.y, s.O.y, 0.0f ) && Approx( r.O.z, s.O.z, 0.0f ) &&
		Approx( r.D.x, s.D.x, 0.0f ) && Approx( r.D.y, s.D.y, 0.0f ) && Approx( r.D.z, s.D.z, 0.0f ) &&
		Approx( r.rD.x, s.rD.x, 0.0f ) && Approx( r.rD.y, s.rD.y, 0.0f ) && Approx( r.rD.z, s.rD.z, 0.0f ) &&
		r.mask == s.mask && Approx( r.time, s.time, 0.0f ) && r.instIdx == s.instIdx;
}

inline int Fail( const char* group, const char* what )
{
	std::fprintf( stderr, "FAIL [%s]: %s\n", group, what );
	return 1;
}

// Build a single-triangle BLAS in each of the three supported float layouts.
struct BlasSet
{
	BVH bvh;
	BVH4_CPU bvh4;
	BVH8_CPU bvh8;
	BVHBase* list[3];
	BlasSet()
	{
		bvh.Build( LOCAL_TRI.data(), 1 );
		bvh4.Build( LOCAL_TRI.data(), 1 );
		bvh8.Build( LOCAL_TRI.data(), 1 );
		list[0] = &bvh; list[1] = &bvh4; list[2] = &bvh8;
	}
};

// -----------------------------------------------------------------------------------------------
// Group 1: affine numeric regression, including a sheared pose and a non-unit ray direction.
// -----------------------------------------------------------------------------------------------
int TestAffine()
{
	const char* G = "affine";
	BlasSet blas;
	for (int layout = 0; layout < 3; layout++)
	{
		BLASInstance inst( 0 ); // the single-element BLAS array below always uses index 0.
		CopyToInstance( inst, OpenPose() );
		CopyEndToInstance( inst, ClosePose() );
		inst.motionEnabled = true;
		BVHBase* blases[] = { blas.list[layout] };
		BVH tlas;
		tlas.Build( &inst, 1, blases, 1 );
		if (!tlas.HasMotionInstances()) return Fail( G, "TLAS did not flag a motion instance" );

		const bvhvec3 O( 0.25f, -0.25f, 0.0f ), D( 0, 0, 2 );

		// The specified mid-pose hit.
		Ray mid( O, D );
		mid.time = 0.5f;
		tlas.Intersect( mid );
		if (!Approx( mid.hit.t, 4.0f ) || !Approx( mid.hit.u, 0.375f ) || !Approx( mid.hit.v, 0.375f ))
			return Fail( G, "mid-pose t/u/v mismatch" );
		const RefHit refMid = ReferenceMotionHit( O, D, 0.5f, 1e30f );
		if (!refMid.hit || !Approx( mid.hit.t, refMid.t ) || !Approx( mid.hit.u, refMid.u ) || !Approx( mid.hit.v, refMid.v ))
			return Fail( G, "mid-pose disagrees with independent transformed triangle" );
		if (mid.hit.prim != 0 || mid.hit.inst != 0) return Fail( G, "prim/instance identity wrong" );

		// Both endpoint rays miss this world ray.
		Ray r0( O, D ); r0.time = 0.0f; tlas.Intersect( r0 );
		Ray r1( O, D ); r1.time = 1.0f; tlas.Intersect( r1 );
		if (r0.hit.t < BVH_FAR || r1.hit.t < BVH_FAR) return Fail( G, "endpoint pose must miss" );
		if (ReferenceMotionHit( O, D, 0.0f, 1e30f ).hit || ReferenceMotionHit( O, D, 1.0f, 1e30f ).hit)
			return Fail( G, "reference expects endpoint misses" );

		// A sweep of times must match the explicit transformed triangle at every sample, and the
		// mid-pose must not be reproducible by interpolating inverse matrices (it is not: we invert
		// the interpolated forward matrix). Cover t in {0, .125, ..., 1}.
		for (int k = 0; k <= 8; k++)
		{
			const float time = k * 0.125f;
			Ray r( O, D ); r.time = time;
			tlas.Intersect( r );
			const RefHit ref = ReferenceMotionHit( O, D, time, 1e30f );
			if (ref.hit != (r.hit.t < BVH_FAR)) return Fail( G, "hit/miss disagreement over time sweep" );
			if (ref.hit && (!Approx( r.hit.t, ref.t, 2e-5f ) || !Approx( r.hit.u, ref.u, 2e-5f ) || !Approx( r.hit.v, ref.v, 2e-5f )))
				return Fail( G, "numeric disagreement over time sweep" );
		}

		// Query leaves every caller-visible ray field and the shared instance record untouched.
		const Snap before = Take( mid );
		const bvhmat4 savedInv = inst.invTransform;
		Ray again( O, D ); again.time = 0.5f;
		tlas.Intersect( again );
		if (!Same( again, before )) return Fail( G, "Intersect modified caller ray fields" );
		for (int i = 0; i < 16; i++) if (!Approx( inst.invTransform[i], savedInv[i], 0.0f ))
			return Fail( G, "shared invTransform was overwritten by a timed query" );

		// Both bundle APIs must work on this TLAS layout with mixed per-ray time and distance limits.
		Ray pack[TINYBVH_BUNDLE_RAYS];
		float packLimit[TINYBVH_BUNDLE_RAYS];
		for (int i = 0; i < TINYBVH_BUNDLE_RAYS; i++)
		{
			const float time = (i % 4) / 3.0f;
			const float tMax = (i % 6 == 0) ? 3.5f : 1e30f;
			packLimit[i] = tMax;
			pack[i] = Ray( O, D, tMax ); pack[i].time = time;
		}
		tlas.IntersectBundle( pack );
		for (int i = 0; i < TINYBVH_BUNDLE_RAYS; i++)
		{
			const RefHit ref = ReferenceMotionHit( O, D, (i % 4) / 3.0f, packLimit[i] );
			if (ref.hit != (pack[i].hit.t < packLimit[i])) return Fail( G, "layout bundle hit/miss mismatch" );
			if (ref.hit && (!Approx( pack[i].hit.t, ref.t, 2e-5f ) ||
				!Approx( pack[i].hit.u, ref.u, 2e-5f ) || !Approx( pack[i].hit.v, ref.v, 2e-5f )))
				return Fail( G, "layout bundle numeric mismatch" );
		}
		Ray shade[TINYBVH_BUNDLE_RAYS];
		bool shadeOcc[TINYBVH_BUNDLE_RAYS];
		for (int i = 0; i < TINYBVH_BUNDLE_RAYS; i++)
		{
			shade[i] = Ray( O, D, (i % 6 == 0) ? 3.5f : 1e30f );
			shade[i].time = (i % 4) / 3.0f;
		}
		tlas.IsOccludedBundle( shade, shadeOcc );
		for (int i = 0; i < TINYBVH_BUNDLE_RAYS; i++)
		{
			const bool expect = ReferenceMotionHit( O, D, shade[i].time, shade[i].hit.t ).hit;
			if (shadeOcc[i] != expect) return Fail( G, "layout occlusion bundle mismatch" );
		}
	}

	// Sheared pose plus a non-unit, non-axis ray direction: world t must stay the parameter of the
	// original ray O + t D, and u/v must remain the local (hence world-affine) barycentrics.
	{
		BlasSet blas;
		BLASInstance inst( 0 );
		RefMat A = OpenPose(), B = ClosePose();
		// Introduce shear in both endpoints so the interpolated matrix is sheared everywhere.
		A.m[1] = 0.4f; A.m[8] = -0.2f;
		B.m[1] = -0.3f; B.m[8] = 0.25f;
		CopyToInstance( inst, A ); CopyEndToInstance( inst, B ); inst.motionEnabled = true;
		BVHBase* blases[] = { &blas.bvh };
		BVH tlas; tlas.Build( &inst, 1, blases, 1 );
		const bvhvec3 O( 0.7f, -0.4f, -1.0f ), D( 0.3f, 0.5f, 2.0f ); // deliberately not normalized
		for (int k = 1; k <= 7; k++)
		{
			const float time = k * 0.125f;
			Ray r( O, D ); r.time = time;
			tlas.Intersect( r );
			const RefMat M = LerpMat( A, B, time );
			const bvhvec3 w0 = XformPoint( bvhvec3( LOCAL_TRI[0] ), M );
			const bvhvec3 w1 = XformPoint( bvhvec3( LOCAL_TRI[1] ), M );
			const bvhvec3 w2 = XformPoint( bvhvec3( LOCAL_TRI[2] ), M );
			const RefHit ref = RefIntersect( O, D, w0, w1, w2, 1e30f );
			if (ref.hit != (r.hit.t < BVH_FAR)) return Fail( G, "shear hit/miss mismatch" );
			if (ref.hit && (!Approx( r.hit.t, ref.t, 2e-5f ) || !Approx( r.hit.u, ref.u, 2e-5f ) || !Approx( r.hit.v, ref.v, 2e-5f )))
				return Fail( G, "shear t/u/v mismatch with non-unit direction" );
		}
	}
	std::puts( "MOTION affine PASS" );
	return 0;
}

// -----------------------------------------------------------------------------------------------
// Group 2: a motion instance in a non-root leaf whose swept box leaves its shutter-open bounds.
// -----------------------------------------------------------------------------------------------
int TestSwept()
{
	const char* G = "swept";
	BlasSet blas;
	// A line of well-separated instances forces a multi-level TLAS, so the motion instance ends up
	// in a non-root leaf. The static peers are parked far from the motion ray near the origin.
	constexpr uint32_t N = 17, MOTION = 8;
	BLASInstance inst[N];
	RefMat parked{}; parked.m[0] = parked.m[5] = parked.m[10] = 0.25f; parked.m[15] = 1.0f;
	for (uint32_t i = 0; i < N; i++)
	{
		inst[i].blasIdx = 0;
		RefMat P = parked; P.m[3] = float( int( i ) - int( MOTION ) ) * 40.0f; P.m[11] = 30.0f;
		CopyToInstance( inst[i], P );
	}
	// The middle instance spans the full open->close range, centered near the origin.
	CopyToInstance( inst[MOTION], OpenPose() );
	CopyEndToInstance( inst[MOTION], ClosePose() );
	inst[MOTION].motionEnabled = true;
	BVHBase* blases[] = { &blas.bvh };
	BVH tlas;
	tlas.Build( inst, N, blases, 1 );
	if (tlas.bvhNode[0].isLeaf()) return Fail( G, "expected an interior TLAS root" );
	if (!tlas.HasMotionInstances()) return Fail( G, "motion flag not propagated to multi-instance TLAS" );

	// Locate the leaf that stores the motion instance and confirm it is not the root node.
	{
		uint32_t stack[64], sp = 0, leafNode = 0xFFFFFFFF;
		stack[sp++] = 0;
		bool found = false;
		while (sp)
		{
			const uint32_t ni = stack[--sp];
			const BVH::BVHNode& n = tlas.bvhNode[ni];
			if (n.isLeaf())
			{
				for (uint32_t k = 0; k < n.triCount; k++)
					if (tlas.primIdx[n.leftFirst + k] == MOTION) { leafNode = ni, found = true; }
			}
			else stack[sp++] = uint32_t( n.leftFirst ), stack[sp++] = uint32_t( n.leftFirst + 1 );
		}
		if (!found || leafNode == 0) return Fail( G, "motion instance is not in a non-root leaf" );
	}

	// The built instance bounds must cover BOTH endpoint poses, i.e. the whole shutter interval.
	{
		const RefMat A = OpenPose(), B = ClosePose();
		const bvhvec3 bmin = blas.bvh.aabbMin, bmax = blas.bvh.aabbMax;
		bvhvec3 lo( BVH_FAR ), hi( -BVH_FAR );
		for (int j = 0; j < 8; j++)
		{
			const bvhvec3 p( j & 1 ? bmax.x : bmin.x, j & 2 ? bmax.y : bmin.y, j & 4 ? bmax.z : bmin.z );
			for (const RefMat* M : { &A, &B })
			{
				const bvhvec3 w = XformPoint( p, *M );
				lo = tinybvh_min( lo, w ), hi = tinybvh_max( hi, w );
			}
		}
		for (int a = 0; a < 3; a++)
		{
			if (inst[MOTION].aabbMin[a] > lo[a] + EPS) return Fail( G, "swept AABB does not cover the open pose" );
			if (inst[MOTION].aabbMax[a] < hi[a] - EPS) return Fail( G, "swept AABB does not cover the close pose" );
		}
		// The close pose extends beyond the open-pose-only box along x (up to x = 7 vs x = -3).
		if (inst[MOTION].aabbMax.x < 6.9f) return Fail( G, "swept box failed to grow beyond the shutter-open bounds" );
	}

	// A ray that only meets the instance later in the shutter, inside the swept region but outside
	// the open-pose box, must hit at intermediate times and miss at time 0. Interleaved times on one
	// built TLAS must all be answered correctly.
	const bvhvec3 O( 0.25f, -0.25f, 0.0f ), D( 0, 0, 2 );
	const float times[] = { 0.0f, 0.5f, 1.0f, 0.25f, 0.75f, 0.5f, 0.0f, 1.0f };
	for (float time : times)
	{
		Ray r( O, D ); r.time = time;
		tlas.Intersect( r );
		const RefHit ref = ReferenceMotionHit( O, D, time, 1e30f );
		if (ref.hit != (r.hit.t < BVH_FAR)) return Fail( G, "interleaved-time hit/miss mismatch" );
		if (ref.hit)
		{
			if (!Approx( r.hit.t, ref.t, 2e-5f ) || !Approx( r.hit.u, ref.u, 2e-5f ) || !Approx( r.hit.v, ref.v, 2e-5f ))
				return Fail( G, "interleaved-time numeric mismatch" );
			if (r.hit.inst != MOTION || r.hit.prim != 0) return Fail( G, "hit returned the wrong instance" );
		}
	}
	std::puts( "MOTION swept PASS" );
	return 0;
}

// -----------------------------------------------------------------------------------------------
// Group 3: short shadow rays keep the half-open hit range, and mask filtering is unchanged.
// -----------------------------------------------------------------------------------------------
int TestShadow()
{
	const char* G = "shadow";
	BlasSet blas;
	BLASInstance inst( 0 );
	CopyToInstance( inst, OpenPose() ); CopyEndToInstance( inst, ClosePose() ); inst.motionEnabled = true;
	BVHBase* blases[] = { &blas.bvh };
	BVH tlas; tlas.Build( &inst, 1, blases, 1 );

	const bvhvec3 O( 0.25f, -0.25f, 0.0f ), D( 0, 0, 2 );
	const float time = 0.5f;
	Ray full( O, D ); full.time = time;
	tlas.Intersect( full );
	if (!(full.hit.t < BVH_FAR)) return Fail( G, "expected a mid-pose hit" );
	const float tHit = full.hit.t;
	const RefHit ref = ReferenceMotionHit( O, D, time, 1e30f );
	if (!ref.hit || !Approx( tHit, ref.t )) return Fail( G, "reference shadow distance mismatch" );

	// Half-open range [0, tMax): a ray ending just before the hit is not occluded; at/after it is.
	Ray shortRay( O, D, tHit * 0.99f ); shortRay.time = time;
	if (tlas.IsOccluded( shortRay )) return Fail( G, "shadow ray ending before the hit must be unoccluded" );
	Ray longRay( O, D, tHit * 1.01f ); longRay.time = time;
	if (!tlas.IsOccluded( longRay )) return Fail( G, "shadow ray reaching the hit must be occluded" );

	// IsOccluded must not modify the ray, including its time and finite distance limit.
	const Snap snap = Take( longRay );
	if (!tlas.IsOccluded( longRay )) return Fail( G, "occlusion result unstable" );
	if (!Same( longRay, snap )) return Fail( G, "IsOccluded modified the ray" );

	// Mask filtering: an instance mask that does not overlap the ray mask makes it invisible.
	inst.mask = 0x0002;
	Ray masked( O, D, tHit * 1.01f ); masked.time = time; masked.mask = 0x0001;
	if (tlas.IsOccluded( masked )) return Fail( G, "non-overlapping mask must skip the instance" );
	masked.mask = RAY_MASK_INTERSECT_ALL;
	if (!tlas.IsOccluded( masked )) return Fail( G, "overlapping mask must reach the instance" );
	inst.mask = RAY_MASK_INTERSECT_ALL;

	// The same rules hold through the bundle API with per-ray masks, times and distance limits.
	// The instance drops bit 0, so a ray carrying only bit 0 is filtered; all other rays pass.
	inst.mask = RAY_MASK_INTERSECT_ALL ^ 0x0001u;
	Ray bundle[TINYBVH_BUNDLE_RAYS];
	for (int i = 0; i < TINYBVH_BUNDLE_RAYS; i++)
	{
		bundle[i] = Ray( O, D, (i & 1) ? tHit * 0.5f : tHit * 1.5f );
		bundle[i].time = (i % 3) * 0.5f; // 0, .5 or 1
		bundle[i].mask = (i % 5 == 0) ? 0x0001 : RAY_MASK_INTERSECT_ALL;
	}
	bool occ[TINYBVH_BUNDLE_RAYS];
	tlas.IsOccludedBundle( bundle, occ );
	for (int i = 0; i < TINYBVH_BUNDLE_RAYS; i++)
	{
		const bool maskPass = (bundle[i].mask & inst.mask) != 0;
		const bool expect = maskPass && ReferenceMotionHit( O, D, bundle[i].time, bundle[i].hit.t ).hit;
		if (occ[i] != expect) return Fail( G, "bundle occlusion disagrees with independent shadow test" );
	}
	inst.mask = RAY_MASK_INTERSECT_ALL;
	std::puts( "MOTION shadow PASS" );
	return 0;
}

// -----------------------------------------------------------------------------------------------
// Group 4: two motion instances sharing one BLAS keep independent poses and identities.
// -----------------------------------------------------------------------------------------------
int TestIdentity()
{
	const char* G = "identity";
	BlasSet blas;
	BLASInstance inst[2];
	// Both reference BLAS 0, but with disjoint shutter trajectories placed at different locations.
	RefMat A0 = OpenPose(), B0 = ClosePose();
	RefMat A1{}, B1{};
	A1.m[0] = 1; A1.m[5] = 1; A1.m[10] = 2; A1.m[3] = 12; A1.m[11] = 4; A1.m[15] = 1;
	B1.m[0] = 3; B1.m[5] = 1; B1.m[10] = 4; B1.m[3] = 20; B1.m[11] = 6; B1.m[15] = 1;
	CopyToInstance( inst[0], A0 ); CopyEndToInstance( inst[0], B0 ); inst[0].motionEnabled = true; inst[0].blasIdx = 0;
	CopyToInstance( inst[1], A1 ); CopyEndToInstance( inst[1], B1 ); inst[1].motionEnabled = true; inst[1].blasIdx = 0;
	BVHBase* blases[] = { &blas.bvh };
	BVH tlas; tlas.Build( inst, 2, blases, 1 );

	// Ray aimed at instance 0.
	const bvhvec3 D( 0, 0, 2 );
	Ray r0( bvhvec3( 0.25f, -0.25f, 0 ), D ); r0.time = 0.5f;
	tlas.Intersect( r0 );
	if (r0.hit.inst != 0 || r0.hit.prim != 0) return Fail( G, "shared-BLAS ray 0 resolved to the wrong instance" );
	{
		const RefHit ref = ReferenceMotionHit( r0.O, D, 0.5f, 1e30f );
		if (!ref.hit || !Approx( r0.hit.t, ref.t, 2e-5f )) return Fail( G, "instance 0 numeric mismatch" );
	}

	// Ray aimed at instance 1's independently interpolated position.
	const bvhvec3 O1( 16.25f, -0.25f, 0 );
	Ray r1( O1, D ); r1.time = 0.5f;
	tlas.Intersect( r1 );
	if (r1.hit.inst != 1 || r1.hit.prim != 0) return Fail( G, "shared-BLAS ray 1 resolved to the wrong instance" );
	{
		const RefMat M = LerpMat( A1, B1, 0.5f );
		const bvhvec3 w0 = XformPoint( bvhvec3( LOCAL_TRI[0] ), M );
		const bvhvec3 w1 = XformPoint( bvhvec3( LOCAL_TRI[1] ), M );
		const bvhvec3 w2 = XformPoint( bvhvec3( LOCAL_TRI[2] ), M );
		const RefHit ref = RefIntersect( O1, D, w0, w1, w2, 1e30f );
		if (!ref.hit || !Approx( r1.hit.t, ref.t, 2e-5f ) || !Approx( r1.hit.u, ref.u, 2e-5f ) || !Approx( r1.hit.v, ref.v, 2e-5f ))
			return Fail( G, "instance 1 numeric mismatch" );
	}

	// Interleaving queries at different times must not cross-contaminate the shared BLAS/instances.
	for (int k = 0; k < 16; k++)
	{
		const float time = (k % 4) / 3.0f;
		Ray a( bvhvec3( 0.25f, -0.25f, 0 ), D ); a.time = time;
		Ray b( O1, D ); b.time = 1.0f - time;
		tlas.Intersect( a ); tlas.Intersect( b );
		const RefHit ra = ReferenceMotionHit( a.O, D, time, 1e30f );
		const RefMat Mb = LerpMat( A1, B1, 1.0f - time );
		const RefHit rb = RefIntersect( O1, D, XformPoint( bvhvec3( LOCAL_TRI[0] ), Mb ),
			XformPoint( bvhvec3( LOCAL_TRI[1] ), Mb ), XformPoint( bvhvec3( LOCAL_TRI[2] ), Mb ), 1e30f );
		if (ra.hit != (a.hit.t < BVH_FAR) || rb.hit != (b.hit.t < BVH_FAR)) return Fail( G, "interleaved identity hit/miss mismatch" );
		if (a.hit.t < BVH_FAR && a.hit.inst != 0) return Fail( G, "instance 0 identity lost under interleaving" );
		if (b.hit.t < BVH_FAR && b.hit.inst != 1) return Fail( G, "instance 1 identity lost under interleaving" );
	}
	std::puts( "MOTION identity PASS" );
	return 0;
}

// -----------------------------------------------------------------------------------------------
// Group 5: bundles mixing times, masks and distance limits, for both closest hit and occlusion.
// -----------------------------------------------------------------------------------------------
int TestBundle()
{
	const char* G = "bundle";
	BlasSet blas;
	// One motion and one static instance so the scene is mixed; the static peer is parked away.
	BLASInstance inst[2];
	CopyToInstance( inst[0], OpenPose() ); CopyEndToInstance( inst[0], ClosePose() ); inst[0].motionEnabled = true; inst[0].blasIdx = 0;
	RefMat parked{}; parked.m[0] = parked.m[5] = parked.m[10] = 0.25f; parked.m[3] = 50; parked.m[11] = 30; parked.m[15] = 1;
	CopyToInstance( inst[1], parked ); inst[1].blasIdx = 0;
	BVHBase* blases[] = { &blas.bvh };
	BVH tlas; tlas.Build( inst, 2, blases, 1 );
	// The reachable motion instance drops bit 0, so rays carrying only bit 0 are filtered out.
	const uint32_t instMask = RAY_MASK_INTERSECT_ALL ^ 0x0001u;
	inst[0].mask = instMask;

	const bvhvec3 O( 0.25f, -0.25f, 0 ), D( 0, 0, 2 );
	Ray rays[TINYBVH_BUNDLE_RAYS];
	float limit[TINYBVH_BUNDLE_RAYS];
	struct Expect { bool hit; float t, u, v; };
	std::vector<Expect> want( TINYBVH_BUNDLE_RAYS );
	for (int i = 0; i < TINYBVH_BUNDLE_RAYS; i++)
	{
		const float time = (i % 4) / 3.0f;                 // 0, 1/3, 2/3, 1
		const float tMax = (i % 6 == 0) ? 3.5f : 1e30f;   // a few deliberately short rays
		const uint32_t m = (i % 7 == 0) ? 0x0001u : RAY_MASK_INTERSECT_ALL;
		limit[i] = tMax;
		rays[i] = Ray( O, D, tMax, m );
		rays[i].time = time;
		RefHit ref = ((m & instMask) == 0) ? RefHit{ false, 0, 0, 0 } : ReferenceMotionHit( O, D, time, tMax );
		want[i] = { ref.hit, ref.t, ref.u, ref.v };
	}
	tlas.IntersectBundle( rays );
	for (int i = 0; i < TINYBVH_BUNDLE_RAYS; i++)
	{
		// A hit always shortens the ray below its own limit; a miss leaves hit.t at the limit.
		const bool got = rays[i].hit.t < limit[i];
		if (got != want[i].hit) return Fail( G, "bundle closest-hit hit/miss mismatch" );
		if (want[i].hit && (!Approx( rays[i].hit.t, want[i].t, 2e-5f ) ||
			!Approx( rays[i].hit.u, want[i].u, 2e-5f ) || !Approx( rays[i].hit.v, want[i].v, 2e-5f )))
			return Fail( G, "bundle closest-hit numeric mismatch" );
		if (got && rays[i].hit.inst != 0) return Fail( G, "bundle returned wrong instance" );
	}

	// Occlusion variant of the same mixed packet, rebuilt fresh so hit limits are untouched.
	Ray shadow[TINYBVH_BUNDLE_RAYS];
	Snap shadowSnap[TINYBVH_BUNDLE_RAYS];
	bool occ[TINYBVH_BUNDLE_RAYS];
	for (int i = 0; i < TINYBVH_BUNDLE_RAYS; i++)
	{
		const float time = (i % 4) / 3.0f;
		const float tMax = (i % 6 == 0) ? 3.5f : 1e30f;
		const uint32_t m = (i % 7 == 0) ? 0x0001u : RAY_MASK_INTERSECT_ALL;
		shadow[i] = Ray( O, D, tMax, m ); shadow[i].time = time;
		shadowSnap[i] = Take( shadow[i] );
	}
	tlas.IsOccludedBundle( shadow, occ );
	for (int i = 0; i < TINYBVH_BUNDLE_RAYS; i++)
	{
		const bool maskPass = (shadow[i].mask & instMask) != 0;
		const bool expect = maskPass && ReferenceMotionHit( O, D, shadow[i].time, shadow[i].hit.t ).hit;
		if (occ[i] != expect) return Fail( G, "bundle occlusion mismatch" );
		// IsOccluded must leave every caller-visible field of every ray untouched.
		if (!Same( shadow[i], shadowSnap[i] )) return Fail( G, "occlusion bundle modified an input ray" );
	}
	std::puts( "MOTION bundle PASS" );
	return 0;
}

// -----------------------------------------------------------------------------------------------
// Group 6: endpoint rebuild, static compatibility, and no leftover motion state.
// -----------------------------------------------------------------------------------------------
int TestStatic()
{
	const char* G = "static";
	BlasSet blas;

	// A motion-disabled instance ignores transformEnd entirely and behaves exactly as before.
	{
		BLASInstance inst( 0 );
		CopyToInstance( inst, OpenPose() );
		RefMat junk = ClosePose(); junk.m[3] = 999; junk.m[1] = 7;
		CopyEndToInstance( inst, junk );
		inst.motionEnabled = false; // default; transformEnd must not participate.
		BVHBase* blases[] = { &blas.bvh };
		BVH tlas; tlas.Build( &inst, 1, blases, 1 );
		if (tlas.HasMotionInstances()) return Fail( G, "static instance must not set the motion flag" );
		const bvhvec3 O( -4, -0.25f, 0 ), D( 0, 0, 2 );
		Ray any( O, D ); any.time = 0.7f; // time must be ignored for a static instance
		tlas.Intersect( any );
		const RefMat M = OpenPose();
		const RefHit ref = RefIntersect( O, D, XformPoint( bvhvec3( LOCAL_TRI[0] ), M ),
			XformPoint( bvhvec3( LOCAL_TRI[1] ), M ), XformPoint( bvhvec3( LOCAL_TRI[2] ), M ), 1e30f );
		if (!ref.hit || !Approx( any.hit.t, ref.t, 2e-5f )) return Fail( G, "static instance used transformEnd or time" );
	}

	// Rebuild a motion TLAS with new endpoints and confirm the swept range is recomputed.
	BLASInstance inst( 0 );
	CopyToInstance( inst, OpenPose() ); CopyEndToInstance( inst, ClosePose() ); inst.motionEnabled = true;
	BVHBase* blases[] = { &blas.bvh };
	BVH tlas; tlas.Build( &inst, 1, blases, 1 );
	if (!tlas.HasMotionInstances()) return Fail( G, "initial motion build did not flag motion" );
	const float firstMaxX = inst.aabbMax.x;

	// New close endpoint that reaches much farther; rebuild must grow the swept box.
	RefMat farClose{}; farClose.m[0] = 6; farClose.m[5] = 1; farClose.m[10] = 5; farClose.m[3] = 14; farClose.m[11] = 7; farClose.m[15] = 1;
	CopyEndToInstance( inst, farClose );
	tlas.Build( &inst, 1, blases, 1 );
	if (inst.aabbMax.x <= firstMaxX + EPS) return Fail( G, "rebuild did not recompute the motion range" );

	// Rebuild the same BVH object as an ordinary static triangle BVH: no motion state may linger.
	BVH tri;
	tri.Build( LOCAL_TRI.data(), 1 );
	if (tri.HasMotionInstances() || tri.isTLAS()) return Fail( G, "triangle BVH retained TLAS/motion state" );
	{
		bvhvec4 worldTri[3] = { bvhvec4( -1, -1, 1, 0 ), bvhvec4( 1, -1, 1, 0 ), bvhvec4( 0, 1, 1, 0 ) };
		Ray r( bvhvec3( 0, -0.25f, 0 ), bvhvec3( 0, 0, 2 ) );
		tri.Intersect( r );
		const RefHit ref = RefIntersect( r.O, r.D, bvhvec3( worldTri[0] ), bvhvec3( worldTri[1] ), bvhvec3( worldTri[2] ), 1e30f );
		if (!ref.hit || !Approx( r.hit.t, ref.t, 2e-5f )) return Fail( G, "rebuilt triangle BVH does not intersect correctly" );
	}

	// Turn the formerly motion instance static and rebuild a TLAS: motion flag must clear.
	inst.motionEnabled = false;
	tlas.Build( &inst, 1, blases, 1 );
	if (tlas.HasMotionInstances()) return Fail( G, "static rebuild left the motion flag set" );
	{
		const bvhvec3 O( -4, -0.25f, 0 ), D( 0, 0, 2 );
		Ray r( O, D ); r.time = 0.9f;
		tlas.Intersect( r );
		if (r.hit.t >= BVH_FAR) return Fail( G, "static-rebuilt instance not intersecting" );
	}

	// A static TLAS still uses the vectorized bundle path correctly (per-ray times are ignored).
	{
		Ray rays[TINYBVH_BUNDLE_RAYS];
		const bvhvec3 O( -4, -0.25f, 0 ), D( 0, 0, 2 );
		for (int i = 0; i < TINYBVH_BUNDLE_RAYS; i++) { rays[i] = Ray( O, D ); rays[i].time = i / float( TINYBVH_BUNDLE_RAYS ); }
		tlas.IntersectBundle( rays );
		for (int i = 0; i < TINYBVH_BUNDLE_RAYS; i++) if (!(rays[i].hit.t < BVH_FAR)) return Fail( G, "static TLAS bundle regression" );
	}
	std::puts( "MOTION static PASS" );
	return 0;
}

#ifdef DOUBLE_PRECISION_SUPPORT
// Independent double-precision transformed-triangle reference.
struct DRefHit { bool hit; double t, u, v; };
inline bvhdbl3 DXform( const bvhdbl3& p, const double* M )
{
	return bvhdbl3(
		M[0] * p.x + M[1] * p.y + M[2] * p.z + M[3],
		M[4] * p.x + M[5] * p.y + M[6] * p.z + M[7],
		M[8] * p.x + M[9] * p.y + M[10] * p.z + M[11] );
}
inline DRefHit DRefIntersect( const bvhdbl3& O, const bvhdbl3& D, const bvhdbl3& v0,
	const bvhdbl3& v1, const bvhdbl3& v2, double tMax )
{
	const bvhdbl3 e1 = v1 - v0, e2 = v2 - v0, h = tinybvh_cross( D, e2 );
	const double a = tinybvh_dot( e1, h );
	if (a == 0.0) return { false, 0, 0, 0 };
	const double f = 1.0 / a;
	const bvhdbl3 s = O - v0;
	const double u = f * tinybvh_dot( s, h );
	if (u < 0.0 || u > 1.0) return { false, 0, 0, 0 };
	const bvhdbl3 q = tinybvh_cross( s, e1 );
	const double v = f * tinybvh_dot( D, q );
	if (v < 0.0 || u + v > 1.0) return { false, 0, 0, 0 };
	const double t = f * tinybvh_dot( e2, q );
	if (!(t >= 0.0 && t < tMax)) return { false, 0, 0, 0 };
	return { true, t, u, v };
}

// Double-precision coverage: BVH_Double as a BLAS under a double TLAS with motion, including both
// bundle APIs with per-ray time, mask and distance limits.
int TestDouble()
{
	const char* G = "double";
	const bvhdbl3 loc[3] = { bvhdbl3( -1, -1, 1 ), bvhdbl3( 1, -1, 1 ), bvhdbl3( 0, 1, 1 ) };
	BVH_Double blas; blas.Build( loc, 1 );
	BVH_Double* blases[] = { &blas };
	BLASInstanceEx inst( 0 );
	inst.transform[0] = 1; inst.transform[5] = 1; inst.transform[10] = 2;
	inst.transform[3] = -4; inst.transform[11] = 4; inst.transform[15] = 1;
	inst.transformEnd[0] = 3; inst.transformEnd[5] = 1; inst.transformEnd[10] = 4;
	inst.transformEnd[3] = 4; inst.transformEnd[11] = 6; inst.transformEnd[15] = 1;
	inst.motionEnabled = true;
	// Drop bit 0 so a ray carrying only bit 0 is filtered.
	inst.mask = (uint32_t)RAY_MASK_INTERSECT_ALL ^ 0x1u;
	BVH_Double tlas; tlas.Build( &inst, 1, blases, 1 );
	if (!tlas.HasMotionInstances()) return Fail( G, "double TLAS motion flag" );

	// Single-ray mid-pose hit and an endpoint miss.
	RayEx ray( bvhdbl3( 0.25, -0.25, 0 ), bvhdbl3( 0, 0, 2 ) );
	ray.time = 0.5;
	tlas.Intersect( ray );
	if (std::fabs( ray.hit.t - 4.0 ) > DEPS || std::fabs( ray.hit.u - 0.375 ) > DEPS || std::fabs( ray.hit.v - 0.375 ) > DEPS)
		return Fail( G, "double mid-pose numeric mismatch" );
	RayEx endRay( bvhdbl3( 0.25, -0.25, 0 ), bvhdbl3( 0, 0, 2 ) ); endRay.time = 0.0;
	tlas.Intersect( endRay );
	if (endRay.hit.t < BVH_DBL_FAR) return Fail( G, "double endpoint must miss" );

	// Closest-hit bundle with mixed time, mask and distance limits.
	const bvhdbl3 O( 0.25, -0.25, 0 ), D( 0, 0, 2 );
	RayEx rays[TINYBVH_BUNDLE_RAYS];
	double dlim[TINYBVH_BUNDLE_RAYS];
	for (int i = 0; i < TINYBVH_BUNDLE_RAYS; i++)
	{
		const double time = (i % 4) / 3.0;
		const double tMax = (i % 6 == 0) ? 3.5 : 1e30;
		const uint32_t m = (i % 7 == 0) ? 0x1u : (uint32_t)RAY_MASK_INTERSECT_ALL;
		dlim[i] = tMax;
		rays[i] = RayEx( O, D, tMax, m ); rays[i].time = time;
	}
	tlas.IntersectBundle( rays );
	for (int i = 0; i < TINYBVH_BUNDLE_RAYS; i++)
	{
		const double time = (i % 4) / 3.0;
		const bool maskPass = (rays[i].mask & inst.mask) != 0;
		DRefHit ref{ false, 0, 0, 0 };
		if (maskPass)
		{
			double M[16]; const double a = 1.0 - time;
			for (int k = 0; k < 16; k++) M[k] = a * inst.transform[k] + time * inst.transformEnd[k];
			const bvhdbl3 w0 = DXform( loc[0], M ), w1 = DXform( loc[1], M ), w2 = DXform( loc[2], M );
			ref = DRefIntersect( O, D, w0, w1, w2, dlim[i] );
		}
		const bool got = rays[i].hit.t < dlim[i];
		if (got != ref.hit) return Fail( G, "double bundle hit/miss mismatch" );
		if (ref.hit && (std::fabs( rays[i].hit.t - ref.t ) > 1e-9 ||
			std::fabs( rays[i].hit.u - ref.u ) > 1e-9 || std::fabs( rays[i].hit.v - ref.v ) > 1e-9))
			return Fail( G, "double bundle numeric mismatch" );
	}

	// Occlusion bundle over the same mixed packet; input rays must remain untouched.
	RayEx shadow[TINYBVH_BUNDLE_RAYS];
	bool occ[TINYBVH_BUNDLE_RAYS];
	for (int i = 0; i < TINYBVH_BUNDLE_RAYS; i++)
	{
		const double time = (i % 4) / 3.0;
		const double tMax = (i % 6 == 0) ? 3.5 : 1e30;
		const uint32_t m = (i % 7 == 0) ? 0x1u : (uint32_t)RAY_MASK_INTERSECT_ALL;
		shadow[i] = RayEx( O, D, tMax, m ); shadow[i].time = time;
	}
	tlas.IsOccludedBundle( shadow, occ );
	for (int i = 0; i < TINYBVH_BUNDLE_RAYS; i++)
	{
		const double time = (i % 4) / 3.0;
		const bool maskPass = (shadow[i].mask & inst.mask) != 0;
		bool expect = false;
		if (maskPass)
		{
			double M[16]; const double a = 1.0 - time;
			for (int k = 0; k < 16; k++) M[k] = a * inst.transform[k] + time * inst.transformEnd[k];
			const bvhdbl3 w0 = DXform( loc[0], M ), w1 = DXform( loc[1], M ), w2 = DXform( loc[2], M );
			expect = DRefIntersect( O, D, w0, w1, w2, shadow[i].hit.t ).hit;
		}
		if (occ[i] != expect) return Fail( G, "double occlusion bundle mismatch" );
		if (shadow[i].time != time || shadow[i].mask != rays[i].mask) return Fail( G, "double occlusion bundle mutated a ray" );
	}
	return 0;
}
#endif

} // namespace

int main()
{
	if (TestAffine()) return 1;
	if (TestSwept()) return 1;
	if (TestShadow()) return 1;
	if (TestIdentity()) return 1;
	if (TestBundle()) return 1;
	if (TestStatic()) return 1;
#ifdef DOUBLE_PRECISION_SUPPORT
	if (TestDouble()) return 1;
#endif
	std::puts( "ALL MOTION TESTS PASSED" );
	return 0;
}
