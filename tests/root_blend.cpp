// *************************************************************************************************
//
// Horde3D
//   Root motion blend player tests
//
// This software is distributed under the terms of the Eclipse Public License v1.0.
//
// *************************************************************************************************

// check.h declares task_check() and the task_cases table. The driver (driver.c) is compiled by
// the build script with the C++ compiler, so plain C++ linkage is used on both sides.
#include "task_support/check.h"

#include "Horde3D.h"
#include "egModules.h"
#include "egScene.h"
#include "egModel.h"
#include "egAnimatables.h"
#include "egAnimation.h"
#include "egGeometry.h"
#include "egRootBlend.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

using namespace Horde3D;


namespace
{

// -------------------------------------------------------------------------------------------------
// Small scalar reference math (independent of the engine implementation)
// -------------------------------------------------------------------------------------------------

struct RVec3 { double x, y, z; };

struct RMat4
{
	double m[16];  // column major, same layout as Matrix4f

	RMat4()
	{
		for( int i = 0; i < 16; ++i ) m[i] = 0.0;
		m[0] = m[5] = m[10] = m[15] = 1.0;
	}

	static RMat4 trans( double x, double y, double z )
	{
		RMat4 r; r.m[12] = x; r.m[13] = y; r.m[14] = z; return r;
	}
	static RMat4 rotY( double yaw )
	{
		double c = cos( yaw ), s = sin( yaw );
		RMat4 r;
		r.m[0] = c;   r.m[2] = -s;
		r.m[8] = s;   r.m[10] = c;
		return r;
	}
};


static RMat4 operator*( const RMat4 &a, const RMat4 &b )
{
	RMat4 r;
	for( int col = 0; col < 4; ++col )
		for( int row = 0; row < 4; ++row )
		{
			double v = 0.0;
			for( int k = 0; k < 4; ++k ) v += a.m[k * 4 + row] * b.m[col * 4 + k];
			r.m[col * 4 + row] = v;
		}
	return r;
}


static RVec3 operator*( const RMat4 &a, const RVec3 &v )
{
	RVec3 r;
	r.x = a.m[0] * v.x + a.m[4] * v.y + a.m[8]  * v.z + a.m[12];
	r.y = a.m[1] * v.x + a.m[5] * v.y + a.m[9]  * v.z + a.m[13];
	r.z = a.m[2] * v.x + a.m[6] * v.y + a.m[10] * v.z + a.m[14];
	return r;
}


static RMat4 rigidInverse( const RMat4 &a )
{
	RMat4 r;
	for( int i = 0; i < 3; ++i )
		for( int j = 0; j < 3; ++j ) r.m[i * 4 + j] = a.m[j * 4 + i];
	r.m[12] = -( r.m[0] * a.m[12] + r.m[4] * a.m[13] + r.m[8]  * a.m[14] );
	r.m[13] = -( r.m[1] * a.m[12] + r.m[5] * a.m[13] + r.m[9]  * a.m[14] );
	r.m[14] = -( r.m[2] * a.m[12] + r.m[6] * a.m[13] + r.m[10] * a.m[14] );
	return r;
}


struct RQuat { double x, y, z, w; };

static RQuat yawQuat( double yaw )
{
	RQuat q { 0.0, sin( yaw * 0.5 ), 0.0, cos( yaw * 0.5 ) };
	return q;
}


// Shortest-arc normalized linear quaternion interpolation, implemented scalarly.
static RQuat nlerp( const RQuat &a, const RQuat &b, double t )
{
	double bx = b.x, by = b.y, bz = b.z, bw = b.w;
	if( a.x * bx + a.y * by + a.z * bz + a.w * bw < 0.0 )
	{ bx = -bx; by = -by; bz = -bz; bw = -bw; }
	RQuat q
	{
		a.x + ( bx - a.x ) * t,
		a.y + ( by - a.y ) * t,
		a.z + ( bz - a.z ) * t,
		a.w + ( bw - a.w ) * t
	};
	double inv = 1.0 / sqrt( q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w );
	q.x *= inv; q.y *= inv; q.z *= inv; q.w *= inv;
	return q;
}


static double quatYaw( const RQuat &q )
{
	// Rotation matrix row 2, columns 0 and 2 (same entries the engine reads from Matrix4f:
	// x[8] = 2(xz + wy), x[10] = 1 - 2(x^2 + y^2)).
	double m20 = 2.0 * ( q.z * q.x + q.w * q.y );
	double m22 = 1.0 - 2.0 * ( q.x * q.x + q.y * q.y );
	return atan2( m20, m22 );
}


static double wrapPi( double a )
{
	while( a > 3.14159265358979323846 ) a -= 6.28318530717958647692;
	while( a <= -3.14159265358979323846 ) a += 6.28318530717958647692;
	return a;
}


// -------------------------------------------------------------------------------------------------
// Procedural H3DA / H3DG generation
// -------------------------------------------------------------------------------------------------

struct Bytes
{
	std::vector<char> data;

	void u32( uint32_t v ) { for( int i = 0; i < 4; ++i ) data.push_back( char( v >> ( 8 * i ) ) ); }
	void u16( int v ) { data.push_back( char( v ) ); data.push_back( char( v >> 8 ) ); }
	void u8( int v ) { data.push_back( char( v ) ); }
	void f32( float v ) { uint32_t bits; std::memcpy( &bits, &v, 4 ); u32( bits ); }
	void text( const char *s, size_t n )
	{
		size_t len = std::strlen( s );
		for( size_t i = 0; i < n; ++i ) data.push_back( i < len ? s[i] : 0 );
	}
	void mat4( const RMat4 &m ) { for( int i = 0; i < 16; ++i ) f32( (float)m.m[i] ); }
	void frame( const RQuat &q, double tx, double ty, double tz, double sx = 1.0,
	            double sy = 1.0, double sz = 1.0 )
	{
		f32( (float)q.x ); f32( (float)q.y ); f32( (float)q.z ); f32( (float)q.w );
		f32( (float)tx ); f32( (float)ty ); f32( (float)tz );
		f32( (float)sx ); f32( (float)sy ); f32( (float)sz );
	}
};


struct AnimFrame { RQuat q; RVec3 t; };
struct AnimEntity
{
	std::string name;
	bool compressed;
	std::vector< AnimFrame > frames;
};


static Bytes buildAnim( uint32_t numFrames, const std::vector< AnimEntity > &entities )
{
	Bytes b;
	b.text( "H3DA", 4 );
	b.u32( 3 );
	b.u32( (uint32_t)entities.size() );
	b.u32( numFrames );
	for( const AnimEntity &e : entities )
	{
		b.text( e.name.c_str(), 256 );
		b.u8( e.compressed ? 1 : 0 );
		for( const AnimFrame &f : e.frames )
			b.frame( f.q, f.t.x, f.t.y, f.t.z );
	}
	return b;
}


// Reference description of a planar root track (the test owns these numbers).
struct RefClip
{
	uint32_t N;
	double fps;
	std::vector<double> yaw;   // size N
	std::vector<double> px;    // size N
	std::vector<double> pz;    // size N
	double py;

	double period() const { return (double)( N - 1 ) / fps; }
};


struct RefLimbTrack
{
	uint32_t N;
	double fps;
	std::vector<RQuat> q;
	std::vector<RVec3> t;
};


static AnimFrame rootFrame( const RefClip &c, uint32_t i, bool negateQuat )
{
	RQuat q = yawQuat( c.yaw[i] );
	if( negateQuat ) { q.x = -q.x; q.y = -q.y; q.z = -q.z; q.w = -q.w; }
	return AnimFrame { q, RVec3 { c.px[i], c.py, c.pz[i] } };
}


static Bytes buildRootLimbAnim( const RefClip &root, const RefLimbTrack &limb, bool negateRootQuat,
                                bool addConstantJoint )
{
	std::vector< AnimEntity > entities;
	AnimEntity rootEnt;
	rootEnt.name = "root";
	rootEnt.compressed = false;
	for( uint32_t i = 0; i < root.N; ++i ) rootEnt.frames.push_back( rootFrame( root, i, negateRootQuat ) );
	entities.push_back( rootEnt );

	AnimEntity limbEnt;
	limbEnt.name = "limb";
	limbEnt.compressed = false;
	for( uint32_t i = 0; i < limb.N; ++i )
		limbEnt.frames.push_back( AnimFrame { limb.q[i], limb.t[i] } );
	entities.push_back( limbEnt );

	if( addConstantJoint )
	{
		AnimEntity constEnt;
		constEnt.name = "constant";
		constEnt.compressed = true;
		constEnt.frames.push_back( AnimFrame { yawQuat( 0.0 ), RVec3 { 0, 0, 0 } } );
		entities.push_back( constEnt );
	}
	return buildAnim( root.N, entities );
}


// Geometry: two joints (root, limb), three vertices, one morph target.
static Bytes buildGeometry( const RMat4 &invBindRoot, const RMat4 &invBindLimb,
                            const RVec3 verts[3] )
{
	Bytes b;
	b.text( "H3DG", 4 );
	b.u32( 5 );
	b.u32( 2 );                       // joint count
	b.mat4( invBindRoot );
	b.mat4( invBindLimb );

	b.u32( 6 );                       // stream count
	b.u32( 3 );                       // vertex count

	// Stream 0: positions
	b.u32( 0 ); b.u32( 12 );
	for( int i = 0; i < 3; ++i ) { b.f32( (float)verts[i].x ); b.f32( (float)verts[i].y ); b.f32( (float)verts[i].z ); }
	// Streams 1..3: normal, tangent, bitangent
	for( int stream = 1; stream <= 3; ++stream )
	{
		b.u32( stream ); b.u32( 6 );
		for( int i = 0; i < 3; ++i )
			for( int c = 0; c < 3; ++c ) b.u16( c == stream - 1 ? 32767 : 0 );
	}
	// Stream 4: joint indices
	b.u32( 4 ); b.u32( 4 );
	const unsigned char joints[3][4] = { {0,0,0,0}, {1,1,1,1}, {0,1,0,1} };
	for( int i = 0; i < 3; ++i ) for( int c = 0; c < 4; ++c ) b.u8( joints[i][c] );
	// Stream 5: weights (v2 gets a 0.502/0.498 split)
	b.u32( 5 ); b.u32( 4 );
	const unsigned char weights[3][4] = { {255,0,0,0}, {0,255,0,0}, {128,127,0,0} };
	for( int i = 0; i < 3; ++i ) for( int c = 0; c < 4; ++c ) b.u8( weights[i][c] );

	// Indices (one triangle)
	b.u32( 3 );
	b.u32( 0 ); b.u32( 1 ); b.u32( 2 );

	// Morph target "lift": one affected vertex (index 0), position diff (0, 1, 0)
	b.u32( 1 );
	b.text( "lift", 256 );
	b.u32( 1 );
	b.u32( 0 );
	b.u32( 3 );  // three streams
	b.u32( 0 ); b.u32( 12 ); b.f32( 0 ); b.f32( 1 ); b.f32( 0 );
	b.u32( 1 ); b.u32( 12 ); b.f32( 0 ); b.f32( 0 ); b.f32( 0 );
	b.u32( 2 ); b.u32( 12 ); b.f32( 0 ); b.f32( 0 ); b.f32( 0 );
	return b;
}


static H3DRes loadResource( int type, const char *name, const Bytes &bytes )
{
	H3DRes r = h3dAddResource( type, name, 0 );
	if( r && h3dLoadResource( r, bytes.data.data(), (int)bytes.data.size() ) ) return r;
	return 0;
}


// -------------------------------------------------------------------------------------------------
// Independent reference player
// -------------------------------------------------------------------------------------------------

struct RefPlayer
{
	RefClip clip[2];
	// Stored as floats and promoted to double on every use, exactly like the engine's config,
	// so boundary placement and signed rates are bit-identical between both implementations.
	float phase[2], rate[2], fps[2];
	std::vector< RootBlendWeightKey > keys;
	double startTheta, startX, startZ, startY;

	double period( int c ) const { return (double)( clip[c].N - 1 ) / (double)fps[c]; }


	double weight( double t ) const
	{
		if( t <= keys.front().t ) return keys.front().w;
		if( t >= keys.back().t ) return keys.back().w;
		size_t lo = 0, hi = keys.size() - 1;
		while( lo + 1 < hi )
		{
			size_t mid = ( lo + hi ) / 2;
			if( keys[mid].t <= t ) lo = mid; else hi = mid;
		}
		double f = ( t - keys[lo].t ) / ( keys[lo + 1].t - keys[lo].t );
		return keys[lo].w + ( keys[lo + 1].w - keys[lo].w ) * f;
	}


	// Local frame time u of a clip at absolute time t.
	void localTime( int c, double t, uint32_t &seg, double &amt, double &yawNow ) const
	{
		const RefClip &cl = clip[c];
		double per = period( c );
		double s = (double)phase[c] + (double)rate[c] * t;
		double cycle = floor( s / per );
		double u = s - cycle * per;
		if( u < 0.0 ) u = 0.0;
		if( u > per ) u = per;
		double alpha = u * (double)fps[c];
		if( alpha >= (double)cl.N - 1.0 ) { seg = cl.N - 2; amt = 1.0; }
		else { seg = (uint32_t)floor( alpha ); amt = alpha - (double)seg; }

		double d = wrapPi( cl.yaw[seg + 1] - cl.yaw[seg] );
		RQuat q = nlerp( yawQuat( cl.yaw[seg] ), yawQuat( cl.yaw[seg] + d ), amt );
		yawNow = quatYaw( q );
	}


	void velocity( int c, double t, double &vxBody, double &vzBody, double &omega ) const
	{
		const RefClip &cl = clip[c];
		uint32_t seg; double amt, yawNow;
		localTime( c, t, seg, amt, yawNow );

		double dpx = ( cl.px[seg + 1] - cl.px[seg] ) * (double)fps[c] * (double)rate[c];
		double dpz = ( cl.pz[seg + 1] - cl.pz[seg] ) * (double)fps[c] * (double)rate[c];

		// Analytic derivative of the shortest-arc nlerp yaw curve.
		double y0 = cl.yaw[seg];
		double d = wrapPi( cl.yaw[seg + 1] - y0 );
		double sh0 = sin( y0 * 0.5 ), ch0 = cos( y0 * 0.5 );
		double sh1 = sin( ( y0 + d ) * 0.5 ), ch1 = cos( ( y0 + d ) * 0.5 );
		double sa = sh0 + ( sh1 - sh0 ) * amt;
		double ca = ch0 + ( ch1 - ch0 ) * amt;
		double n2 = sa * sa + ca * ca;
		double dyawAmt = 2.0 * ( ca * ( sh1 - sh0 ) - sa * ( ch1 - ch0 ) ) / n2;
		omega = dyawAmt * (double)fps[c] * (double)rate[c];

		double co = cos( yawNow ), si = sin( yawNow );
		vxBody = co * dpx - si * dpz;
		vzBody = si * dpx + co * dpz;
	}


	void deriv( double t, double theta, double &dx, double &dz, double &dtheta ) const
	{
		double vx0, vz0, om0, vx1, vz1, om1;
		velocity( 0, t, vx0, vz0, om0 );
		velocity( 1, t, vx1, vz1, om1 );
		double w = weight( t );
		double vx = ( 1.0 - w ) * vx0 + w * vx1;
		double vz = ( 1.0 - w ) * vz0 + w * vz1;
		dtheta = ( 1.0 - w ) * om0 + w * om1;
		double co = cos( theta ), si = sin( theta );
		dx = co * vx + si * vz;
		dz = -si * vx + co * vz;
	}


	double nextCrossing( int c, double cur ) const
	{
		// Frame index boundary: phase + rate*t is an integer (a velocity kink).
		if( rate[c] == 0.0f ) return 100.0;
		double s = (double)phase[c] + (double)rate[c] * cur;
		double k = rate[c] > 0.0f ? floor( s ) + 1.0 : ceil( s ) - 1.0;
		return ( k - (double)phase[c] ) / (double)rate[c];
	}


	struct Pose { double theta, x, z; };


	Pose integrate( double endT ) const
	{
		Pose s { startTheta, startX, startZ };
		double cur = 0.0;
		size_t ki = 0;
		const double hMax = 1.0 / 2048.0;
		while( cur < endT )
		{
			double ev = endT;
			while( ki < keys.size() && keys[ki].t <= cur ) ++ki;
			if( ki < keys.size() && keys[ki].t > cur ) ev = std::min( ev, keys[ki].t );
			double c0 = nextCrossing( 0, cur ); if( c0 > cur ) ev = std::min( ev, c0 );
			double c1 = nextCrossing( 1, cur ); if( c1 > cur ) ev = std::min( ev, c1 );
			if( ev <= cur ) ev = endT;  // No usable event ahead: finish the interval
			if( ev > endT ) ev = endT;

			while( cur < ev )
			{
				double h = std::min( hMax, ev - cur );
				double k1x, k1z, k1t; deriv( cur, s.theta, k1x, k1z, k1t );
				double k2x, k2z, k2t; deriv( cur + h * 0.5, s.theta + h * 0.5 * k1t, k2x, k2z, k2t );
				double k3x, k3z, k3t; deriv( cur + h * 0.5, s.theta + h * 0.5 * k2t, k3x, k3z, k3t );
				double k4x, k4z, k4t; deriv( cur + h, s.theta + h * k3t, k4x, k4z, k4t );
				s.x += h / 6.0 * ( k1x + 2.0 * k2x + 2.0 * k3x + k4x );
				s.z += h / 6.0 * ( k1z + 2.0 * k2z + 2.0 * k3z + k4z );
				s.theta += h / 6.0 * ( k1t + 2.0 * k2t + 2.0 * k3t + k4t );
				cur += h;
			}
			cur = ev;
		}
		return s;
	}
};


// -------------------------------------------------------------------------------------------------
// Shared fixture data
// -------------------------------------------------------------------------------------------------

struct Fixture
{
	H3DRes animA = 0, animB = 0, animSlide = 0, geo = 0, material = 0;
	RefClip refA, refB, refSlide;
	RefLimbTrack limbA, limbB;
	RMat4 invBindRoot, invBindLimb;
	RVec3 baseVerts[3];


	void init()
	{
		// Clip A: 4 frames @ 10 fps (period 0.3s). Quarter turns walking a square.
		refA.N = 4; refA.fps = 10.0; refA.py = 0.0;
		refA.yaw = { 0.0, 1.5707963267948966, 3.1415926535897932, 4.71238898038469 };
		refA.px = { 0.0, 1.0, 1.0, 0.0 };
		refA.pz = { 0.0, 0.0, -1.0, -1.0 };
		limbA.N = 4; limbA.fps = 10.0;
		for( uint32_t i = 0; i < 4; ++i )
		{
			limbA.q.push_back( yawQuat( 0.0 ) );
			limbA.t.push_back( RVec3 { 0.0, 1.0 + 0.1 * i, 0.0 } );
		}

		// Clip B: 6 frames @ 15 fps (period 1/3s), 30 degree turns, straight 2-unit steps.
		refB.N = 6; refB.fps = 15.0; refB.py = 0.0;
		for( uint32_t i = 0; i < 6; ++i )
		{
			refB.yaw.push_back( i * 3.1415926535897932 / 6.0 );
			refB.px.push_back( 2.0 * i );
			refB.pz.push_back( 0.0 );
		}
		limbB.N = 6; limbB.fps = 15.0;
		for( uint32_t i = 0; i < 6; ++i )
		{
			limbB.q.push_back( yawQuat( 0.0 ) );
			limbB.t.push_back( RVec3 { 0.0, 2.0 + 0.05 * i, 0.0 } );
		}

		// Slide clip: 3 frames @ 10 fps, no rotation, one unit per frame -> body speed 10.
		refSlide.N = 3; refSlide.fps = 10.0; refSlide.py = 0.0;
		for( uint32_t i = 0; i < 3; ++i )
		{
			refSlide.yaw.push_back( 0.0 );
			refSlide.px.push_back( (double)i );
			refSlide.pz.push_back( 0.0 );
		}

		Bytes bytesA = buildRootLimbAnim( refA, limbA, false, true );
		Bytes bytesB = buildRootLimbAnim( refB, limbB, true, true );  // negated root quaternions
		animA = loadResource( H3DResTypes::Animation, "clipA.anim", bytesA );
		animB = loadResource( H3DResTypes::Animation, "clipB.anim", bytesB );

		// Slide reuses a single limb track.
		RefLimbTrack slideLimb;
		slideLimb.N = 3; slideLimb.fps = 10.0;
		for( uint32_t i = 0; i < 3; ++i )
		{
			slideLimb.q.push_back( yawQuat( 0.0 ) );
			slideLimb.t.push_back( RVec3 { 0.0, 1.0, 0.0 } );
		}
		RefClip slideRoot = refSlide;
		Bytes bytesSlide = buildRootLimbAnim( slideRoot, slideLimb, false, false );
		animSlide = loadResource( H3DResTypes::Animation, "slide.anim", bytesSlide );

		invBindRoot = RMat4::trans( 0.5, 0.0, -0.25 ) * RMat4::rotY( 0.3 );
		invBindLimb = RMat4::trans( 0.0, 1.0, 0.0 );
		baseVerts[0] = RVec3 { 0.2, 0.3, 0.4 };
		baseVerts[1] = RVec3 { 0.1, 0.1, 0.1 };
		baseVerts[2] = RVec3 { -0.3, 0.7, 0.2 };
		Bytes geoBytes = buildGeometry( invBindRoot, invBindLimb, baseVerts );
		geo = loadResource( H3DResTypes::Geometry, "skin.geo", geoBytes );
		material = h3dAddResource( H3DResTypes::Material, "unrendered.material", 0 );
	}


	RootBlendConfig baseConfig( ModelNode *model, const std::vector< RootBlendWeightKey > &keys,
	                            Matrix4f start = Matrix4f() ) const
	{
		RootBlendConfig cfg;
		cfg.anim[0] = (AnimationResource *)Modules::resMan().resolveResHandle( animA );
		cfg.anim[1] = (AnimationResource *)Modules::resMan().resolveResHandle( animB );
		cfg.fps[0] = 10.0f; cfg.fps[1] = 15.0f;
		cfg.phase[0] = 0.0f; cfg.phase[1] = 0.0f;
		cfg.rate[0] = 1.0f; cfg.rate[1] = 1.0f;
		cfg.rootName = "root";
		cfg.model = model;
		cfg.startRoot = start;
		cfg.keys = keys.data();
		cfg.numKeys = keys.size();
		return cfg;
	}


	RefPlayer baseReference( const std::vector< RootBlendWeightKey > &keys,
	                         double theta0 = 0.0, double x0 = 0.0, double z0 = 0.0 ) const
	{
		RefPlayer ref;
		ref.clip[0] = refA; ref.clip[1] = refB;
		ref.fps[0] = 10.0f; ref.fps[1] = 15.0f;
		ref.phase[0] = ref.phase[1] = 0.0f;
		ref.rate[0] = ref.rate[1] = 1.0f;
		ref.keys = keys;
		ref.startTheta = theta0; ref.startX = x0; ref.startZ = z0; ref.startY = 0.0;
		return ref;
	}
};


static bool nearVal( double a, double b, double eps )
{
	return std::isfinite( a ) && std::isfinite( b ) && fabs( a - b ) <= eps;
}


static bool nearMat( const Matrix4f &a, const RMat4 &b, double eps )
{
	for( int i = 0; i < 16; ++i ) if( !nearVal( a.x[i], b.m[i], eps ) ) return false;
	return true;
}


// Builds the standard scene: parent group (with transform), model, root + limb + constant joints,
// mesh, software skinning enabled. Returns handles through the out parameters.
struct SceneHandles
{
	H3DNode parent, model, root, limb, constant, mesh;
};


static SceneHandles buildScene( H3DRes geo, H3DRes material,
                                float px = 0.0f, float py = 0.0f, float pz = 0.0f,
                                float ry = 0.0f )
{
	SceneHandles h;
	h.parent = h3dAddGroupNode( H3DRootNode, "placement" );
	h3dSetNodeTransform( h.parent, px, py, pz, 0.0f, ry, 0.0f, 1.0f, 1.0f, 1.0f );
	h.model = h3dAddModelNode( h.parent, "character", geo );
	h3dSetNodeParamI( h.model, H3DModel::SWSkinningI, 1 );
	h.root = h3dAddJointNode( h.model, "root", 0 );
	h.limb = h3dAddJointNode( h.root, "limb", 1 );
	h.constant = h3dAddJointNode( h.limb, "constant", 2 );
	h.mesh = h3dAddMeshNode( h.model, "triangle", material, H3DMeshPrimType::TriangleList,
	                         0, 3, 0, 2 );
	return h;
}


// Expected blended limb local matrix at time t (same-layer non-additive TRS blend, scalar ref).
static RMat4 refLimbLocal( const Fixture &fx, const RefPlayer &ref, double t )
{
	double w = ref.weight( t );
	RMat4 mats[2];
	for( int c = 0; c < 2; ++c )
	{
		const RefLimbTrack &lt = c == 0 ? fx.limbA : fx.limbB;
		const RefClip &cl = ref.clip[c];
		double s = ref.phase[c] + ref.rate[c] * t;
		double cycle = floor( s / cl.period() );
		double u = s - cycle * cl.period();
		double alpha = u * cl.fps;
		uint32_t f0; double amt;
		if( alpha >= (double)cl.N - 1.0 ) { f0 = cl.N - 2; amt = 1.0; }
		else { f0 = (uint32_t)floor( alpha ); amt = alpha - (double)f0; }
		RQuat q = nlerp( lt.q[f0], lt.q[f0 + 1], amt );
		double tx = lt.t[f0].x + ( lt.t[f0 + 1].x - lt.t[f0].x ) * amt;
		double ty = lt.t[f0].y + ( lt.t[f0 + 1].y - lt.t[f0].y ) * amt;
		double tz = lt.t[f0].z + ( lt.t[f0 + 1].z - lt.t[f0].z ) * amt;
		RMat4 r = RMat4::rotY( quatYaw( q ) );
		r.m[12] = tx; r.m[13] = ty; r.m[14] = tz;
		mats[c] = r;
	}
	// Limb tracks are yaw-only with identity rotation here, so a matrix blend reduces to a
	// translation lerp followed by the (identical) rotation; do the generic TRS math anyway.
	RMat4 out;
	for( int i = 0; i < 16; ++i )
		out.m[i] = ( 1.0 - w ) * mats[0].m[i] + w * mats[1].m[i];
	return out;
}


// Independent expected world position of a skinned vertex (morph included).
static RVec3 refWorldVertex( const Fixture &fx, const RefPlayer &ref, double t,
                             const RMat4 &absModel, int vertIndex, double morphWeight )
{
	RefPlayer::Pose p = ref.integrate( t );
	RMat4 worldRoot = RMat4::rotY( p.theta );
	worldRoot.m[12] = p.x; worldRoot.m[13] = ref.startY; worldRoot.m[14] = p.z;

	RMat4 rootLocal = rigidInverse( absModel ) * worldRoot;
	RMat4 limbLocal = refLimbLocal( fx, ref, t );
	RMat4 relLimb = rootLocal * limbLocal;

	RMat4 skin[2];
	skin[0] = rootLocal * fx.invBindRoot;
	skin[1] = relLimb * fx.invBindLimb;

	static const double wtab[3][4] =
	{
		{ 1.0, 0.0, 0.0, 0.0 },
		{ 0.0, 1.0, 0.0, 0.0 },
		{ 128.0 / 255.0, 127.0 / 255.0, 0.0, 0.0 }
	};
	static const int jtab[3][4] =
	{
		{ 0, 0, 0, 0 }, { 1, 1, 1, 1 }, { 0, 1, 0, 1 }
	};

	RVec3 v = fx.baseVerts[vertIndex];
	if( vertIndex == 0 ) v.y += morphWeight;  // morph diff (0,1,0) * weight
	RVec3 modelPos { 0.0, 0.0, 0.0 };
	for( int k = 0; k < 4; ++k )
	{
		double wgt = wtab[vertIndex][k];
		if( wgt == 0.0 ) continue;
		RVec3 sv = skin[jtab[vertIndex][k]] * v;
		modelPos.x += wgt * sv.x; modelPos.y += wgt * sv.y; modelPos.z += wgt * sv.z;
	}
	RVec3 out = absModel * modelPos;
	return out;
}


static std::vector< RootBlendWeightKey > defaultKeys()
{
	return { { 0.0, 0.0f }, { 1.0, 1.0f }, { 2.0, 0.25f }, { 5.0, 0.75f } };
}


static bool startEngine()
{
	return h3dInit( (H3DRenderDevice::List)256 ) &&
	       h3dSetOption( H3DOptions::FastAnimation, 0 );
}


// RAII engine lifecycle. Declared before any player so that players (which unbind themselves in
// their destructor) are still alive while the engine exists.
struct EngineGuard
{
	bool ok;
	EngineGuard() : ok( startEngine() ) {}
	~EngineGuard() { if( ok ) h3dRelease(); }
};

}  // namespace


// =================================================================================================
// twist group: rotation, quaternion signs, fps, phase, reverse playback
// =================================================================================================

static int twist_negated_quaternions()
{
	EngineGuard guard; TASK_CHECK( guard.ok );
	Fixture fx; fx.init();
	TASK_CHECK( fx.animA && fx.animB && fx.geo && fx.material );
	SceneHandles h = buildScene( fx.geo, fx.material );
	auto keys = std::vector< RootBlendWeightKey > { { 0.0, 1.0f } };  // clip B only, negated quats
	RootBlendPlayer player;
	TASK_CHECK( player.initialize( fx.baseConfig(
		(ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model ), keys ) ) );

		RefPlayer ref = fx.baseReference( keys );
	for( double t : { 0.05, 0.17, 0.33, 0.34, 0.4, 0.5, 0.66 } )
	{
		TASK_CHECK( player.evaluate( t ) );
		Matrix4f wr; TASK_CHECK( player.getWorldRoot( wr ) );
		RefPlayer::Pose p = ref.integrate( t );
		TASK_CHECK( nearVal( wr.x[12], p.x, 2e-5 ) );
		TASK_CHECK( nearVal( wr.x[14], p.z, 2e-5 ) );
		TASK_CHECK( nearVal( atan2( (double)wr.x[8], (double)wr.x[10] ),
		                     wrapPi( p.theta ), 2e-5 ) );
	}
	return 0;
}


static int twist_different_fps_single_clip()
{
	EngineGuard guard; TASK_CHECK( guard.ok );
	Fixture fx; fx.init();
	SceneHandles h = buildScene( fx.geo, fx.material );

	// Clip A alone (fps 10) and clip B alone (fps 15), one period each.
	for( int clip = 0; clip < 2; ++clip )
	{
		std::vector< RootBlendWeightKey > keys { { 0.0, clip == 0 ? 0.0f : 1.0f } };
		RootBlendPlayer player;
		RootBlendConfig cfg = fx.baseConfig(
			(ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model ), keys );
		TASK_CHECK( player.initialize( cfg ) );
		RefPlayer ref = fx.baseReference( keys );
		double period = clip == 0 ? fx.refA.period() : fx.refB.period();
		for( double t : { period * 0.25, period * 0.5, period * 0.99 } )
		{
			TASK_CHECK( player.evaluate( t ) );
			Matrix4f wr; player.getWorldRoot( wr );
			RefPlayer::Pose p = ref.integrate( t );
			TASK_CHECK( nearVal( wr.x[12], p.x, 2e-5 ) );
			TASK_CHECK( nearVal( wr.x[14], p.z, 2e-5 ) );
		}
		TASK_CHECK( player.unbind() );
	}
	return 0;
}


static int twist_nonzero_phase()
{
	EngineGuard guard; TASK_CHECK( guard.ok );
	Fixture fx; fx.init();
	SceneHandles h = buildScene( fx.geo, fx.material );
	auto keys = defaultKeys();
	RootBlendConfig cfg = fx.baseConfig(
		(ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model ), keys );
	cfg.phase[0] = 0.37f; cfg.phase[1] = -0.21f;
	RootBlendPlayer player;
	TASK_CHECK( player.initialize( cfg ) );

	RefPlayer ref = fx.baseReference( keys );
	ref.phase[0] = 0.37; ref.phase[1] = -0.21;
	for( double t : { 0.1, 0.8, 1.5, 2.7 } )
	{
		TASK_CHECK( player.evaluate( t ) );
		Matrix4f wr; player.getWorldRoot( wr );
		RefPlayer::Pose p = ref.integrate( t );
		TASK_CHECK( nearVal( wr.x[12], p.x, 2e-5 ) );
		TASK_CHECK( nearVal( wr.x[14], p.z, 2e-5 ) );
		TASK_CHECK( nearVal( atan2( (double)wr.x[8], (double)wr.x[10] ),
		                     wrapPi( p.theta ), 2e-5 ) );
	}
	return 0;
}


static int twist_reverse_across_cycles()
{
	EngineGuard guard; TASK_CHECK( guard.ok );
	Fixture fx; fx.init();
	SceneHandles h = buildScene( fx.geo, fx.material );
	auto keys = std::vector< RootBlendWeightKey > { { 0.0, 0.4f } };
	RootBlendConfig cfg = fx.baseConfig(
		(ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model ), keys );
	cfg.rate[0] = -1.5f; cfg.rate[1] = -2.25f;
	cfg.phase[0] = 1.3f; cfg.phase[1] = 0.7f;
	RootBlendPlayer player;
	TASK_CHECK( player.initialize( cfg ) );

	RefPlayer ref = fx.baseReference( keys );
	ref.rate[0] = -1.5; ref.rate[1] = -2.25;
	ref.phase[0] = 1.3; ref.phase[1] = 0.7;
	// Times span several periods of both clips (periods 0.3s and 1/3s).
	for( double t : { 0.0, 0.4, 1.1, 2.3, 4.9 } )
	{
		TASK_CHECK( player.evaluate( t ) );
		Matrix4f wr; player.getWorldRoot( wr );
		RefPlayer::Pose p = ref.integrate( t );
		TASK_CHECK( nearVal( wr.x[12], p.x, 2e-5 ) );
		TASK_CHECK( nearVal( wr.x[14], p.z, 2e-5 ) );
		TASK_CHECK( nearVal( atan2( (double)wr.x[8], (double)wr.x[10] ),
		                     wrapPi( p.theta ), 2e-5 ) );
	}
	return 0;
}


// =================================================================================================
// integration group
// =================================================================================================

static int integration_straight_line_exact()
{
	EngineGuard guard; TASK_CHECK( guard.ok );
	Fixture fx; fx.init();
	SceneHandles h = buildScene( fx.geo, fx.material );
	auto keys = std::vector< RootBlendWeightKey > { { 0.0, 0.0f } };

	RootBlendConfig cfg;
	cfg.anim[0] = (AnimationResource *)Modules::resMan().resolveResHandle( fx.animSlide );
	cfg.anim[1] = (AnimationResource *)Modules::resMan().resolveResHandle( fx.animSlide );
	cfg.fps[0] = cfg.fps[1] = 10.0f;
	cfg.phase[0] = cfg.phase[1] = 0.0f;
	cfg.rate[0] = cfg.rate[1] = 1.0f;
	cfg.rootName = "root";
	cfg.model = (ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model );
	cfg.keys = keys.data(); cfg.numKeys = keys.size();

	RootBlendPlayer player;
	TASK_CHECK( player.initialize( cfg ) );
	// Body speed is exactly 10 m/s; over 2.5 s facing +x: displacement 25, no rotation.
	TASK_CHECK( player.evaluate( 2.5 ) );
	Matrix4f wr; player.getWorldRoot( wr );
	TASK_CHECK( nearVal( wr.x[12], 25.0, 2e-5 ) );
	TASK_CHECK( nearVal( wr.x[14], 0.0, 2e-5 ) );
	TASK_CHECK( nearVal( wr.x[0], 1.0, 2e-6 ) && nearVal( wr.x[10], 1.0, 2e-6 ) );
	// Zero rate freezes the body. A second player can only bind after the first is released.
	TASK_CHECK( player.unbind() );
	cfg.rate[0] = cfg.rate[1] = 0.0f;
	RootBlendPlayer paused;
	TASK_CHECK( paused.initialize( cfg ) );
	TASK_CHECK( paused.evaluate( 7.0 ) );
	Matrix4f wr2; paused.getWorldRoot( wr2 );
	TASK_CHECK( nearVal( wr2.x[12], 0.0, 2e-6 ) );
	return 0;
}


static int integration_turning_time_varying_reference()
{
	EngineGuard guard; TASK_CHECK( guard.ok );
	Fixture fx; fx.init();
	SceneHandles h = buildScene( fx.geo, fx.material );
	auto keys = defaultKeys();
	RootBlendPlayer player;
	TASK_CHECK( player.initialize( fx.baseConfig(
		(ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model ), keys ) ) );
	RefPlayer ref = fx.baseReference( keys, 0.4, 1.25, -0.75 );

	// Non-trivial start pose.
	Matrix4f start = Matrix4f::RotMat( 0.0f, 0.4f, 0.0f );
	start.c[3][0] = 1.25f; start.c[3][2] = -0.75f;
	TASK_CHECK( player.unbind() );
	TASK_CHECK( player.initialize( fx.baseConfig(
		(ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model ), keys, start ) ) );

	for( double t : { 0.3, 1.0, 1.0001, 2.0, 7.5, 15.0, 30.0 } )
	{
		TASK_CHECK( player.evaluate( t ) );
		Matrix4f wr; player.getWorldRoot( wr );
		RefPlayer::Pose p = ref.integrate( t );
		TASK_CHECK( nearVal( wr.x[12], p.x, 2e-5 ) );
		TASK_CHECK( nearVal( wr.x[14], p.z, 2e-5 ) );
		TASK_CHECK( nearVal( atan2( (double)wr.x[8], (double)wr.x[10] ),
		                     wrapPi( p.theta ), 2e-5 ) );
	}
	return 0;
}


static int integration_split_queries_match()
{
	EngineGuard guard; TASK_CHECK( guard.ok );
	Fixture fx; fx.init();
	SceneHandles h = buildScene( fx.geo, fx.material );
	auto keys = defaultKeys();
	RootBlendPlayer player;
	TASK_CHECK( player.initialize( fx.baseConfig(
		(ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model ), keys ) ) );

	const double T = 6.5;
	TASK_CHECK( player.evaluate( T ) );
	Matrix4f whole; player.getWorldRoot( whole );

	// Arbitrary split, including a zero-length pause and a backward jump.
	Matrix4f acc;
	for( double t : { 1.25, 1.25, 0.4, 3.3, 3.300001, T } )
	{
		TASK_CHECK( player.evaluate( t ) );
		player.getWorldRoot( acc );
	}
	for( int i : { 0, 5, 10, 12, 14 } )
		TASK_CHECK( nearVal( whole.x[i], acc.x[i], 2e-6 ) );

	// A fresh player on an independent model, queried only at T, gives the identical world root.
	SceneHandles h2 = buildScene( fx.geo, fx.material );
	RootBlendPlayer player2;
	TASK_CHECK( player2.initialize( fx.baseConfig(
		(ModelNode *)Modules::sceneMan().resolveNodeHandle( h2.model ), keys ) ) );
	TASK_CHECK( player2.evaluate( T ) );
	Matrix4f fresh; player2.getWorldRoot( fresh );
	for( int i : { 0, 5, 10, 12, 14 } )
		TASK_CHECK( nearVal( whole.x[i], fresh.x[i], 2e-6 ) );
	return 0;
}


static int integration_weight_clamping()
{
	EngineGuard guard; TASK_CHECK( guard.ok );
	Fixture fx; fx.init();
	SceneHandles h = buildScene( fx.geo, fx.material );
	auto keys = std::vector< RootBlendWeightKey > { { 1.0, 0.2f }, { 2.0, 0.8f } };
	RootBlendPlayer player;
	TASK_CHECK( player.initialize( fx.baseConfig(
		(ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model ), keys ) ) );
	RefPlayer ref = fx.baseReference( keys );

	// Before the first key the weight is clamped to 0.2; after the last key to 0.8.
	for( double t : { 0.0, 0.5, 2.5, 9.0 } )
	{
		TASK_CHECK( player.evaluate( t ) );
		Matrix4f wr; player.getWorldRoot( wr );
		RefPlayer::Pose p = ref.integrate( t );
		TASK_CHECK( nearVal( wr.x[12], p.x, 2e-5 ) );
		TASK_CHECK( nearVal( wr.x[14], p.z, 2e-5 ) );
	}
	return 0;
}


// =================================================================================================
// pose group: joints, skinning, morph, parents
// =================================================================================================

static int pose_root_replaces_animated_motion()
{
	EngineGuard guard; TASK_CHECK( guard.ok );
	Fixture fx; fx.init();
	SceneHandles h = buildScene( fx.geo, fx.material );
	auto keys = defaultKeys();
	RootBlendPlayer player;
	TASK_CHECK( player.initialize( fx.baseConfig(
		(ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model ), keys ) ) );
	RefPlayer ref = fx.baseReference( keys );

	TASK_CHECK( player.evaluate( 1.7 ) );
	RefPlayer::Pose p = ref.integrate( 1.7 );

	const float *absRoot = nullptr;
	h3dGetNodeTransMats( h.root, nullptr, &absRoot );
	TASK_CHECK( absRoot != nullptr );
	TASK_CHECK( nearVal( absRoot[12], p.x, 2e-5 ) );
	TASK_CHECK( nearVal( absRoot[14], p.z, 2e-5 ) );
	TASK_CHECK( nearVal( absRoot[13], 0.0, 2e-6 ) );
	TASK_CHECK( nearVal( atan2( (double)absRoot[8], (double)absRoot[10] ),
	                     wrapPi( p.theta ), 2e-5 ) );

	// The root's local matrix is inverse(model abs) * world root, not the animated TRS.
	ModelNode *model = (ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model );
	RMat4 expLocal = rigidInverse( RMat4() ) * RMat4::rotY( p.theta );
	expLocal.m[12] = p.x; expLocal.m[14] = p.z;
	const float *relRoot = nullptr;
	h3dGetNodeTransMats( h.root, &relRoot, nullptr );
	TASK_CHECK( nearMat( model->getChildren()[0]->getRelTrans(), expLocal, 2e-5 ) );
	TASK_CHECK( relRoot != nullptr );
	return 0;
}


static int pose_transformed_parent_and_vertices()
{
	EngineGuard guard; TASK_CHECK( guard.ok );
	Fixture fx; fx.init();
	SceneHandles h = buildScene( fx.geo, fx.material, 3.0f, 0.0f, -2.0f, 35.0f );
	auto keys = defaultKeys();
	RootBlendPlayer player;
	TASK_CHECK( player.initialize( fx.baseConfig(
		(ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model ), keys ) ) );
	RefPlayer ref = fx.baseReference( keys );
	h3dSetModelMorpher( h.model, "lift", 0.25f );

	// Absolute model matrix from the scene description (parent rotation 35 deg, translation).
	RMat4 absModel = RMat4::trans( 3.0, 0.0, -2.0 ) * RMat4::rotY( 35.0 * 3.1415926535897932 / 180.0 );

	ModelNode *model = (ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model );
	for( double t : { 0.6, 1.4, 3.2 } )
	{
		TASK_CHECK( player.evaluate( t ) );
		for( int v = 0; v < 3; ++v )
		{
			RVec3 expected = refWorldVertex( fx, ref, t, absModel, v, 0.25 );
			Vec3f actualModel = model->getGeometryResource()->getVertPosData()[v];
			RVec3 actualWorld = absModel * RVec3 { actualModel.x, actualModel.y, actualModel.z };
			TASK_CHECK( nearVal( actualWorld.x, expected.x, 2e-5 ) );
			TASK_CHECK( nearVal( actualWorld.y, expected.y, 2e-5 ) );
			TASK_CHECK( nearVal( actualWorld.z, expected.z, 2e-5 ) );
		}
	}
	return 0;
}


static int pose_nonuniform_scale_sibling()
{
	EngineGuard guard; TASK_CHECK( guard.ok );
	Fixture fx; fx.init();
	SceneHandles h = buildScene( fx.geo, fx.material, 1.0f, 0.0f, 1.0f, 20.0f );
	// A sibling branch of the model carries a non-uniform scale.
	H3DNode sibling = h3dAddGroupNode( h.parent, "sibling" );
	h3dSetNodeTransform( sibling, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f, 3.0f, 0.5f );
	auto keys = defaultKeys();
	RootBlendPlayer player;
	TASK_CHECK( player.initialize( fx.baseConfig(
		(ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model ), keys ) ) );
	RefPlayer ref = fx.baseReference( keys );

	TASK_CHECK( player.evaluate( 2.2 ) );
	RefPlayer::Pose p = ref.integrate( 2.2 );
	const float *absRoot = nullptr;
	h3dGetNodeTransMats( h.root, nullptr, &absRoot );
	TASK_CHECK( nearVal( absRoot[12], p.x, 2e-5 ) );
	TASK_CHECK( nearVal( absRoot[14], p.z, 2e-5 ) );
	return 0;
}


static int pose_inverse_bind_and_skinning()
{
	EngineGuard guard; TASK_CHECK( guard.ok );
	Fixture fx; fx.init();
	SceneHandles h = buildScene( fx.geo, fx.material );
	auto keys = defaultKeys();
	RootBlendPlayer player;
	TASK_CHECK( player.initialize( fx.baseConfig(
		(ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model ), keys ) ) );
	RefPlayer ref = fx.baseReference( keys );
	ModelNode *model = (ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model );

	const double t = 2.6;
	TASK_CHECK( player.evaluate( t ) );
	RMat4 identity;
	for( int v = 0; v < 3; ++v )
	{
		RVec3 expected = refWorldVertex( fx, ref, t, identity, v, 0.0 );
		Vec3f actual = model->getGeometryResource()->getVertPosData()[v];
		TASK_CHECK( nearVal( actual.x, expected.x, 2e-5 ) );
		TASK_CHECK( nearVal( actual.y, expected.y, 2e-5 ) );
		TASK_CHECK( nearVal( actual.z, expected.z, 2e-5 ) );
	}

	// Inverse-bind palette rows were written through JointNode::onPostUpdate.
	TASK_CHECK( model->jointExists( 0 ) && model->jointExists( 1 ) );
	return 0;
}


static int pose_morph_only()
{
	EngineGuard guard; TASK_CHECK( guard.ok );
	Fixture fx; fx.init();
	SceneHandles h = buildScene( fx.geo, fx.material );
	auto keys = defaultKeys();
	RootBlendPlayer player;
	TASK_CHECK( player.initialize( fx.baseConfig(
		(ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model ), keys ) ) );
	h3dSetModelMorpher( h.model, "lift", 0.5f );
	TASK_CHECK( player.evaluate( 0.0 ) );

	ModelNode *model = (ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model );
	RefPlayer ref = fx.baseReference( keys );
	RMat4 identity;
	// Vertex 0 carries the morph diff (0,1,0) * 0.5 on top of skinning.
	RVec3 expected = refWorldVertex( fx, ref, 0.0, identity, 0, 0.5 );
	Vec3f actual = model->getGeometryResource()->getVertPosData()[0];
	TASK_CHECK( nearVal( actual.x, expected.x, 2e-5 ) );
	TASK_CHECK( nearVal( actual.y, expected.y, 2e-5 ) );
	TASK_CHECK( nearVal( actual.z, expected.z, 2e-5 ) );
	return 0;
}


// =================================================================================================
// lifecycle group
// =================================================================================================

static int lifecycle_unbind_restores_stages()
{
	EngineGuard guard; TASK_CHECK( guard.ok );
	Fixture fx; fx.init();
	SceneHandles h = buildScene( fx.geo, fx.material );
	auto keys = defaultKeys();
	RootBlendPlayer player;
	ModelNode *model = (ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model );
	TASK_CHECK( model->getParamI( ModelNodeParams::AnimCountI ) == 0 );
	TASK_CHECK( player.initialize( fx.baseConfig( model, keys ) ) );
	TASK_CHECK( model->getParamI( ModelNodeParams::AnimCountI ) == 2 );
	TASK_CHECK( player.evaluate( 0.5 ) );
	TASK_CHECK( player.unbind() );
	TASK_CHECK( !player.isBound() );
	TASK_CHECK( model->getParamI( ModelNodeParams::AnimCountI ) == 0 );

	// Regular stage playback works again after unbind.
	model->setupAnimStage( 0, (AnimationResource *)Modules::resMan().resolveResHandle( fx.animA ),
	                       0, "", false );
	model->setAnimParams( 0, 1.0f, 1.0f );
	model->update( ModelUpdateFlags::Animation | ModelUpdateFlags::Geometry );
	const float *relLimb = nullptr;
	h3dGetNodeTransMats( h.limb, &relLimb, nullptr );
	TASK_CHECK( relLimb != nullptr && nearVal( relLimb[13], 1.1, 3e-4 ) );

	// Rebind works.
	RootBlendPlayer again;
	TASK_CHECK( again.initialize( fx.baseConfig( model, keys ) ) );
	TASK_CHECK( again.evaluate( 0.2 ) );
	return 0;
}


static int lifecycle_two_models_isolated()
{
	EngineGuard guard; TASK_CHECK( guard.ok );
	Fixture fx; fx.init();
	SceneHandles h1 = buildScene( fx.geo, fx.material, 0.0f, 0.0f, 0.0f );
	SceneHandles h2 = buildScene( fx.geo, fx.material, 5.0f, 0.0f, 5.0f, 90.0f );
	auto keys = defaultKeys();
	RootBlendPlayer p1, p2;
	ModelNode *m1 = (ModelNode *)Modules::sceneMan().resolveNodeHandle( h1.model );
	ModelNode *m2 = (ModelNode *)Modules::sceneMan().resolveNodeHandle( h2.model );
	TASK_CHECK( p1.initialize( fx.baseConfig( m1, keys ) ) );
	TASK_CHECK( p2.initialize( fx.baseConfig( m2, keys ) ) );

	RefPlayer ref = fx.baseReference( keys );
	double times1[] = { 0.3, 2.1, 4.0 };
	double times2[] = { 1.1, 0.2, 4.0 };
	for( int i = 0; i < 3; ++i )
	{
		TASK_CHECK( p1.evaluate( times1[i] ) );
		TASK_CHECK( p2.evaluate( times2[i] ) );
		RefPlayer::Pose q1 = ref.integrate( times1[i] );
		RefPlayer::Pose q2 = ref.integrate( times2[i] );
		Matrix4f w1, w2;
		p1.getWorldRoot( w1 ); p2.getWorldRoot( w2 );
		TASK_CHECK( nearVal( w1.x[12], q1.x, 2e-5 ) && nearVal( w1.x[14], q1.z, 2e-5 ) );
		TASK_CHECK( nearVal( w2.x[12], q2.x, 2e-5 ) && nearVal( w2.x[14], q2.z, 2e-5 ) );
		// World roots are independent of the parent placement (both start at the world origin).
		TASK_CHECK( nearVal( w1.x[12], w2.x[12], 1.0 ) || times1[i] != times2[i] );
	}
	// Interleaved re-query leaves each player's own answer intact.
	TASK_CHECK( p1.evaluate( 4.0 ) );
	Matrix4f w1; p1.getWorldRoot( w1 );
	RefPlayer::Pose q1 = ref.integrate( 4.0 );
	TASK_CHECK( nearVal( w1.x[12], q1.x, 2e-5 ) );
	return 0;
}


static int lifecycle_shared_anim_not_modified()
{
	EngineGuard guard; TASK_CHECK( guard.ok );
	Fixture fx; fx.init();
	SceneHandles h = buildScene( fx.geo, fx.material );

	AnimationResource *a = (AnimationResource *)Modules::resMan().resolveResHandle( fx.animA );
	AnimResEntity *rootEnt = a->findEntity( AnimationController::hashName( "root" ) );
	TASK_CHECK( rootEnt != 0x0 );
	std::vector<float> snapshot;
	for( const Frame &f : rootEnt->frames )
	{
		snapshot.push_back( f.rotQuat.x ); snapshot.push_back( f.rotQuat.y );
		snapshot.push_back( f.rotQuat.z ); snapshot.push_back( f.rotQuat.w );
		snapshot.push_back( f.transVec.x ); snapshot.push_back( f.transVec.y );
		snapshot.push_back( f.transVec.z );
	}

	auto keys = defaultKeys();
	RootBlendPlayer player;
	TASK_CHECK( player.initialize( fx.baseConfig(
		(ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model ), keys ) ) );
	for( double t : { 0.0, 1.0, 2.0, 3.0 } ) TASK_CHECK( player.evaluate( t ) );
	TASK_CHECK( player.unbind() );

	size_t k = 0;
	for( const Frame &f : rootEnt->frames )
	{
		TASK_CHECK( f.rotQuat.x == snapshot[k++] && f.rotQuat.y == snapshot[k++] );
		TASK_CHECK( f.rotQuat.z == snapshot[k++] && f.rotQuat.w == snapshot[k++] );
		TASK_CHECK( f.transVec.x == snapshot[k++] && f.transVec.y == snapshot[k++] );
		TASK_CHECK( f.transVec.z == snapshot[k++] );
	}
	return 0;
}


static int lifecycle_world_root_getter_and_pause()
{
	EngineGuard guard; TASK_CHECK( guard.ok );
	Fixture fx; fx.init();
	SceneHandles h = buildScene( fx.geo, fx.material );
	auto keys = defaultKeys();
	RootBlendPlayer player;
	Matrix4f before;
	TASK_CHECK( !player.getWorldRoot( before ) );  // unbound getter fails

	Matrix4f start = Matrix4f::RotMat( 0.0f, 0.7f, 0.0f );
	start.c[3][0] = 2.0f; start.c[3][1] = 1.5f; start.c[3][2] = -3.0f;
	TASK_CHECK( player.initialize( fx.baseConfig(
		(ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model ), keys, start ) ) );
	Matrix4f initial;
	TASK_CHECK( player.getWorldRoot( initial ) );
	TASK_CHECK( nearMat( initial, RMat4( [&] {
		RMat4 r = RMat4::rotY( 0.7 ); r.m[12] = 2.0; r.m[13] = 1.5; r.m[14] = -3.0; return r;
	}() ), 2e-6 ) );

	// Paused queries: same time twice yields bit-identical data and preserves the Y coordinate.
	TASK_CHECK( player.evaluate( 1.0 ) );
	Matrix4f a; player.getWorldRoot( a );
	TASK_CHECK( player.evaluate( 1.0 ) );
	Matrix4f b; player.getWorldRoot( b );
	for( int i = 0; i < 16; ++i ) TASK_CHECK( a.x[i] == b.x[i] );
	TASK_CHECK( nearVal( a.x[13], 1.5, 2e-7 ) );
	return 0;
}


// =================================================================================================
// invalid group: failures leave everything untouched and remain recoverable
// =================================================================================================

static int invalid_bad_arguments()
{
	EngineGuard guard; TASK_CHECK( guard.ok );
	Fixture fx; fx.init();
	SceneHandles h = buildScene( fx.geo, fx.material );
	ModelNode *model = (ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model );
	auto good = defaultKeys();
	RootBlendConfig cfg = fx.baseConfig( model, good );
	RootBlendPlayer player;

	// Null / wrong-type resources.
	RootBlendConfig bad = cfg; bad.anim[0] = 0x0;
	TASK_CHECK( !player.initialize( bad ) );
	bad = cfg; bad.anim[1] = (AnimationResource *)Modules::resMan().resolveResHandle( fx.geo );
	TASK_CHECK( !player.initialize( bad ) );

	// Non-finite scalars.
	bad = cfg; bad.fps[0] = 0.0f; TASK_CHECK( !player.initialize( bad ) );
	bad = cfg; bad.fps[1] = -1.0f; TASK_CHECK( !player.initialize( bad ) );
	bad = cfg; bad.rate[0] = NAN; TASK_CHECK( !player.initialize( bad ) );
	bad = cfg; bad.phase[1] = INFINITY; TASK_CHECK( !player.initialize( bad ) );

	// Bad weight keys.
	std::vector< RootBlendWeightKey > unsorted { { 1.0, 0.5f }, { 1.0, 0.6f } };
	bad = cfg; bad.keys = unsorted.data(); bad.numKeys = unsorted.size();
	TASK_CHECK( !player.initialize( bad ) );
	std::vector< RootBlendWeightKey > outOfRange { { 0.0, 1.2f } };
	bad = cfg; bad.keys = outOfRange.data(); bad.numKeys = outOfRange.size();
	TASK_CHECK( !player.initialize( bad ) );
	bad = cfg; bad.keys = nullptr; bad.numKeys = 0;
	TASK_CHECK( !player.initialize( bad ) );

	// Non-rigid start pose (non-uniform scale).
	bad = cfg;
	bad.startRoot = Matrix4f::ScaleMat( 1.0f, 2.0f, 1.0f );
	TASK_CHECK( !player.initialize( bad ) );

	// Rigid but not planar: a tilt about X cannot be represented by the planar world root.
	bad = cfg;
	bad.startRoot = Matrix4f::RotMat( 0.3f, 0.0f, 0.0f );
	TASK_CHECK( !player.initialize( bad ) );

	// Missing root joint.
	bad = cfg; bad.rootName = "ghost";
	TASK_CHECK( !player.initialize( bad ) );

	// No stage was set up by any failed attempt.
	TASK_CHECK( model->getParamI( ModelNodeParams::AnimCountI ) == 0 );

	// The valid configuration still initializes.
	TASK_CHECK( player.initialize( cfg ) );
	TASK_CHECK( player.evaluate( 0.5 ) );
	return 0;
}


static int invalid_bad_tracks()
{
	EngineGuard guard; TASK_CHECK( guard.ok );
	Fixture fx; fx.init();
	SceneHandles h = buildScene( fx.geo, fx.material );
	ModelNode *model = (ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model );
	auto keys = defaultKeys();

	// Helper: synthesize a 3-frame animation whose only entity is the root joint.
	auto makeAnim = []( const AnimFrame frames[3], const char *name ) -> H3DRes
	{
		AnimEntity e; e.name = "root"; e.compressed = false;
		e.frames = { frames[0], frames[1], frames[2] };
		Bytes b = buildAnim( 3, { e } );
		return loadResource( H3DResTypes::Animation, name, b );
	};

	AnimFrame good[3] =
	{
		{ yawQuat( 0.0 ), RVec3 { 0, 0, 0 } },
		{ yawQuat( 0.5 ), RVec3 { 1, 0, 0 } },
		{ yawQuat( 1.0 ), RVec3 { 2, 0, 0 } }
	};

	// Varying Y translation.
	AnimFrame badY[3] = { good[0], good[1], good[2] };
	badY[1].t.y = 0.5;
	H3DRes resBadY = makeAnim( badY, "badY.anim" );
	TASK_CHECK( resBadY != 0 );

	// Non-unit scale.
	// Encode scale directly into the frame writer through a dedicated root entity.
	{
		AnimEntity e; e.name = "root"; e.compressed = false;
		for( int i = 0; i < 3; ++i )
			e.frames.push_back( AnimFrame { good[i].q, good[i].t } );
		Bytes b;
		b.text( "H3DA", 4 ); b.u32( 3 ); b.u32( 1 ); b.u32( 3 );
		b.text( "root", 256 ); b.u8( 0 );
		for( int i = 0; i < 3; ++i )
		{
			double sc = i == 1 ? 2.0 : 1.0;
			b.frame( e.frames[i].q, e.frames[i].t.x, e.frames[i].t.y, e.frames[i].t.z, sc, 1, 1 );
		}
		H3DRes r = loadResource( H3DResTypes::Animation, "badScale.anim", b );
		TASK_CHECK( r != 0 );
		RootBlendConfig cfg = fx.baseConfig( model, keys );
		cfg.anim[0] = (AnimationResource *)Modules::resMan().resolveResHandle( r );
		RootBlendPlayer player;
		TASK_CHECK( !player.initialize( cfg ) );
	}

	// Rotation not about Y (quaternion about the X axis).
	{
		AnimEntity e; e.name = "root"; e.compressed = false;
		for( int i = 0; i < 3; ++i )
		{
			double a = 0.3 * i;
			RQuat q { sin( a * 0.5 ), 0.0, 0.0, cos( a * 0.5 ) };
			e.frames.push_back( AnimFrame { q, RVec3 { (double)i, 0, 0 } } );
		}
		Bytes b = buildAnim( 3, { e } );
		H3DRes r = loadResource( H3DResTypes::Animation, "badRot.anim", b );
		TASK_CHECK( r != 0 );
		RootBlendConfig cfg = fx.baseConfig( model, keys );
		cfg.anim[1] = (AnimationResource *)Modules::resMan().resolveResHandle( r );
		RootBlendPlayer player;
		TASK_CHECK( !player.initialize( cfg ) );
	}

	// Missing root entity (only an unrelated joint is animated).
	{
		AnimEntity e; e.name = "limb"; e.compressed = false;
		for( int i = 0; i < 3; ++i )
			e.frames.push_back( AnimFrame { yawQuat( 0.0 ), RVec3 { 0, 0, 0 } } );
		Bytes b = buildAnim( 3, { e } );
		H3DRes r = loadResource( H3DResTypes::Animation, "noRoot.anim", b );
		TASK_CHECK( r != 0 );
		RootBlendConfig cfg = fx.baseConfig( model, keys );
		cfg.anim[0] = (AnimationResource *)Modules::resMan().resolveResHandle( r );
		RootBlendPlayer player;
		TASK_CHECK( !player.initialize( cfg ) );
	}

	// The varying-Y resource is rejected as well.
	{
		RootBlendConfig cfg = fx.baseConfig( model, keys );
		cfg.anim[0] = (AnimationResource *)Modules::resMan().resolveResHandle( resBadY );
		RootBlendPlayer player;
		TASK_CHECK( !player.initialize( cfg ) );
		TASK_CHECK( model->getParamI( ModelNodeParams::AnimCountI ) == 0 );
	}

	// A legal configuration still initializes after all failed attempts.
	RootBlendPlayer player;
	TASK_CHECK( player.initialize( fx.baseConfig( model, keys ) ) );
	TASK_CHECK( player.evaluate( 0.5 ) );
	TASK_CHECK( player.unbind() );
	return 0;
}


static int invalid_query_range_and_recovery()
{
	EngineGuard guard; TASK_CHECK( guard.ok );
	Fixture fx; fx.init();
	SceneHandles h = buildScene( fx.geo, fx.material );
	ModelNode *model = (ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model );
	auto keys = defaultKeys();
	RootBlendPlayer player;
	TASK_CHECK( player.initialize( fx.baseConfig( model, keys ) ) );

	TASK_CHECK( !player.evaluate( -0.01 ) );
	TASK_CHECK( !player.evaluate( 30.01 ) );
	TASK_CHECK( !player.evaluate( NAN ) );
	TASK_CHECK( !player.evaluate( INFINITY ) );

	// World root stays at the start pose after failed queries.
	Matrix4f wr; TASK_CHECK( player.getWorldRoot( wr ) );
	TASK_CHECK( nearVal( wr.x[12], 0.0, 2e-7 ) && nearVal( wr.x[14], 0.0, 2e-7 ) );

	// Legal calls keep working, including backwards queries.
	TASK_CHECK( player.evaluate( 2.0 ) );
	TASK_CHECK( player.evaluate( 0.3 ) );
	RefPlayer ref = fx.baseReference( keys );
	RefPlayer::Pose p = ref.integrate( 0.3 );
	player.getWorldRoot( wr );
	TASK_CHECK( nearVal( wr.x[12], p.x, 2e-5 ) );

	// Unbound player rejects evaluation.
	TASK_CHECK( player.unbind() );
	TASK_CHECK( !player.evaluate( 1.0 ) );
	return 0;
}


static int invalid_nonrigid_parent_and_unchanged_model()
{
	EngineGuard guard; TASK_CHECK( guard.ok );
	Fixture fx; fx.init();
	SceneHandles h = buildScene( fx.geo, fx.material );
	ModelNode *model = (ModelNode *)Modules::sceneMan().resolveNodeHandle( h.model );
	auto keys = defaultKeys();

	// Snapshot the root joint before the failed attempt.
	Modules::sceneMan().updateNodes();
	Matrix4f snapshot = model->getChildren()[0]->getRelTrans();

	h3dSetNodeTransform( h.parent, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 2.0f );
	RootBlendPlayer player;
	TASK_CHECK( !player.initialize( fx.baseConfig( model, keys ) ) );
	TASK_CHECK( model->getParamI( ModelNodeParams::AnimCountI ) == 0 );
	Matrix4f after = model->getChildren()[0]->getRelTrans();
	for( int i = 0; i < 16; ++i ) TASK_CHECK( snapshot.x[i] == after.x[i] );

	// Restore rigidity: initialization now succeeds.
	h3dSetNodeTransform( h.parent, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f );
	TASK_CHECK( player.initialize( fx.baseConfig( model, keys ) ) );
	TASK_CHECK( player.evaluate( 1.0 ) );

	// Making the parent non-rigid while bound makes evaluate fail without changing the world root.
	Matrix4f goodWorld; player.getWorldRoot( goodWorld );
	h3dSetNodeTransform( h.parent, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f );
	TASK_CHECK( !player.evaluate( 2.0 ) );
	Matrix4f keptWorld; player.getWorldRoot( keptWorld );
	for( int i = 0; i < 16; ++i ) TASK_CHECK( goodWorld.x[i] == keptWorld.x[i] );
	return 0;
}


static int invalid_failure_then_legal_other_model()
{
	EngineGuard guard; TASK_CHECK( guard.ok );
	Fixture fx; fx.init();
	SceneHandles h1 = buildScene( fx.geo, fx.material );
	SceneHandles h2 = buildScene( fx.geo, fx.material, 4.0f, 0.0f, 4.0f );
	ModelNode *m1 = (ModelNode *)Modules::sceneMan().resolveNodeHandle( h1.model );
	ModelNode *m2 = (ModelNode *)Modules::sceneMan().resolveNodeHandle( h2.model );
	auto keys = defaultKeys();

	RootBlendPlayer p1;
	RootBlendConfig bad = fx.baseConfig( m1, keys );
	bad.fps[0] = 0.0f;
	TASK_CHECK( !p1.initialize( bad ) );

	RootBlendPlayer p2;
	TASK_CHECK( p2.initialize( fx.baseConfig( m2, keys ) ) );
	TASK_CHECK( p2.evaluate( 1.5 ) );
	RefPlayer ref = fx.baseReference( keys );
	RefPlayer::Pose p = ref.integrate( 1.5 );
	Matrix4f wr; p2.getWorldRoot( wr );
	TASK_CHECK( nearVal( wr.x[12], p.x, 2e-5 ) && nearVal( wr.x[14], p.z, 2e-5 ) );

	// m1 is untouched and can still be bound afterwards.
	TASK_CHECK( m1->getParamI( ModelNodeParams::AnimCountI ) == 0 );
	TASK_CHECK( p1.initialize( fx.baseConfig( m1, keys ) ) );
	TASK_CHECK( p1.evaluate( 0.7 ) );
	return 0;
}


// =================================================================================================
// Registration
// =================================================================================================

const task_case task_cases[] =
{
	{ "twist",       "negated_quaternions",             twist_negated_quaternions },
	{ "twist",       "different_fps_single_clip",       twist_different_fps_single_clip },
	{ "twist",       "nonzero_phase",                   twist_nonzero_phase },
	{ "twist",       "reverse_across_cycles",           twist_reverse_across_cycles },

	{ "integration", "straight_line_exact",             integration_straight_line_exact },
	{ "integration", "turning_time_varying_reference",  integration_turning_time_varying_reference },
	{ "integration", "split_queries_match",             integration_split_queries_match },
	{ "integration", "weight_clamping",                 integration_weight_clamping },

	{ "pose",        "root_replaces_animated_motion",   pose_root_replaces_animated_motion },
	{ "pose",        "transformed_parent_and_vertices", pose_transformed_parent_and_vertices },
	{ "pose",        "nonuniform_scale_sibling",        pose_nonuniform_scale_sibling },
	{ "pose",        "inverse_bind_and_skinning",       pose_inverse_bind_and_skinning },
	{ "pose",        "morph_only",                      pose_morph_only },

	{ "lifecycle",   "unbind_restores_stages",          lifecycle_unbind_restores_stages },
	{ "lifecycle",   "two_models_isolated",             lifecycle_two_models_isolated },
	{ "lifecycle",   "shared_anim_not_modified",        lifecycle_shared_anim_not_modified },
	{ "lifecycle",   "world_root_getter_and_pause",     lifecycle_world_root_getter_and_pause },

	{ "invalid",     "bad_arguments",                   invalid_bad_arguments },
	{ "invalid",     "bad_tracks",                      invalid_bad_tracks },
	{ "invalid",     "query_range_and_recovery",        invalid_query_range_and_recovery },
	{ "invalid",     "nonrigid_parent_unchanged_model", invalid_nonrigid_parent_and_unchanged_model },
	{ "invalid",     "failure_then_legal_other_model",  invalid_failure_then_legal_other_model },
};

const size_t task_case_count = sizeof( task_cases ) / sizeof( task_cases[0] );
