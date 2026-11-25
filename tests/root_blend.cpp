// *************************************************************************************************
//
// Horde3D
//   Task tests for the dual-action root-motion player (egRootBlend).
//
//   No main(): the cases are registered through task_support/check.h and driven by driver.c.
//   All H3DA / skinned geometry is generated procedurally and every numeric expectation is
//   computed by an independent double-precision scalar reference in this file.
//
// *************************************************************************************************

#include "task_support/check.h"
#include "Horde3D.h"
#include "egModules.h"
#include "egScene.h"
#include "egModel.h"
#include "egAnimation.h"
#include "egRootBlend.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

using namespace Horde3D;

namespace
{

// -----------------------------------------------------------------------------------------------
// Small scalar (double precision) reference math, independent of the engine's float classes
// -----------------------------------------------------------------------------------------------

struct DVec
{
	double x, y, z;
	DVec( double x_ = 0, double y_ = 0, double z_ = 0 ) : x( x_ ), y( y_ ), z( z_ ) {}
	DVec operator+( const DVec &o ) const { return DVec( x + o.x, y + o.y, z + o.z ); }
};

struct DMat
{
	double m[16];  // Column major, identical layout to Matrix4f
	DMat()
	{
		for( int i = 0; i < 16; ++i ) m[i] = ( i % 5 == 0 ) ? 1.0 : 0.0;
	}
};

DMat operator*( const DMat &a, const DMat &b )
{
	DMat r;
	for( int c = 0; c < 4; ++c )
		for( int row = 0; row < 4; ++row )
		{
			double s = 0;
			for( int k = 0; k < 4; ++k ) s += a.m[k * 4 + row] * b.m[c * 4 + k];
			r.m[c * 4 + row] = s;
		}
	return r;
}

DVec mulPoint( const DMat &a, const DVec &v )
{
	return DVec(
		a.m[0] * v.x + a.m[4] * v.y + a.m[8]  * v.z + a.m[12],
		a.m[1] * v.x + a.m[5] * v.y + a.m[9]  * v.z + a.m[13],
		a.m[2] * v.x + a.m[6] * v.y + a.m[10] * v.z + a.m[14] );
}

DMat yawMat( double yaw )
{
	DMat r;
	const double c = cos( yaw ), s = sin( yaw );
	r.m[0] = c;  r.m[2] = s;
	r.m[8] = -s; r.m[10] = c;
	return r;
}

DMat transMat( double x, double y, double z )
{
	DMat r;
	r.m[12] = x; r.m[13] = y; r.m[14] = z;
	return r;
}

DMat rigidInverse( const DMat &a )
{
	DMat r;
	for( int c = 0; c < 3; ++c )
		for( int row = 0; row < 3; ++row )
			r.m[c * 4 + row] = a.m[row * 4 + c];  // Transpose rotation
	const DVec t( a.m[12], a.m[13], a.m[14] );
	r.m[12] = -( r.m[0] * t.x + r.m[4] * t.y + r.m[8]  * t.z );
	r.m[13] = -( r.m[1] * t.x + r.m[5] * t.y + r.m[9]  * t.z );
	r.m[14] = -( r.m[2] * t.x + r.m[6] * t.y + r.m[10] * t.z );
	return r;
}

void yawQuat( double yaw, double &qy, double &qw )
{
	// Match the H3DA builder, which stores float32 components from single-precision trig.
	qy = (double)(float)sinf( (float)( yaw * 0.5 ) );
	qw = (double)(float)cosf( (float)( yaw * 0.5 ) );
}

// Shortest-arc normalized linear quaternion interpolation for Y-only quaternions; returns yaw.
double nlerpYaw( double y0, double w0, double y1, double w1, double a )
{
	if( y0 * y1 + w0 * w1 < 0.0 ) { y1 = -y1; w1 = -w1; }
	double y = y0 + ( y1 - y0 ) * a;
	double w = w0 + ( w1 - w0 ) * a;
	const double l = sqrt( y * y + w * w );
	y /= l; w /= l;
	// Canonicalize the double cover so the returned angle is the actual small Y rotation.
	if( w < 0.0 ) { w = -w; y = -y; }
	return 2.0 * atan2( y, w );
}

bool near( double a, double b, double tol = 2e-5 )
{
	return std::isfinite( a ) && std::isfinite( b ) && fabs( a - b ) <= tol;
}

bool nearV( const DVec &a, const Vec3f &b, double tol = 2e-5 )
{
	return near( a.x, b.x, tol ) && near( a.y, b.y, tol ) && near( a.z, b.z, tol );
}

// The cases have no main(), so bring the headless engine up and tear it down with the TU.
struct EngineLifetime
{
	EngineLifetime()
	{
		h3dInit( static_cast<H3DRenderDevice::List>( 256 ) );
		h3dSetOption( H3DOptions::FastAnimation, 0 );
	}
	~EngineLifetime() { h3dRelease(); }
};
static EngineLifetime g_engineLifetime;

// -----------------------------------------------------------------------------------------------
// Procedural resource builders
// -----------------------------------------------------------------------------------------------

struct Bytes
{
	std::vector<char> d;
	void u32( uint32_t v ) { for( int i = 0; i < 4; ++i ) d.push_back( char( v >> ( 8 * i ) ) ); }
	void u16( int v ) { d.push_back( char( v ) ); d.push_back( char( v >> 8 ) ); }
	void u8( int v ) { d.push_back( char( v ) ); }
	void f32( float v ) { uint32_t b; std::memcpy( &b, &v, 4 ); u32( b ); }
	void text( const char *s, size_t n )
	{
		size_t len = std::strlen( s );
		for( size_t i = 0; i < n; ++i ) d.push_back( i < len ? s[i] : 0 );
	}
};

struct JointFrame
{
	float qx, qy, qz, qw, tx, ty, tz, sx, sy, sz;
};

struct AnimJoint
{
	std::string name;
	bool compressed;
	std::vector< JointFrame > frames;
};

Bytes makeAnim( uint32_t numFrames, const std::vector< AnimJoint > &joints )
{
	Bytes b;
	b.text( "H3DA", 4 );
	b.u32( 3 );
	b.u32( (uint32_t)joints.size() );
	b.u32( numFrames );
	for( const AnimJoint &j : joints )
	{
		b.text( j.name.c_str(), 256 );
		b.u8( j.compressed ? 1 : 0 );
		const size_t n = j.compressed ? 1 : j.frames.size();
		for( size_t k = 0; k < n; ++k )
		{
			const JointFrame &f = j.frames[k];
			b.f32( f.qx ); b.f32( f.qy ); b.f32( f.qz ); b.f32( f.qw );
			b.f32( f.tx ); b.f32( f.ty ); b.f32( f.tz );
			b.f32( f.sx ); b.f32( f.sy ); b.f32( f.sz );
		}
	}
	return b;
}

JointFrame rootFrame( float yaw, float x, float z, bool negate = false )
{
	JointFrame f;
	float qy = sinf( yaw * 0.5f ), qw = cosf( yaw * 0.5f );
	if( negate ) { qy = -qy; qw = -qw; }
	f.qx = 0; f.qy = qy; f.qz = 0; f.qw = qw;
	f.tx = x; f.ty = 0; f.tz = z;
	f.sx = f.sy = f.sz = 1;
	return f;
}

JointFrame trFrame( float x, float y, float z )
{
	JointFrame f;
	f.qx = f.qy = f.qz = 0; f.qw = 1;
	f.tx = x; f.ty = y; f.tz = z;
	f.sx = f.sy = f.sz = 1;
	return f;
}

// Geometry: two joints, three vertices (v0,v1 -> joint0, v2 -> joint1), optional morph on v0.
Bytes makeGeometry( const float invBind0[16], const float invBind1[16], bool withMorph )
{
	const float xyz[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
	Bytes b;
	b.text( "H3DG", 4 );
	b.u32( 5 );
	b.u32( 2 );  // Two joints
	for( int i = 0; i < 16; ++i ) b.f32( invBind0[i] );
	for( int i = 0; i < 16; ++i ) b.f32( invBind1[i] );

	b.u32( 6 );  // Streams
	b.u32( 3 );  // Vertex count
	b.u32( 0 ); b.u32( 12 );
	for( auto &p : xyz ) for( float v : p ) b.f32( v );
	for( int stream = 1; stream <= 3; ++stream )
	{
		b.u32( stream ); b.u32( 6 );
		for( int i = 0; i < 3; ++i )
			for( int c = 0; c < 3; ++c ) b.u16( c == stream - 1 ? 32767 : 0 );
	}
	b.u32( 4 ); b.u32( 4 );  // Joint indices
	for( int i = 0; i < 3; ++i ) { b.u8( i == 2 ? 1 : 0 ); b.u8( 0 ); b.u8( 0 ); b.u8( 0 ); }
	b.u32( 5 ); b.u32( 4 );  // Weights
	for( int i = 0; i < 3; ++i ) { b.u8( 255 ); b.u8( 0 ); b.u8( 0 ); b.u8( 0 ); }

	b.u32( 3 ); b.u32( 0 ); b.u32( 1 ); b.u32( 2 );  // One triangle

	b.u32( withMorph ? 1u : 0u );
	if( withMorph )
	{
		b.text( "lift", 256 );
		b.u32( 1 );            // One affected vertex
		b.u32( 0 );            // Vertex index 0
		b.u32( 1 );            // One morph stream (position)
		b.u32( 0 ); b.u32( 12 );
		b.f32( 0.25f ); b.f32( 0.0f ); b.f32( -0.5f );
	}
	return b;
}

H3DRes loadRes( int type, const char *name, const Bytes &b )
{
	H3DRes r = h3dAddResource( type, name, 0 );
	return r && h3dLoadResource( r, b.d.data(), (int)b.d.size() ) ? r : 0;
}

// -----------------------------------------------------------------------------------------------
// Independent reference root-motion simulator
// -----------------------------------------------------------------------------------------------

struct RefAction
{
	std::vector<double> yaw, px, pz;  // Length N, planar root track
	double fps, phase, rate;
};

struct RefKeys
{
	std::vector<double> t, w;
};

struct RefSim
{
	const RefAction *a0, *a1;
	RefKeys keys;

	double weight( double t ) const
	{
		if( t <= keys.t.front() ) return keys.w.front();
		if( t >= keys.t.back() ) return keys.w.back();
		for( size_t i = 1; i < keys.t.size(); ++i )
			if( t <= keys.t[i] )
			{
				const double s = ( t - keys.t[i - 1] ) / ( keys.t[i] - keys.t[i - 1] );
				return keys.w[i - 1] + ( keys.w[i] - keys.w[i - 1] ) * s;
			}
		return keys.w.back();
	}

	// Wrapped frame coordinate, segment and interpolation of one action at time t. Outputs the
	// body-frame planar velocity and the yaw rate taken from the actual interpolation curves.
	void sample( const RefAction &a, double t, double &yaw, double &x, double &z,
	             double &bodyVx, double &bodyVz, double &yawRate ) const
	{
		const int N = (int)a.yaw.size();
		const double span = N - 1;  // Period spans frames 0..N-1, no extra closing segment
		double ff = fmod( ( a.phase + a.rate * t ) * a.fps, span );
		if( ff < 0 ) ff += span;

		int f0 = (int)floor( ff );
		if( f0 >= N - 1 ) f0 = N - 2;
		const double al = ff - f0;
		x = a.px[f0] + ( a.px[f0 + 1] - a.px[f0] ) * al;
		z = a.pz[f0] + ( a.pz[f0 + 1] - a.pz[f0] ) * al;
		double y0, w0, y1, w1;
		yawQuat( a.yaw[f0], y0, w0 );
		yawQuat( a.yaw[f0 + 1], y1, w1 );
		yaw = nlerpYaw( y0, w0, y1, w1, al );

		const double rfps = a.rate * a.fps;
		const double ddx = ( a.px[f0 + 1] - a.px[f0] ) * rfps;
		const double ddz = ( a.pz[f0 + 1] - a.pz[f0] ) * rfps;
		// dphi/dalpha of the nlerp curve, then dalpha/dt = rate*fps.
		if( y0 * y1 + w0 * w1 < 0.0 ) { y1 = -y1; w1 = -w1; }
		const double ly = y1 - y0, lw = w1 - w0;
		const double yt = y0 + ly * al, wt = w0 + lw * al;
		const double denom = yt * yt + wt * wt;
		const double dPhi = denom > 1e-15 ? 2.0 * ( wt * ly - yt * lw ) / denom : 0.0;
		yawRate = dPhi * rfps;
		// World velocity rotated into the body frame.
		const double c = cos( yaw ), s = sin( yaw );
		bodyVx = c * ddx - s * ddz;
		bodyVz = s * ddx + c * ddz;
	}

	void deriv( double t, const double st[3], double d[3] ) const
	{
		double y0, x0, z0, bx0, bz0, w0v;
		double y1, x1, z1, bx1, bz1, w1v;
		sample( *a0, t, y0, x0, z0, bx0, bz0, w0v );
		sample( *a1, t, y1, x1, z1, bx1, bz1, w1v );
		const double w = weight( t );
		const double bx = ( 1 - w ) * bx0 + w * bx1;
		const double bz = ( 1 - w ) * bz0 + w * bz1;
		const double om = ( 1 - w ) * w0v + w * w1v;
		const double th = st[2], c = cos( th ), s = sin( th );
		d[0] = c * bx - s * bz;
		d[1] = s * bx + c * bz;
		d[2] = om;
	}

	// Event-aligned fixed-substep RK4 from zero to T.
	void run( double T, double startX, double startZ, double startYaw,
	          double &outX, double &outZ, double &outYaw ) const
	{
		double st[3] = { startX, startZ, startYaw };
		double cur = 0.0;
		while( T - cur > 1e-13 )
		{
			double ev = T;
			for( double kt : keys.t ) if( kt > cur + 1e-12 && kt < ev ) ev = kt;
			const RefAction *acts[2] = { a0, a1 };
			for( int k = 0; k < 2; ++k )
			{
				const RefAction &a = *acts[k];
				if( a.rate == 0.0 ) continue;
				const double fNow = ( a.phase + a.rate * cur ) * a.fps;
				// Direction of the playhead depends on the sign of rate.
				const double j = a.rate > 0.0 ? floor( fNow + 1e-12 ) + 1.0
				                               : ceil( fNow - 1e-12 ) - 1.0;
				const double bt = ( j / a.fps - a.phase ) / a.rate;
				if( bt > cur + 1e-12 && bt < ev ) ev = bt;
			}
			const double segEnd = std::min( ev, T );
			// Event segments keep every RK sample inside a smooth region; a fine fixed step makes
			// the truncation error negligible and the reference independent of the engine.
			const int sub = std::max( 2, (int)ceil( ( segEnd - cur ) / 2e-6 ) );
			const double h = ( segEnd - cur ) / sub;
			for( int q = 0; q < sub; ++q )
			{
				double k1[3], k2[3], k3[3], k4[3], tmp[3];
				deriv( cur, st, k1 );
				for( int c = 0; c < 3; ++c ) tmp[c] = st[c] + h * 0.5 * k1[c];
				deriv( cur + h * 0.5, tmp, k2 );
				for( int c = 0; c < 3; ++c ) tmp[c] = st[c] + h * 0.5 * k2[c];
				deriv( cur + h * 0.5, tmp, k3 );
				for( int c = 0; c < 3; ++c ) tmp[c] = st[c] + h * k3[c];
				deriv( cur + h, tmp, k4 );
				for( int c = 0; c < 3; ++c )
					st[c] += h / 6.0 * ( k1[c] + 2 * k2[c] + 2 * k3[c] + k4[c] );
				cur += h;
			}
		}
		outX = st[0]; outZ = st[1]; outYaw = st[2];
	}
};

// Build a planar reference action from engine-style frame descriptions.
RefAction makeRef( const std::vector<double> &yaws, const std::vector<double> &xs,
                   const std::vector<double> &zs, double fps, double phase, double rate )
{
	// The engine's H3DA tracks are stored as float32, so round the reference data the same way;
	// this keeps the independent integration honest while sharing the exact source precision.
	auto asFloat = []( const std::vector<double> &v )
	{
		std::vector<double> r( v.size() );
		for( size_t i = 0; i < v.size(); ++i ) r[i] = (double)(float)v[i];
		return r;
	};
	RefAction a;
	a.yaw = asFloat( yaws ); a.px = asFloat( xs ); a.pz = asFloat( zs );
	a.fps = fps; a.phase = phase; a.rate = rate;
	return a;
}

// -----------------------------------------------------------------------------------------------
// Shared fixture world
// -----------------------------------------------------------------------------------------------

struct World
{
	H3DRes anim0, anim1, geo, mat;
	H3DNode parent, model, root, limb;
	ModelNode *modelNode;

	void init( const Bytes &a0, const Bytes &a1, const Bytes &g, bool swSkin = true )
	{
		// Unique resource/node names per fixture so cases never collide inside the shared engine.
		const unsigned id = s_worldCounter++;
		char name[64];
		std::snprintf( name, sizeof( name ), "a0_%u.anim", id );
		anim0 = loadRes( H3DResTypes::Animation, name, a0 );
		std::snprintf( name, sizeof( name ), "a1_%u.anim", id );
		anim1 = loadRes( H3DResTypes::Animation, name, a1 );
		std::snprintf( name, sizeof( name ), "g_%u.geo", id );
		geo = loadRes( H3DResTypes::Geometry, name, g );
		std::snprintf( name, sizeof( name ), "m_%u.material", id );
		mat = h3dAddResource( H3DResTypes::Material, name, 0 );
		std::snprintf( name, sizeof( name ), "placement_%u", id );
		parent = h3dAddGroupNode( H3DRootNode, name );
		model = h3dAddModelNode( parent, "character", geo );
		h3dSetNodeParamI( model, H3DModel::SWSkinningI, swSkin ? 1 : 0 );
		root = h3dAddJointNode( model, "root", 0 );
		limb = h3dAddJointNode( root, "limb", 1 );
		h3dAddMeshNode( model, "tri", mat, H3DMeshPrimType::TriangleList, 0, 3, 0, 2 );
		modelNode = (ModelNode *)Modules::sceneMan().resolveNodeHandle( model );
	}

private:
	static unsigned s_worldCounter;
};
unsigned World::s_worldCounter = 0;

void identityInvBind( float m[16] )
{
	for( int i = 0; i < 16; ++i ) m[i] = ( i % 5 == 0 ) ? 1.0f : 0.0f;
}

// Two planar tracks with linear forward motion and constant turning, different lengths.
void linearTurnTracks( std::vector< AnimJoint > &j0, std::vector< AnimJoint > &j1,
                       std::vector<double> &ry0, std::vector<double> &rx0, std::vector<double> &rz0,
                       std::vector<double> &ry1, std::vector<double> &rx1, std::vector<double> &rz1,
                       bool negate0 = false )
{
	const int N0 = 4, N1 = 5;
	AnimJoint root0, root1, limb0, limb1;
	root0.name = "root"; root1.name = "root";
	limb0.name = "limb"; limb1.name = "limb";
	for( int i = 0; i < N0; ++i )
	{
		const float yaw = 0.3f * i, x = 1.0f + 0.5f * i, z = -0.25f * i;
		root0.frames.push_back( rootFrame( yaw, x, z, negate0 ) );
		limb0.frames.push_back( trFrame( 0, 1.0f + 0.1f * i, 0 ) );
		ry0.push_back( yaw ); rx0.push_back( x ); rz0.push_back( z );
	}
	for( int i = 0; i < N1; ++i )
	{
		const float yaw = -0.2f * i, x = 2.0f * i, z = 1.0f + 0.4f * i;
		root1.frames.push_back( rootFrame( yaw, x, z ) );
		limb1.frames.push_back( trFrame( 0, 2.0f + 0.2f * i, 0 ) );
		ry1.push_back( yaw ); rx1.push_back( x ); rz1.push_back( z );
	}
	j0 = { root0, limb0 };
	j1 = { root1, limb1 };
}

std::vector< RootBlendKey > keys( std::initializer_list<std::pair<float,float>> kv )
{
	std::vector< RootBlendKey > r;
	for( auto p : kv ) r.push_back( RootBlendKey{ p.first, p.second } );
	return r;
}

Matrix4f startPose( float x, float y, float z, float yaw )
{
	Matrix4f m = Matrix4f( Quaternion( 0, yaw, 0 ) );
	m = Matrix4f::TransMat( x, y, z ) * m;
	return m;
}

int checkAgainstRef( RootBlendPlayer &p, const RefSim &ref, float t,
                     float startX, float startZ, float startYaw, double tol = 2e-5 )
{
	double rx, rz, ry;
	ref.run( t, startX, startZ, startYaw, rx, rz, ry );
	TASK_CHECK( p.evaluate( t ) );
	Matrix4f w;
	TASK_CHECK( p.getWorldRoot( w ) );
	TASK_CHECK( near( rx, w.x[12], tol ) );
	TASK_CHECK( near( rz, w.x[14], tol ) );
	const float gy = atan2f( w.c[2][0], w.c[0][0] );
	// Yaw is a heading on the circle; the reference keeps it unwrapped while the matrix stores it
	// in [-pi, pi], so compare the wrapped angular difference.
	const double diff = atan2( sin( ry - (double)gy ), cos( ry - (double)gy ) );
	TASK_CHECK( near( diff, 0.0, tol ) );
	return 0;
}

}  // namespace

// =================================================================================================
// twist: different fps, negated quaternions, non-zero phase, reverse wrap, turning under blend
// =================================================================================================

static int twist_neg_quat()
{
	std::vector<AnimJoint> j0, j1;
	std::vector<double> ry0, rx0, rz0, ry1, rx1, rz1;
	linearTurnTracks( j0, j1, ry0, rx0, rz0, ry1, rx1, rz1, /*negate0*/ true );
	const Bytes a0 = makeAnim( 4, j0 ), a1 = makeAnim( 5, j1 );
	float ib0[16], ib1[16]; identityInvBind( ib0 ); identityInvBind( ib1 );
	const Bytes g = makeGeometry( ib0, ib1, false );

	World w; w.init( a0, a1, g );
	RootBlendPlayer p;
	const auto ks = keys( { { 0.0f, 0.2f }, { 5.0f, 0.8f }, { 30.0f, 0.8f } } );
	TASK_CHECK( p.bind( (AnimationResource *)Modules::resMan().resolveResHandle( w.anim0 ),
	                    (AnimationResource *)Modules::resMan().resolveResHandle( w.anim1 ),
	                    10, 10, 0, 0, 1, 1, "root", w.modelNode, startPose( 0, 0, 0, 0 ), ks ) );

	RefAction ra0 = makeRef( ry0, rx0, rz0, 10, 0, 1 );
	RefAction ra1 = makeRef( ry1, rx1, rz1, 10, 0, 1 );
	RefSim ref{ &ra0, &ra1, { { 0, 5, 30 }, { 0.2, 0.8, 0.8 } } };
	TASK_CHECK( checkAgainstRef( p, ref, 2.5f, 0, 0, 0 ) == 0 );
	p.unbind();
	return 0;
}

static int twist_diff_fps_phase()
{
	std::vector<AnimJoint> j0, j1;
	std::vector<double> ry0, rx0, rz0, ry1, rx1, rz1;
	linearTurnTracks( j0, j1, ry0, rx0, rz0, ry1, rx1, rz1 );
	const Bytes a0 = makeAnim( 4, j0 ), a1 = makeAnim( 5, j1 );
	float ib0[16], ib1[16]; identityInvBind( ib0 ); identityInvBind( ib1 );
	const Bytes g = makeGeometry( ib0, ib1, false );

	World w; w.init( a0, a1, g );
	RootBlendPlayer p;
	const auto ks = keys( { { 0.0f, 0.0f }, { 10.0f, 1.0f } } );
	TASK_CHECK( p.bind( (AnimationResource *)Modules::resMan().resolveResHandle( w.anim0 ),
	                    (AnimationResource *)Modules::resMan().resolveResHandle( w.anim1 ),
	                    10, 15, 0.7f, -1.3f, 1, 1, "root", w.modelNode,
	                    startPose( 1, 2, -3, 0.4f ), ks ) );

	RefAction ra0 = makeRef( ry0, rx0, rz0, 10, 0.7, 1 );
	RefAction ra1 = makeRef( ry1, rx1, rz1, 15, -1.3, 1 );
	RefSim ref{ &ra0, &ra1, { { 0, 10 }, { 0, 1 } } };
	TASK_CHECK( checkAgainstRef( p, ref, 3.0f, 1, -3, 0.4 ) == 0 );
	TASK_CHECK( checkAgainstRef( p, ref, 7.5f, 1, -3, 0.4 ) == 0 );
	p.unbind();
	return 0;
}

static int twist_reverse_wrap()
{
	std::vector<AnimJoint> j0, j1;
	std::vector<double> ry0, rx0, rz0, ry1, rx1, rz1;
	linearTurnTracks( j0, j1, ry0, rx0, rz0, ry1, rx1, rz1 );
	const Bytes a0 = makeAnim( 4, j0 ), a1 = makeAnim( 5, j1 );
	float ib0[16], ib1[16]; identityInvBind( ib0 ); identityInvBind( ib1 );
	const Bytes g = makeGeometry( ib0, ib1, false );

	World w; w.init( a0, a1, g );
	RootBlendPlayer p;
	const auto ks = keys( { { 0.0f, 0.5f }, { 30.0f, 0.5f } } );
	TASK_CHECK( p.bind( (AnimationResource *)Modules::resMan().resolveResHandle( w.anim0 ),
	                    (AnimationResource *)Modules::resMan().resolveResHandle( w.anim1 ),
	                    12, 8, 0.2f, 0.9f, -1.2f, -0.7f, "root", w.modelNode,
	                    startPose( 0, 0, 0, 0 ), ks ) );

	RefAction ra0 = makeRef( ry0, rx0, rz0, 12, 0.2, -1.2 );
	RefAction ra1 = makeRef( ry1, rx1, rz1, 8, 0.9, -0.7 );
	RefSim ref{ &ra0, &ra1, { { 0, 30 }, { 0.5, 0.5 } } };
	// Long enough to cross several cycle boundaries backwards.
	TASK_CHECK( checkAgainstRef( p, ref, 6.0f, 0, 0, 0 ) == 0 );
	TASK_CHECK( checkAgainstRef( p, ref, 12.0f, 0, 0, 0 ) == 0 );
	p.unbind();
	return 0;
}

static int twist_turn_while_blending()
{
	std::vector<AnimJoint> j0, j1;
	std::vector<double> ry0, rx0, rz0, ry1, rx1, rz1;
	linearTurnTracks( j0, j1, ry0, rx0, rz0, ry1, rx1, rz1 );
	const Bytes a0 = makeAnim( 4, j0 ), a1 = makeAnim( 5, j1 );
	float ib0[16], ib1[16]; identityInvBind( ib0 ); identityInvBind( ib1 );
	const Bytes g = makeGeometry( ib0, ib1, false );

	World w; w.init( a0, a1, g );
	RootBlendPlayer p;
	const auto ks = keys( { { 0.0f, 0.0f }, { 2.0f, 0.3f }, { 6.0f, 0.9f }, { 30.0f, 0.9f } } );
	TASK_CHECK( p.bind( (AnimationResource *)Modules::resMan().resolveResHandle( w.anim0 ),
	                    (AnimationResource *)Modules::resMan().resolveResHandle( w.anim1 ),
	                    10, 10, 0, 0, 1, 1, "root", w.modelNode,
	                    startPose( 2, 0, 1, 0.1f ), ks ) );

	RefAction ra0 = makeRef( ry0, rx0, rz0, 10, 0, 1 );
	RefAction ra1 = makeRef( ry1, rx1, rz1, 10, 0, 1 );
	RefSim ref{ &ra0, &ra1, { { 0, 2, 6, 30 }, { 0, 0.3, 0.9, 0.9 } } };
	const float order[] = { 4.0f, 1.0f, 8.0f, 2.0f, 5.5f };  // Deliberately shuffled
	for( float t : order ) TASK_CHECK( checkAgainstRef( p, ref, t, 2, 1, 0.1 ) == 0 );
	p.unbind();
	return 0;
}

// =================================================================================================
// integration: split-query determinism and pause / zero rate
// =================================================================================================

static int integration_split_consistency()
{
	std::vector<AnimJoint> j0, j1;
	std::vector<double> ry0, rx0, rz0, ry1, rx1, rz1;
	linearTurnTracks( j0, j1, ry0, rx0, rz0, ry1, rx1, rz1 );
	const Bytes a0 = makeAnim( 4, j0 ), a1 = makeAnim( 5, j1 );
	float ib0[16], ib1[16]; identityInvBind( ib0 ); identityInvBind( ib1 );
	const Bytes g = makeGeometry( ib0, ib1, false );

	World w; w.init( a0, a1, g );
	RootBlendPlayer p;
	const auto ks = keys( { { 0.0f, 0.1f }, { 4.0f, 0.7f }, { 30.0f, 0.7f } } );
	TASK_CHECK( p.bind( (AnimationResource *)Modules::resMan().resolveResHandle( w.anim0 ),
	                    (AnimationResource *)Modules::resMan().resolveResHandle( w.anim1 ),
	                    10, 10, 0, 0, 1, 1, "root", w.modelNode, startPose( 0, 0, 0, 0 ), ks ) );

	Matrix4f full;
	TASK_CHECK( p.evaluate( 5.0f ) );
	TASK_CHECK( p.getWorldRoot( full ) );

	// Re-query through several arbitrary subdivisions; each row sums to 5 s. Because every
	// evaluation integrates independently from zero, the final t = 5 query must reproduce the
	// single-shot result to high precision regardless of the earlier intermediate queries.
	const float splits[][3] = { { 1.0f, 2.0f, 2.0f }, { 0.7f, 3.3f, 1.0f }, { 2.5f, 1.5f, 1.0f } };
	for( auto &s : splits )
	{
		Matrix4f acc;
		float t = 0;
		for( int k = 0; k < 3; ++k )
		{
			t += s[k];
			TASK_CHECK( p.evaluate( t ) );
			TASK_CHECK( p.getWorldRoot( acc ) );
		}
		TASK_CHECK( near( (double)t, 5.0, 1e-6 ) );
		for( int i = 0; i < 16; ++i ) TASK_CHECK( near( (double)full.x[i], acc.x[i], 2e-6 ) );
	}
	p.unbind();
	return 0;
}

static int integration_pause_zero_rate()
{
	std::vector<AnimJoint> j0, j1;
	std::vector<double> ry0, rx0, rz0, ry1, rx1, rz1;
	linearTurnTracks( j0, j1, ry0, rx0, rz0, ry1, rx1, rz1 );
	const Bytes a0 = makeAnim( 4, j0 ), a1 = makeAnim( 5, j1 );
	float ib0[16], ib1[16]; identityInvBind( ib0 ); identityInvBind( ib1 );
	const Bytes g = makeGeometry( ib0, ib1, false );

	World w; w.init( a0, a1, g );
	RootBlendPlayer p;
	const auto ks = keys( { { 0.0f, 1.0f }, { 30.0f, 1.0f } } );  // Only action 1 active
	TASK_CHECK( p.bind( (AnimationResource *)Modules::resMan().resolveResHandle( w.anim0 ),
	                    (AnimationResource *)Modules::resMan().resolveResHandle( w.anim1 ),
	                    10, 10, 0, 0, 0.0f, 1.0f, "root", w.modelNode,
	                    startPose( 0, 0, 0, 0 ), ks ) );

	RefAction ra0 = makeRef( ry0, rx0, rz0, 10, 0, 0.0 );
	RefAction ra1 = makeRef( ry1, rx1, rz1, 10, 0, 1.0 );
	RefSim ref{ &ra0, &ra1, { { 0, 30 }, { 1, 1 } } };
	TASK_CHECK( checkAgainstRef( p, ref, 4.0f, 0, 0, 0 ) == 0 );

	// Both rates zero: the root must not move regardless of time.
	RootBlendPlayer p2;
	const auto ks0 = keys( { { 0.0f, 0.5f }, { 30.0f, 0.5f } } );
	TASK_CHECK( p2.bind( (AnimationResource *)Modules::resMan().resolveResHandle( w.anim0 ),
	                     (AnimationResource *)Modules::resMan().resolveResHandle( w.anim1 ),
	                     10, 10, 0, 0, 0, 0, "root", w.modelNode,
	                     startPose( 3, 1, 2, 0.2f ), ks0 ) );
	Matrix4f m1, m2;
	TASK_CHECK( p2.evaluate( 0.0f ) ); TASK_CHECK( p2.getWorldRoot( m1 ) );
	TASK_CHECK( p2.evaluate( 9.0f ) ); TASK_CHECK( p2.getWorldRoot( m2 ) );
	for( int i = 0; i < 16; ++i ) TASK_CHECK( near( (double)m1.x[i], m2.x[i], 1e-9 ) );
	p.unbind(); p2.unbind();
	return 0;
}

// =================================================================================================
// pose: stripped root, blended joints, non-identity inverse bind, morph, transformed rigid parent
// =================================================================================================

static DMat engineToD( const Matrix4f &m )
{
	DMat r;
	for( int i = 0; i < 16; ++i ) r.m[i] = m.x[i];
	return r;
}

static int pose_full_pipeline()
{
	std::vector<AnimJoint> j0, j1;
	std::vector<double> ry0, rx0, rz0, ry1, rx1, rz1;
	linearTurnTracks( j0, j1, ry0, rx0, rz0, ry1, rx1, rz1 );
	const Bytes a0 = makeAnim( 4, j0 ), a1 = makeAnim( 5, j1 );

	// Non-identity inverse bind matrices.
	float ib0[16], ib1[16];
	identityInvBind( ib0 ); identityInvBind( ib1 );
	ib0[12] = 1.0f; ib0[14] = 2.0f;       // Translation in inverse bind of root
	ib1[13] = -1.0f;                       // Translation in inverse bind of limb
	const Bytes g = makeGeometry( ib0, ib1, true );

	World w; w.init( a0, a1, g );
	// Rigid parent with rotation and translation; a non-uniformly scaled sibling branch is allowed.
	h3dSetNodeTransform( w.parent, 5, 0, -2, 0, 35, 0, 1, 1, 1 );
	H3DNode sibling = h3dAddGroupNode( H3DRootNode, "sibling" );
	h3dSetNodeTransform( sibling, 0, 0, 0, 0, 0, 0, 2, 3, 4 );

	RootBlendPlayer p;
	const auto ks = keys( { { 0.0f, 0.25f }, { 10.0f, 0.75f } } );
	TASK_CHECK( p.bind( (AnimationResource *)Modules::resMan().resolveResHandle( w.anim0 ),
	                    (AnimationResource *)Modules::resMan().resolveResHandle( w.anim1 ),
	                    10, 10, 0, 0, 1, 1, "root", w.modelNode,
	                    startPose( 1, 0, -1, 0.3f ), ks ) );
	h3dSetModelMorpher( w.model, "lift", 0.5f );

	const float t = 3.0f;
	TASK_CHECK( p.evaluate( t ) );

	// --- Scalar reference for the integrated root ---
	RefAction ra0 = makeRef( ry0, rx0, rz0, 10, 0, 1 );
	RefAction ra1 = makeRef( ry1, rx1, rz1, 10, 0, 1 );
	RefSim ref{ &ra0, &ra1, { { 0, 10 }, { 0.25, 0.75 } } };
	double rx, rz, ryaw;
	ref.run( t, 1, -1, 0.3, rx, rz, ryaw );
	Matrix4f worldRoot; TASK_CHECK( p.getWorldRoot( worldRoot ) );
	TASK_CHECK( near( rx, worldRoot.x[12] ) );
	TASK_CHECK( near( rz, worldRoot.x[14] ) );

	// --- Reference skinning and world positions built from the engine's own joint matrices ---
	// This verifies the pipeline mechanics (root planar motion stripped, inverse-bind palette,
	// software skinning, morph and the rigid parent transform) without re-deriving the TRS blend.
	DMat dIb0; for( int i = 0; i < 16; ++i ) dIb0.m[i] = ib0[i];
	DMat dIb1; for( int i = 0; i < 16; ++i ) dIb1.m[i] = ib1[i];

	Modules::sceneMan().updateNodes();
	const float *rootRelF = 0, *limbRelF = 0;
	h3dGetNodeTransMats( w.root, &rootRelF, 0 );
	h3dGetNodeTransMats( w.limb, &limbRelF, 0 );
	const DMat rootRelModel = engineToD( *reinterpret_cast<const Matrix4f *>( rootRelF ) );
	const DMat limbRelLocal = engineToD( *reinterpret_cast<const Matrix4f *>( limbRelF ) );
	const DMat limbRelModel = rootRelModel * limbRelLocal;
	const DMat skin0 = rootRelModel * dIb0;
	const DMat skin1 = limbRelModel * dIb1;

	const DMat parentAbs = engineToD( w.modelNode->getParent()->getAbsTrans() );
	const DMat dWorldRoot = engineToD( worldRoot );
	const DMat modelRel = rigidInverse( parentAbs ) * dWorldRoot;

	const DVec base[3] = { DVec( 1, 0, 0 ), DVec( 0, 1, 0 ), DVec( 0, 0, 1 ) };
	const DVec morph( 0.25 * 0.5, 0, -0.5 * 0.5 );
	const DVec expectedModel[3] = {
		mulPoint( skin0, base[0] + DVec( morph.x, morph.y, morph.z ) ),
		mulPoint( skin0, base[1] ),
		mulPoint( skin1, base[2] )
	};

	GeometryResource *geo = w.modelNode->getGeometryResource();
	TASK_CHECK( geo != 0 );
	for( int v = 0; v < 3; ++v )
	{
		const Vec3f &got = geo->getVertPosData()[v];
		TASK_CHECK( nearV( expectedModel[v], got, 3e-5 ) );
		const DVec world = mulPoint( parentAbs * modelRel, expectedModel[v] );
		const Vec3f gotWorld = w.modelNode->getAbsTrans() * got;
		TASK_CHECK( nearV( world, gotWorld, 3e-5 ) );
	}

	// The animated root joint carries no planar translation of its own anymore (only height/Y).
	const float *relMat = 0;
	h3dGetNodeTransMats( w.root, &relMat, 0 );
	TASK_CHECK( near( relMat[12], 0.0, 1e-6 ) );
	TASK_CHECK( near( relMat[14], 0.0, 1e-6 ) );

	// The sibling branch kept its non-uniform scale and the parent stayed rigid.
	const float *sibMat = 0;
	h3dGetNodeTransMats( sibling, &sibMat, 0 );
	TASK_CHECK( near( sibMat[0], 2.0, 1e-6 ) && near( sibMat[5], 3.0, 1e-6 ) &&
	            near( sibMat[10], 4.0, 1e-6 ) );
	p.unbind();
	return 0;
}

// =================================================================================================
// lifecycle: two independent models, failure recovery, unbind restores ordinary stages
// =================================================================================================

static int lifecycle_two_models_and_restore()
{
	std::vector<AnimJoint> j0, j1;
	std::vector<double> ry0, rx0, rz0, ry1, rx1, rz1;
	linearTurnTracks( j0, j1, ry0, rx0, rz0, ry1, rx1, rz1 );
	const Bytes a0 = makeAnim( 4, j0 ), a1 = makeAnim( 5, j1 );
	float ib0[16], ib1[16]; identityInvBind( ib0 ); identityInvBind( ib1 );
	const Bytes g = makeGeometry( ib0, ib1, false );

	H3DRes ra = loadRes( H3DResTypes::Animation, "shared0.anim", a0 );
	H3DRes rb = loadRes( H3DResTypes::Animation, "shared1.anim", a1 );
	H3DRes gg = loadRes( H3DResTypes::Geometry, "shared.geo", g );
	H3DRes mm = h3dAddResource( H3DResTypes::Material, "mm.material", 0 );

	H3DNode mA = h3dAddModelNode( H3DRootNode, "mA", gg );
	H3DNode rA = h3dAddJointNode( mA, "root", 0 );
	h3dAddJointNode( rA, "limb", 1 );
	h3dAddMeshNode( mA, "t", mm, H3DMeshPrimType::TriangleList, 0, 3, 0, 2 );
	H3DNode mB = h3dAddModelNode( H3DRootNode, "mB", gg );
	H3DNode rB = h3dAddJointNode( mB, "root", 0 );
	h3dAddJointNode( rB, "limb", 1 );
	h3dAddMeshNode( mB, "t", mm, H3DMeshPrimType::TriangleList, 0, 3, 0, 2 );
	ModelNode *nodeA = (ModelNode *)Modules::sceneMan().resolveNodeHandle( mA );
	ModelNode *nodeB = (ModelNode *)Modules::sceneMan().resolveNodeHandle( mB );

	RootBlendPlayer pa, pb;
	const auto ks = keys( { { 0, 0.3f }, { 30, 0.6f } } );
	TASK_CHECK( pa.bind( (AnimationResource *)Modules::resMan().resolveResHandle( ra ),
	                     (AnimationResource *)Modules::resMan().resolveResHandle( rb ),
	                     10, 10, 0, 0, 1, 1, "root", nodeA, startPose( 0, 0, 0, 0 ), ks ) );
	TASK_CHECK( pb.bind( (AnimationResource *)Modules::resMan().resolveResHandle( ra ),
	                     (AnimationResource *)Modules::resMan().resolveResHandle( rb ),
	                     10, 10, 0, 0, 1, 1, "root", nodeB,
	                     startPose( 10, 0, 10, 0 ), ks ) );

	TASK_CHECK( pa.evaluate( 2.0f ) );
	TASK_CHECK( pb.evaluate( 2.0f ) );
	Matrix4f wa, wb;
	TASK_CHECK( pa.getWorldRoot( wa ) );
	TASK_CHECK( pb.getWorldRoot( wb ) );
	// Different start poses must not contaminate each other.
	TASK_CHECK( near( wa.x[12], wb.x[12] - 10.0, 2e-5 ) );
	TASK_CHECK( near( wa.x[14], wb.x[14] - 10.0, 2e-5 ) );

	// Unbind A and drive it through the ordinary stage pipeline again.
	pa.unbind();
	TASK_CHECK( !pa.isBound() );
	TASK_CHECK( h3dGetNodeParamI( mA, H3DModel::AnimCountI ) == 0 );
	h3dSetupModelAnimStage( mA, 0, ra, 0, "", false );
	h3dSetModelAnimParams( mA, 0, 1.0f, 1.0f );
	h3dUpdateModel( mA, H3DModelUpdateFlags::Animation );
	const float *limbRel = 0;
	H3DNode limbA = h3dGetNodeChild( rA, 0 );
	h3dGetNodeTransMats( limbA, &limbRel, 0 );
	TASK_CHECK( near( limbRel[13], 1.1, 3e-4 ) );  // limb0 frame 1 translation y = 1.1

	// B keeps working while A is back on the normal path.
	TASK_CHECK( pb.evaluate( 3.0f ) );
	Matrix4f wb2; TASK_CHECK( pb.getWorldRoot( wb2 ) );
	TASK_CHECK( std::isfinite( wb2.x[12] ) );

	// A can be re-bound after unbind.
	TASK_CHECK( pa.bind( (AnimationResource *)Modules::resMan().resolveResHandle( ra ),
	                     (AnimationResource *)Modules::resMan().resolveResHandle( rb ),
	                     10, 10, 0, 0, 1, 1, "root", nodeA, startPose( 0, 0, 0, 0 ), ks ) );
	TASK_CHECK( pa.evaluate( 1.0f ) );
	pb.unbind();
	return 0;
}

// =================================================================================================
// invalid: every rejection path, state preservation, recovery
// =================================================================================================

static int invalid_rejections()
{
	std::vector<AnimJoint> j0, j1;
	std::vector<double> ry0, rx0, rz0, ry1, rx1, rz1;
	linearTurnTracks( j0, j1, ry0, rx0, rz0, ry1, rx1, rz1 );
	const Bytes a0 = makeAnim( 4, j0 ), a1 = makeAnim( 5, j1 );
	float ib0[16], ib1[16]; identityInvBind( ib0 ); identityInvBind( ib1 );
	const Bytes g = makeGeometry( ib0, ib1, false );
	World w; w.init( a0, a1, g );

	AnimationResource *r0 = (AnimationResource *)Modules::resMan().resolveResHandle( w.anim0 );
	AnimationResource *r1 = (AnimationResource *)Modules::resMan().resolveResHandle( w.anim1 );
	const auto good = keys( { { 0, 0.2f }, { 30, 0.8f } } );
	RootBlendPlayer p;

	// Null resource.
	TASK_CHECK( !p.bind( 0, r1, 10, 10, 0, 0, 1, 1, "root", w.modelNode,
	                     startPose( 0, 0, 0, 0 ), good ) );
	TASK_CHECK( !p.isBound() );
	// Missing root joint name.
	TASK_CHECK( !p.bind( r0, r1, 10, 10, 0, 0, 1, 1, "ghost", w.modelNode,
	                     startPose( 0, 0, 0, 0 ), good ) );
	// Non-finite rate.
	TASK_CHECK( !p.bind( r0, r1, 10, 10, 0, 0, NAN, 1, "root", w.modelNode,
	                     startPose( 0, 0, 0, 0 ), good ) );
	// Non-positive fps.
	TASK_CHECK( !p.bind( r0, r1, 0, 10, 0, 0, 1, 1, "root", w.modelNode,
	                     startPose( 0, 0, 0, 0 ), good ) );
	// Bad weight keys: out of order and out of range.
	TASK_CHECK( !p.bind( r0, r1, 10, 10, 0, 0, 1, 1, "root", w.modelNode,
	                     startPose( 0, 0, 0, 0 ), keys( { { 2, 0.5f }, { 1, 0.5f } } ) ) );
	TASK_CHECK( !p.bind( r0, r1, 10, 10, 0, 0, 1, 1, "root", w.modelNode,
	                     startPose( 0, 0, 0, 0 ), keys( { { 0, 1.2f } } ) ) );
	// Tilted start pose (non-Y rotation).
	Matrix4f tilted = Matrix4f( Quaternion( 0.5f, 0, 0 ) );
	TASK_CHECK( !p.bind( r0, r1, 10, 10, 0, 0, 1, 1, "root", w.modelNode, tilted, good ) );
	// Model stages untouched by the failed attempts.
	TASK_CHECK( h3dGetNodeParamI( w.model, H3DModel::AnimCountI ) == 0 );

	// A valid bind still works after all rejections.
	TASK_CHECK( p.bind( r0, r1, 10, 10, 0, 0, 1, 1, "root", w.modelNode,
	                    startPose( 0, 0, 0, 0 ), good ) );
	TASK_CHECK( p.evaluate( 1.0f ) );
	Matrix4f before; TASK_CHECK( p.getWorldRoot( before ) );

	// Out-of-range and non-finite queries fail and preserve state.
	TASK_CHECK( !p.evaluate( -0.1f ) );
	TASK_CHECK( !p.evaluate( 30.1f ) );
	TASK_CHECK( !p.evaluate( NAN ) );
	Matrix4f after; TASK_CHECK( p.getWorldRoot( after ) );
	for( int i = 0; i < 16; ++i ) TASK_CHECK( near( (double)before.x[i], after.x[i], 1e-9 ) );

	// Recovery: a legal query after the failed ones still works.
	TASK_CHECK( p.evaluate( 2.0f ) );
	p.unbind();
	return 0;
}

static int invalid_bad_tracks_and_parent()
{
	float ib0[16], ib1[16]; identityInvBind( ib0 ); identityInvBind( ib1 );
	const Bytes g = makeGeometry( ib0, ib1, false );
	H3DRes gg = loadRes( H3DResTypes::Geometry, "bg.geo", g );
	H3DRes mm = h3dAddResource( H3DResTypes::Material, "bm.material", 0 );

	// Root track with non-unit scale.
	AnimJoint badScale; badScale.name = "root";
	JointFrame bf = rootFrame( 0, 0, 0 ); bf.sx = 2.0f;
	badScale.frames.push_back( bf );
	badScale.frames.push_back( rootFrame( 0.5f, 1, 0 ) );
	Bytes bs = makeAnim( 2, { badScale } );
	H3DRes badS = loadRes( H3DResTypes::Animation, "badscale.anim", bs );

	// Root track with a tilted (non-Y) rotation.
	AnimJoint badRot; badRot.name = "root";
	JointFrame rf0 = rootFrame( 0, 0, 0 ); rf0.qx = 0.1f; rf0.qw = 0.994987f;
	badRot.frames.push_back( rf0 );
	badRot.frames.push_back( rootFrame( 0.5f, 1, 0 ) );
	Bytes br = makeAnim( 2, { badRot } );
	H3DRes badR = loadRes( H3DResTypes::Animation, "badrot.anim", br );

	std::vector<AnimJoint> j0, j1;
	std::vector<double> ry0, rx0, rz0, ry1, rx1, rz1;
	linearTurnTracks( j0, j1, ry0, rx0, rz0, ry1, rx1, rz1 );
	H3DRes goodA = loadRes( H3DResTypes::Animation, "good.anim", makeAnim( 4, j0 ) );

	H3DNode parent = h3dAddGroupNode( H3DRootNode, "scaledParent" );
	h3dSetNodeTransform( parent, 0, 0, 0, 0, 0, 0, 2, 2, 2 );  // Non-rigid chain
	H3DNode model = h3dAddModelNode( parent, "m", gg );
	H3DNode root = h3dAddJointNode( model, "root", 0 );
	h3dAddJointNode( root, "limb", 1 );
	h3dAddMeshNode( model, "t", mm, H3DMeshPrimType::TriangleList, 0, 3, 0, 2 );
	ModelNode *node = (ModelNode *)Modules::sceneMan().resolveNodeHandle( model );

	RootBlendPlayer p;
	const auto good = keys( { { 0, 0.5f } } );
	AnimationResource *pGood = (AnimationResource *)Modules::resMan().resolveResHandle( goodA );
	AnimationResource *pBadS = (AnimationResource *)Modules::resMan().resolveResHandle( badS );
	AnimationResource *pBadR = (AnimationResource *)Modules::resMan().resolveResHandle( badR );

	TASK_CHECK( !p.bind( pBadS, pGood, 10, 10, 0, 0, 1, 1, "root", node,
	                     startPose( 0, 0, 0, 0 ), good ) );
	TASK_CHECK( !p.bind( pGood, pBadR, 10, 10, 0, 0, 1, 1, "root", node,
	                     startPose( 0, 0, 0, 0 ), good ) );
	TASK_CHECK( !p.bind( pGood, pGood, 10, 10, 0, 0, 1, 1, "root", node,
	                     startPose( 0, 0, 0, 0 ), good ) );  // Rejected by non-rigid parent
	TASK_CHECK( !p.isBound() );

	// Re-parent onto a rigid group and the same resources bind successfully.
	H3DNode rigidParent = h3dAddGroupNode( H3DRootNode, "rigidParent" );
	h3dSetNodeTransform( rigidParent, 1, 0, 1, 0, 20, 0, 1, 1, 1 );
	Modules::sceneMan().relocateNode( *node, *Modules::sceneMan().resolveNodeHandle( rigidParent ) );
	TASK_CHECK( p.bind( pGood, pGood, 10, 10, 0, 0, 1, 1, "root", node,
	                    startPose( 0, 0, 0, 0 ), good ) );
	TASK_CHECK( p.evaluate( 1.5f ) );
	p.unbind();
	return 0;
}

// =================================================================================================
// Registration
// =================================================================================================

const task_case task_cases[] = {
	{ "twist", "neg_quat", twist_neg_quat },
	{ "twist", "diff_fps_phase", twist_diff_fps_phase },
	{ "twist", "reverse_wrap", twist_reverse_wrap },
	{ "twist", "turn_while_blending", twist_turn_while_blending },
	{ "integration", "split_consistency", integration_split_consistency },
	{ "integration", "pause_zero_rate", integration_pause_zero_rate },
	{ "pose", "full_pipeline", pose_full_pipeline },
	{ "lifecycle", "two_models_restore", lifecycle_two_models_and_restore },
	{ "invalid", "rejections", invalid_rejections },
	{ "invalid", "bad_tracks_parent", invalid_bad_tracks_and_parent },
};

const size_t task_case_count = sizeof( task_cases ) / sizeof( task_cases[0] );
