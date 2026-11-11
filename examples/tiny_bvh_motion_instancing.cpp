// Windowless example for linear affine instance motion.
//
// A single static triangle is shared as a BLAS by a motion-enabled BLASInstance. The instance
// stores a shutter-open and a shutter-close local-to-world matrix; the BLAS triangles themselves
// never move. Each ray carries a 'time' in [0, 1], and the TLAS interpolates the instance matrix
// elementwise at that time. One built TLAS answers rays at arbitrary, interleaved times.
//
// Build and run:
//   c++ -std=c++20 -O2 -I. examples/tiny_bvh_motion_instancing.cpp -o motion_instancing
//   ./motion_instancing

#define TINYBVH_IMPLEMENTATION
#include "tiny_bvh.h"
#include <cstdio>

using namespace tinybvh;

static bvhmat4 MakePose( float sx, float sy, float sz, float tx, float ty, float tz )
{
	bvhmat4 m; // identity by default
	m[0] = sx, m[5] = sy, m[10] = sz;
	m[3] = tx, m[7] = ty, m[11] = tz;
	return m;
}

int main()
{
	// 1. A static BLAS: one triangle in local space.
	bvhvec4 triangle[3] = {
		bvhvec4( -1, -1, 1, 0 ), bvhvec4( 1, -1, 1, 0 ), bvhvec4( 0, 1, 1, 0 ) };
	BVH blas;
	blas.Build( triangle, 1 );
	BVHBase* blasList[] = { &blas };

	// 2. A motion instance. 'transform' is the shutter-open pose, 'transformEnd' the close pose.
	BLASInstance inst( 0 );
	inst.transform = MakePose( 1, 1, 2, -4, 0, 4 );    // open:  scale (1,1,2), translate (-4,0,4)
	inst.transformEnd = MakePose( 3, 1, 4, 4, 0, 6 );  // close: scale (3,1,4), translate (4,0,6)
	inst.motionEnabled = true;

	// 3. Build the TLAS once. Its instance bounds cover the entire shutter interval, so rays at
	//    any time traverse the same tree without rebuilding or per-ray allocation.
	BVH tlas;
	tlas.Build( &inst, 1, blasList, 1 );

	// 4. Trace the same world ray at several shutter times.
	const bvhvec3 O( 0.25f, -0.25f, 0.0f ), D( 0, 0, 2 );
	for (int k = 0; k <= 4; k++)
	{
		Ray ray( O, D );
		ray.time = k * 0.25f; // 0 .. 1
		tlas.Intersect( ray );
		if (ray.hit.t < BVH_FAR)
			printf( "time=%.2f: hit t=%.4f u=%.4f v=%.4f inst=%u prim=%u\n",
				ray.time, ray.hit.t, ray.hit.u, ray.hit.v, ray.hit.inst, ray.hit.prim );
		else
			printf( "time=%.2f: no hit\n", ray.time );
	}

	// 5. A shadow ray at a fixed time uses the same half-open distance convention as static rays.
	Ray shadow( O, D, 3.99f );
	shadow.time = 0.5f;
	printf( "occluded before the surface: %s\n", tlas.IsOccluded( shadow ) ? "yes" : "no" );

	// 6. A static instance ignores ray.time and transformEnd entirely.
	BLASInstance still( 0 );
	still.transform = MakePose( 1, 1, 2, -4, 0, 4 );
	// still.motionEnabled stays false; still.transformEnd is not consulted.
	BVH staticTlas;
	staticTlas.Build( &still, 1, blasList, 1 );
	Ray r( bvhvec3( -4, -0.25f, 0 ), D );
	r.time = 0.9f; // has no effect on a static instance
	staticTlas.Intersect( r );
	printf( "static instance at arbitrary ray time: t=%.4f\n", r.hit.t );
	return 0;
}
