// *************************************************************************************************
//
// Horde3D Motion Matching - task test suite
//
// The tests generate short H3DA animations, a skeleton with a static side branch and a small
// skinned triangle with non-identity inverse-bind matrices entirely in code. All feature values,
// candidate costs and final world vertices are cross-checked against independent scalar/matrix
// reference implementations in this file rather than against the engine code under test.
//
// *************************************************************************************************

#include "task_support/check.h"

#include "Horde3D.h"
#include "egModules.h"
#include "egScene.h"
#include "egModel.h"
#include "egAnimatables.h"
#include "egGeometry.h"
#include "egAnimation.h"
#include "egMotionMatching.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace Horde3D;


namespace
{

// ------------------------------------------------------------------------------------------------
// Small test utilities
// ------------------------------------------------------------------------------------------------

const float PI = 3.14159265358979323846f;


// The whole suite shares one headless engine instance.
static int ensureEngine()
{
	static bool ready = false;
	if( !ready )
	{
		TASK_CHECK( h3dInit( static_cast< H3DRenderDevice::List >( 256 ) ) );
		h3dSetOption( H3DOptions::FastAnimation, 0 );
		ready = true;
	}
	return 0;
}


bool nearF( float a, float b, float eps = 1.0e-4f )
{
	return std::isfinite( a ) && std::isfinite( b ) && std::fabs( a - b ) <= eps;
}


bool nearVec( const Vec3f &a, const Vec3f &b, float eps = 1.0e-4f )
{
	return nearF( a.x, b.x, eps ) && nearF( a.y, b.y, eps ) && nearF( a.z, b.z, eps );
}


bool nearMat( const Matrix4f &a, const Matrix4f &b, float eps = 1.0e-4f )
{
	for( uint32 i = 0; i < 16; ++i )
		if( !nearF( a.x[i], b.x[i], eps ) ) return false;
	return true;
}


// ------------------------------------------------------------------------------------------------
// Independent scalar/matrix reference (kept separate from the engine math types)
// ------------------------------------------------------------------------------------------------

struct RefQuat
{
	float x, y, z, w;
};


struct RefMat
{
	// Column-major, same memory layout as Matrix4f
	float m[16];

	RefMat()
	{
		std::memset( m, 0, sizeof( m ) );
		m[0] = m[5] = m[10] = m[15] = 1.0f;
	}

	static RefMat yRotation( float yaw )
	{
		// Build through the same quaternion as the engine so the column-major layout matches
		// Matrix4f(Quaternion) exactly: x[2] = sin(yaw), x[8] = -sin(yaw).
		RefQuat q;
		q.x = 0.0f;
		q.y = std::sin( yaw * 0.5f );
		q.z = 0.0f;
		q.w = std::cos( yaw * 0.5f );
		return fromQuat( q );
	}

	static RefMat fromQuat( const RefQuat &q )
	{
		RefMat r;
		const float x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
		const float xx = q.x * x2, xy = q.x * y2, xz = q.x * z2;
		const float yy = q.y * y2, yz = q.y * z2, zz = q.z * z2;
		const float wx = q.w * x2, wy = q.w * y2, wz = q.w * z2;
		r.m[0] = 1 - ( yy + zz ); r.m[4] = xy - wz;      r.m[8]  = xz + wy;
		r.m[1] = xy + wz;         r.m[5] = 1 - ( xx + zz ); r.m[9]  = yz - wx;
		r.m[2] = xz - wy;         r.m[6] = yz + wx;      r.m[10] = 1 - ( xx + yy );
		return r;
	}

	static RefMat translation( float x, float y, float z )
	{
		RefMat r;
		r.m[12] = x; r.m[13] = y; r.m[14] = z;
		return r;
	}
};


RefMat operator*( const RefMat &a, const RefMat &b )
{
	RefMat r;
	for( uint32 col = 0; col < 4; ++col )
		for( uint32 row = 0; row < 4; ++row )
		{
			float sum = 0.0f;
			for( uint32 k = 0; k < 4; ++k )
				sum += a.m[k * 4 + row] * b.m[col * 4 + k];
			r.m[col * 4 + row] = sum;
		}
	return r;
}


Vec3f refTransform( const RefMat &m, const Vec3f &v )
{
	return Vec3f(
		m.m[0] * v.x + m.m[4] * v.y + m.m[8]  * v.z + m.m[12],
		m.m[1] * v.x + m.m[5] * v.y + m.m[9]  * v.z + m.m[13],
		m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14] );
}


RefMat refRigid( const RefQuat &q, const Vec3f &t )
{
	return RefMat::translation( t.x, t.y, t.z ) * RefMat::fromQuat( q );
}


RefQuat yQuat( float yaw )
{
	RefQuat q;
	q.x = 0.0f;
	q.y = std::sin( yaw * 0.5f );
	q.z = 0.0f;
	q.w = std::cos( yaw * 0.5f );
	return q;
}


RefQuat identityQuat()
{
	RefQuat q;
	q.x = q.y = q.z = 0.0f;
	q.w = 1.0f;
	return q;
}


float quatDot( const RefQuat &a, const RefQuat &b )
{
	return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}


// Shortest-arc normalized linear quaternion interpolation, matching Quaternion::nlerp.
RefQuat refNlerp( RefQuat a, const RefQuat &b, float t )
{
	RefQuat bb = b;
	if( quatDot( a, bb ) < 0.0f )
	{
		bb.x = -bb.x; bb.y = -bb.y; bb.z = -bb.z; bb.w = -bb.w;
	}
	RefQuat r;
	r.x = a.x + ( bb.x - a.x ) * t;
	r.y = a.y + ( bb.y - a.y ) * t;
	r.z = a.z + ( bb.z - a.z ) * t;
	r.w = a.w + ( bb.w - a.w ) * t;
	const float inv = 1.0f / std::sqrt( r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w );
	r.x *= inv; r.y *= inv; r.z *= inv; r.w *= inv;
	return r;
}


float refYawOf( const RefMat &m )
{
	// Matches the engine's yawFromYRotMat: atan2(c[2][0], c[0][0]), i.e. the flat column-major
	// elements x[8] (sin yaw) and x[0] (cos yaw).
	return std::atan2( m.m[8], m.m[0] );
}


Vec3f refRotateVec( const RefMat &m, const Vec3f &v )
{
	return Vec3f(
		m.m[0] * v.x + m.m[4] * v.y + m.m[8]  * v.z,
		m.m[1] * v.x + m.m[5] * v.y + m.m[9]  * v.z,
		m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z );
}


RefMat refInverseRigid( const RefMat &m )
{
	// Inverse of a rigid transform: transposed rotation, negated/rotated translation.
	RefMat r;
	r.m[0] = m.m[0]; r.m[4] = m.m[1]; r.m[8]  = m.m[2];
	r.m[1] = m.m[4]; r.m[5] = m.m[5]; r.m[9]  = m.m[6];
	r.m[2] = m.m[8]; r.m[6] = m.m[9]; r.m[10] = m.m[10];
	const Vec3f t( m.m[12], m.m[13], m.m[14] );
	const Vec3f nt = refRotateVec( r, t ) * -1.0f;
	r.m[12] = nt.x; r.m[13] = nt.y; r.m[14] = nt.z;
	return r;
}


RefMat toRef( const Matrix4f &m )
{
	RefMat r;
	std::memcpy( r.m, &m.x[0], sizeof( float ) * 16 );
	return r;
}


// ------------------------------------------------------------------------------------------------
// Programmatic resource builders
// ------------------------------------------------------------------------------------------------

struct Bytes
{
	std::vector< char > data;

	void u32( uint32_t v ) { for( int i = 0; i < 4; ++i ) data.push_back( char( v >> ( 8 * i ) ) ); }
	void u16( int v ) { data.push_back( char( v ) ); data.push_back( char( v >> 8 ) ); }
	void u8( int v ) { data.push_back( char( v ) ); }
	void f32( float v ) { uint32_t bits; std::memcpy( &bits, &v, 4 ); u32( bits ); }
	void text( const char *s, size_t n )
	{
		size_t len = std::strlen( s );
		for( size_t i = 0; i < n; ++i ) data.push_back( i < len ? s[i] : 0 );
	}
};


struct AnimFrame
{
	RefQuat q;
	Vec3f   t;
};


struct AnimBuilder
{
	Bytes bytes;

	AnimBuilder() {}

	void begin( uint32_t entityCount, uint32_t frameCount )
	{
		bytes.text( "H3DA", 4 );
		bytes.u32( 3 );       // Version 3
		bytes.u32( entityCount );
		bytes.u32( frameCount );
	}

	void entity( const char *name, const std::vector< AnimFrame > &frames, bool compressed = false )
	{
		bytes.text( name, 256 );
		bytes.u8( compressed ? 1 : 0 );
		const uint32_t count = compressed ? 1u : (uint32_t)frames.size();
		for( uint32_t i = 0; i < count; ++i )
		{
			const AnimFrame &f = frames[ compressed ? 0 : i ];
			bytes.f32( f.q.x ); bytes.f32( f.q.y ); bytes.f32( f.q.z ); bytes.f32( f.q.w );
			bytes.f32( f.t.x ); bytes.f32( f.t.y ); bytes.f32( f.t.z );
			bytes.f32( 1.0f );  bytes.f32( 1.0f );  bytes.f32( 1.0f );
		}
	}

	H3DRes load( const char *name ) const
	{
		H3DRes r = h3dAddResource( H3DResTypes::Animation, name, 0 );
		if( r && h3dLoadResource( r, bytes.data.data(), (int)bytes.data.size() ) ) return r;
		return 0;
	}
};


AnimFrame rootFrame( float yaw, float x, float z = 0.0f )
{
	AnimFrame f;
	f.q = yQuat( yaw );
	f.t = Vec3f( x, 0.0f, z );
	return f;
}


AnimFrame limbFrame( const Vec3f &t, const RefQuat &q = identityQuat() )
{
	AnimFrame f;
	f.q = q;
	f.t = t;
	return f;
}


// Builds the small skinned triangle. Three vertices, two joints (a root and a limb), a
// non-identity inverse bind on the limb and one morph target "lift" moving vertex 0 up by one.
// All vertices are fully bound to the limb joint (joint slot 1) so the animated hierarchy,
// inverse-bind palette and software skinning all move the triangle.
H3DRes buildTriangleGeometry( const char *name, const Matrix4f &invBind,
                              const Vec3f positions[3] )
{
	Bytes b;
	b.text( "H3DG", 4 );
	b.u32( 5 );          // Version 5
	b.u32( 2 );          // Two joints: slot 0 root, slot 1 limb
	for( int i = 0; i < 16; ++i ) b.f32( i % 5 == 0 ? 1.0f : 0.0f );  // Root inverse bind: identity
	for( int i = 0; i < 16; ++i ) b.f32( invBind.x[i] );              // Limb inverse bind

	b.u32( 6 );          // Six streams
	b.u32( 3 );          // Three vertices

	// Stream 0: positions
	b.u32( 0 ); b.u32( 12 );
	for( int v = 0; v < 3; ++v )
		for( int c = 0; c < 3; ++c )
			b.f32( c == 0 ? positions[v].x : c == 1 ? positions[v].y : positions[v].z );
	// Streams 1-3: normal, tangent, bitangent (short, unit axis samples)
	for( int stream = 1; stream <= 3; ++stream )
	{
		b.u32( stream ); b.u32( 6 );
		for( int v = 0; v < 3; ++v )
			for( int c = 0; c < 3; ++c )
				b.u16( c == stream - 1 ? 32767 : 0 );
	}
	// Stream 4: joint indices, every vertex bound to limb slot 1
	b.u32( 4 ); b.u32( 4 );
	for( int v = 0; v < 3; ++v ) { b.u8( 1 ); b.u8( 0 ); b.u8( 0 ); b.u8( 0 ); }
	// Stream 5: weights, full weight on the first index
	b.u32( 5 ); b.u32( 4 );
	for( int v = 0; v < 3; ++v ) { b.u8( 255 ); b.u8( 0 ); b.u8( 0 ); b.u8( 0 ); }

	// Indices
	b.u32( 3 );
	b.u32( 0 ); b.u32( 1 ); b.u32( 2 );

	// One morph target affecting only vertex 0
	b.u32( 1 );
	b.text( "lift", 256 );
	b.u32( 1 );            // One affected vertex
	b.u32( 0 );            // Vertex index 0
	b.u32( 1 );            // One morph stream
	b.u32( 0 ); b.u32( 12 );
	b.f32( 0.0f ); b.f32( 1.0f ); b.f32( 0.0f );

	H3DRes r = h3dAddResource( H3DResTypes::Geometry, name, 0 );
	if( r && h3dLoadResource( r, b.data.data(), (int)b.data.size() ) ) return r;
	return 0;
}


// ------------------------------------------------------------------------------------------------
// Clip description and a fully independent feature reference
// ------------------------------------------------------------------------------------------------

struct RefClip
{
	float                        fps;
	uint32                       frameCount;
	std::vector< std::string >   jointNames;   // Skeleton order
	std::vector< int >           parents;
	std::vector< Matrix4f >      restLocal;
	// Per joint, per frame local rigid matrix; missing tracks are marked and use restLocal.
	std::vector< bool >                         hasTrack;
	std::vector< std::vector< RefMat > >        localMats;

	float duration() const { return (float)( frameCount - 1 ) / fps; }
};


RefMat refSampleLocal( const RefClip &clip, uint32 joint, float t )
{
	if( !clip.hasTrack[joint] )
		return toRef( clip.restLocal[joint] );

	const float ff = t * clip.fps;
	int f0 = (int)ff;
	float a = ff - (float)f0;
	if( f0 < 0 ) { f0 = 0; a = 0.0f; }
	if( (uint32)f0 >= clip.frameCount ) { f0 = (int)clip.frameCount - 1; a = 0.0f; }
	if( f0 == (int)clip.frameCount - 1 ) a = 0.0f;
	uint32 f1 = f0 + 1;
	if( f1 > clip.frameCount - 1 ) f1 = clip.frameCount - 1;

	// Local matrices are rigid; decompose back to quaternion/translation for nlerp by rebuilding
	// through the frame data is not available here, so interpolate rotation via the matrix basis
	// extracted as quaternions.
	const RefMat &m0 = clip.localMats[joint][f0];
	const RefMat &m1 = clip.localMats[joint][f1];
	RefQuat q0 = yQuat( refYawOf( m0 ) );  // Generic rotation extraction is not needed: the
	RefQuat q1 = yQuat( refYawOf( m1 ) );  // generated fixtures only use Y or identity rotations.
	RefQuat q = refNlerp( q0, q1, a );
	Vec3f t0( m0.m[12], m0.m[13], m0.m[14] );
	Vec3f t1( m1.m[12], m1.m[13], m1.m[14] );
	return refRigid( q, t0.lerp( t1, a ) );
}


// Evaluates model-space matrices for every joint at time t.
std::vector< RefMat > refModelMats( const RefClip &clip, float t )
{
	const uint32 n = (uint32)clip.jointNames.size();
	std::vector< RefMat > locals( n ), models( n );
	for( uint32 j = 0; j < n; ++j ) locals[j] = refSampleLocal( clip, j, t );
	for( uint32 j = 0; j < n; ++j )
	{
		std::vector< uint32 > chain;
		int cur = (int)j;
		while( cur != -1 ) { chain.push_back( (uint32)cur ); cur = clip.parents[cur]; }
		RefMat acc;
		for( int k = (int)chain.size() - 1; k >= 0; --k )
			acc = acc * locals[chain[k]];
		models[j] = acc;
	}
	return models;
}


struct RefFeatures
{
	std::vector< Vec3f > positions;
	std::vector< Vec3f > velocities;
	Vec3f                futureDisp;
	float                futureYaw;
};


RefFeatures refComputeFeatures( const RefClip &clip, const std::vector< uint32 > &featureJoints,
                                uint32 frame, float horizon )
{
	const float t = (float)frame / clip.fps;
	const float delta = 1.0f / clip.fps;

	std::vector< RefMat > mt = refModelMats( clip, t );
	std::vector< RefMat > mm = refModelMats( clip, t - delta );
	std::vector< RefMat > mp = refModelMats( clip, t + delta );
	std::vector< RefMat > mh = refModelMats( clip, t + horizon );

	uint32 root = 0;
	for( uint32 j = 0; j < (uint32)clip.parents.size(); ++j )
		if( clip.parents[j] == -1 ) root = j;
	RefMat invG = refInverseRigid( mt[root] );
	RefMat invGRot = invG;
	invGRot.m[12] = invGRot.m[13] = invGRot.m[14] = 0.0f;

	RefFeatures f;
	for( uint32 i = 0; i < (uint32)featureJoints.size(); ++i )
	{
		const uint32 j = featureJoints[i];
		Vec3f pT( mt[j].m[12], mt[j].m[13], mt[j].m[14] );
		Vec3f pM( mm[j].m[12], mm[j].m[13], mm[j].m[14] );
		Vec3f pP( mp[j].m[12], mp[j].m[13], mp[j].m[14] );
		f.positions.push_back( refTransform( invG, pT ) );
		Vec3f modelVel = ( pP - pM ) * ( 1.0f / ( 2.0f * delta ) );
		f.velocities.push_back( refRotateVec( invGRot, modelVel ) );
	}

	RefMat futureRoot = invG * mh[root];
	f.futureDisp = Vec3f( futureRoot.m[12], 0.0f, futureRoot.m[14] );
	f.futureYaw = refYawOf( futureRoot );
	return f;
}


// Reference cost, identical component-by-component sum to the documented database formula.
float refCost( const MMCandidateInfo &c, const MMSearchInput &q )
{
	float cost = 0.0f;
	for( uint32 i = 0; i < (uint32)c.positions.size(); ++i )
	{
		Vec3f dp( c.positions[i].x - q.positions[i].x,
		          c.positions[i].y - q.positions[i].y,
		          c.positions[i].z - q.positions[i].z );
		cost += q.weights.position * dp.dot( dp );
		Vec3f dv( c.velocities[i].x - q.velocities[i].x,
		          c.velocities[i].y - q.velocities[i].y,
		          c.velocities[i].z - q.velocities[i].z );
		cost += q.weights.velocity * dv.dot( dv );
	}
	float ddx = c.futureDisplacement.x - q.futureDisplacement.x;
	float ddz = c.futureDisplacement.z - q.futureDisplacement.z;
	cost += q.weights.trajectory * ( ddx * ddx + ddz * ddz );
	float dcx = std::cos( c.futureYaw ) - std::cos( q.futureYaw );
	float dsz = std::sin( c.futureYaw ) - std::sin( q.futureYaw );
	cost += q.weights.facing * ( dcx * dcx + dsz * dsz );
	return cost;
}


// ------------------------------------------------------------------------------------------------
// Skeleton / clip fixture construction
// ------------------------------------------------------------------------------------------------

MMSkeleton makeSkeleton( bool rootFirst = true )
{
	MMSkeleton skel;
	MMSkeletonJoint root, limb, side;
	root.name = "root";
	root.parentIndex = -1;
	root.restLocal = MMMat4( Matrix4f() );

	limb.name = "limb";
	limb.restLocal = MMMat4( Matrix4f::TransMat( 0.0f, 2.0f, 0.0f ) );

	side.name = "side";
	side.restLocal = MMMat4( Matrix4f::TransMat( 0.0f, 0.0f, 2.0f ) );

	if( rootFirst )
	{
		limb.parentIndex = 0;
		side.parentIndex = 0;
		skel.joints.push_back( root );
		skel.joints.push_back( limb );
		skel.joints.push_back( side );
	}
	else
	{
		// Deliberately unordered: children appear before their parent.
		limb.parentIndex = 1;
		side.parentIndex = 1;
		skel.joints.push_back( limb );
		skel.joints.push_back( root );
		skel.joints.push_back( side );
	}
	return skel;
}


// Straight walk: root advances along X with a fixed yaw step, limb translates on Y.
// The side joint gets no track and keeps its rest pose.
H3DRes buildWalkClip( const char *name, float fps, uint32 frameCount, float yawStep,
                      float xStep, float limbBase, float limbStep )
{
	(void)fps;  // Frame rate is supplied to the database separately; H3DA stores frames only.
	AnimBuilder b;
	b.begin( 2, frameCount );
	std::vector< AnimFrame > root, limb;
	for( uint32 i = 0; i < frameCount; ++i )
	{
		root.push_back( rootFrame( yawStep * (float)i, xStep * (float)i ) );
		limb.push_back( limbFrame( Vec3f( 0.0f, limbBase + limbStep * (float)i, 0.0f ) ) );
	}
	b.entity( "root", root );
	b.entity( "limb", limb );
	return b.load( name );
}


RefClip makeRefWalk( float fps, uint32 frameCount, float yawStep, float xStep,
                     float limbBase, float limbStep, const MMSkeleton &skel,
                     bool includeSide = false )
{
	RefClip c;
	c.fps = fps;
	c.frameCount = frameCount;
	const uint32 n = (uint32)skel.joints.size();
	c.jointNames.resize( n );
	c.parents.resize( n );
	c.restLocal.resize( n );
	c.hasTrack.assign( n, false );
	c.localMats.resize( n );
	for( uint32 j = 0; j < n; ++j )
	{
		c.jointNames[j] = skel.joints[j].name;
		c.parents[j] = skel.joints[j].parentIndex;
		c.restLocal[j] = skel.joints[j].restLocal.m;
	}
	int rootIdx = -1, limbIdx = -1;
	for( uint32 j = 0; j < n; ++j )
	{
		if( c.jointNames[j] == "root" ) rootIdx = (int)j;
		if( c.jointNames[j] == "limb" ) limbIdx = (int)j;
	}
	c.localMats[rootIdx].resize( frameCount );
	c.localMats[limbIdx].resize( frameCount );
	c.hasTrack[rootIdx] = true;
	c.hasTrack[limbIdx] = true;
	for( uint32 i = 0; i < frameCount; ++i )
	{
		c.localMats[rootIdx][i] =
			RefMat::translation( xStep * (float)i, 0.0f, 0.0f ) * RefMat::yRotation( yawStep * (float)i );
		c.localMats[limbIdx][i] =
			RefMat::translation( 0.0f, limbBase + limbStep * (float)i, 0.0f );
	}
	if( includeSide )
	{
		int sideIdx = -1;
		for( uint32 j = 0; j < n; ++j )
			if( c.jointNames[j] == "side" ) sideIdx = (int)j;
		c.hasTrack[sideIdx] = false;
	}
	(void)rootIdx;
	return c;
}


// Builds the engine scene graph for a skinned character and returns the model and joint handles.
struct Character
{
	H3DNode parent;
	H3DNode model;
	H3DNode rootJoint;
	H3DNode limbJoint;
	H3DNode sideJoint;
	ModelNode *modelPtr;
};


Character buildCharacter( const char *prefix, const Matrix4f &invBind, const Vec3f positions[3],
                          float parentYawDeg, const Vec3f &parentTrans )
{
	Character ch;
	ch.parent = h3dAddGroupNode( H3DRootNode, ( std::string( prefix ) + "_parent" ).c_str() );
	h3dSetNodeTransform( ch.parent, parentTrans.x, parentTrans.y, parentTrans.z,
	                     0.0f, parentYawDeg, 0.0f, 1.0f, 1.0f, 1.0f );

	H3DRes geo = buildTriangleGeometry( ( std::string( prefix ) + ".geo" ).c_str(), invBind, positions );
	ch.model = h3dAddModelNode( ch.parent, ( std::string( prefix ) + "_model" ).c_str(), geo );
	h3dSetNodeTransform( ch.model, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f );
	h3dSetNodeParamI( ch.model, H3DModel::SWSkinningI, 1 );

	ch.rootJoint = h3dAddJointNode( ch.model, "root", 0 );
	ch.limbJoint = h3dAddJointNode( ch.rootJoint, "limb", 1 );
	ch.sideJoint = h3dAddJointNode( ch.rootJoint, "side", 2 );
	h3dSetNodeTransform( ch.sideJoint, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f );

	H3DRes material = h3dAddResource( H3DResTypes::Material,
	                                  ( std::string( prefix ) + ".material" ).c_str(), 0 );
	h3dAddMeshNode( ch.model, "triangle", material, H3DMeshPrimType::TriangleList, 0, 3, 0, 2 );

	ch.modelPtr = static_cast< ModelNode * >( Modules::sceneMan().resolveNodeHandle( ch.model ) );
	return ch;
}


// A deliberately non-identity inverse bind: inverse of a translation by (1, 2, 0).
Matrix4f testInvBind()
{
	return Matrix4f::TransMat( -1.0f, -2.0f, 0.0f );
}


// First legal candidate of a database.
MMCandidateInfo firstCandidate( const MotionDatabase &db )
{
	MMCandidateInfo info;
	db.getCandidate( 0, info );
	return info;
}

}  // namespace


// Forward declaration; defined with the handoff helpers below.
static MMSearchInput queryFromCandidate( const MMCandidateInfo &c );


// =================================================================================================
// Group: features
// =================================================================================================

static int features_basic()
{
	ensureEngine();
	const float fps = 10.0f, h = 0.2f;
	H3DRes anim = buildWalkClip( "feat_walk.anim", (uint32)fps, 5, 0.3f, 0.5f, 2.0f, 0.5f );
	TASK_CHECK( anim != 0 );

	MMSkeleton skel = makeSkeleton( true );
	MotionDatabase db;
	std::vector< MMClip > clips;
	clips.push_back( MMClip( (AnimationResource *)Modules::resMan().resolveResHandle( anim ), fps ) );
	std::vector< std::string > features;
	features.push_back( "root" );
	features.push_back( "limb" );
	features.push_back( "side" );
	TASK_CHECK( db.build( skel, clips, features, h ) );
	TASK_CHECK( db.getClipCount() == 1 );
	TASK_CHECK( db.getCandidateCount() == 2 );  // Frames 1 and 2 only
	TASK_CHECK( nearF( db.getClipDuration( 0 ), 0.4f ) );

	RefClip ref = makeRefWalk( fps, 5, 0.3f, 0.5f, 2.0f, 0.5f, skel );
	std::vector< uint32 > refFeatures;
	refFeatures.push_back( 0 );
	refFeatures.push_back( 1 );
	refFeatures.push_back( 2 );

	for( uint32 frame = 1; frame <= 2; ++frame )
	{
		MMCandidateInfo info;
		TASK_CHECK( db.getCandidate( frame - 1, info ) );
		TASK_CHECK( info.clipIndex == 0 && info.frame == frame );
		TASK_CHECK( nearF( info.sourceTime, (float)frame / fps ) );

		RefFeatures rf = refComputeFeatures( ref, refFeatures, frame, h );
		for( uint32 j = 0; j < features.size(); ++j )
		{
			TASK_CHECK( nearVec( Vec3f( info.positions[j].x, info.positions[j].y, info.positions[j].z ),
			                     rf.positions[j] ) );
			TASK_CHECK( nearVec( Vec3f( info.velocities[j].x, info.velocities[j].y, info.velocities[j].z ),
			                     rf.velocities[j] ) );
		}
		TASK_CHECK( nearF( info.futureDisplacement.x, rf.futureDisp.x ) );
		TASK_CHECK( nearF( info.futureDisplacement.z, rf.futureDisp.z ) );
		TASK_CHECK( nearF( info.futureDisplacement.y, 0.0f ) );
		TASK_CHECK( nearF( info.futureYaw, rf.futureYaw, 1.0e-4f ) );
	}

	// The static side branch never moves relative to the root: its root-frame position is the
	// same at every candidate even though the root itself translates and rotates.
	MMCandidateInfo c0 = firstCandidate( db ), c1;
	db.getCandidate( 1, c1 );
	TASK_CHECK( nearVec( Vec3f( c0.positions[2].x, c0.positions[2].y, c0.positions[2].z ),
	                     Vec3f( c1.positions[2].x, c1.positions[2].y, c1.positions[2].z ) ) );
	// The side velocity still agrees with the independent reference (tangential motion of a
	// fixed root-frame point while the root rotates), it is not silently zeroed.
	TASK_CHECK( nearVec( Vec3f( c0.velocities[2].x, c0.velocities[2].y, c0.velocities[2].z ),
	                     refComputeFeatures( ref, refFeatures, 1, h ).velocities[2] ) );
	return 0;
}


static int features_pi_crossing_and_fps()
{
	ensureEngine();
	// Yaw sequence crosses +pi so the shortest-arc nlerp must pass through the sign flip.
	const float fps20 = 20.0f, h = 0.1f;
	AnimBuilder b;
	const uint32 frames = 6;
	b.begin( 1, frames );
	std::vector< AnimFrame > root;
	for( uint32 i = 0; i < frames; ++i )
		root.push_back( rootFrame( 2.8f + 0.2f * (float)i, 0.25f * (float)i ) );
	b.entity( "root", root );
	H3DRes anim = b.load( "feat_pi.anim" );
	TASK_CHECK( anim != 0 );

	MMSkeleton skel;
	MMSkeletonJoint rj;
	rj.name = "root"; rj.parentIndex = -1; rj.restLocal = MMMat4( Matrix4f() );
	skel.joints.push_back( rj );

	MotionDatabase db;
	std::vector< MMClip > clips;
	clips.push_back( MMClip( (AnimationResource *)Modules::resMan().resolveResHandle( anim ), fps20 ) );
	std::vector< std::string > feat( 1, "root" );
	TASK_CHECK( db.build( skel, clips, feat, h ) );
	// Frames 1,2,3: t-delta >= 0 and t+h <= 0.25
	TASK_CHECK( db.getCandidateCount() == 3 );
	TASK_CHECK( nearF( db.getClipDuration( 0 ), 0.25f ) );

	RefClip ref;
	ref.fps = fps20; ref.frameCount = frames;
	ref.jointNames.push_back( "root" );
	ref.parents.push_back( -1 );
	ref.restLocal.push_back( Matrix4f() );
	ref.hasTrack.push_back( true );
	ref.localMats.resize( 1 );
	for( uint32 i = 0; i < frames; ++i )
		ref.localMats[0].push_back(
			RefMat::translation( 0.25f * (float)i, 0.0f, 0.0f ) * RefMat::yRotation( 2.8f + 0.2f * (float)i ) );
	std::vector< uint32 > rfIdx( 1, 0 );

	for( uint32 k = 0; k < 3; ++k )
	{
		const uint32 frame = k + 1;
		MMCandidateInfo info;
		TASK_CHECK( db.getCandidate( k, info ) );
		RefFeatures rf = refComputeFeatures( ref, rfIdx, frame, h );
		TASK_CHECK( nearF( info.futureYaw, rf.futureYaw, 2.0e-4f ) );
		// Yaw always stays in the principal range and the flip does not produce a jump of 2pi.
		TASK_CHECK( info.futureYaw >= -PI - 1.0e-4f && info.futureYaw <= PI + 1.0e-4f );
		TASK_CHECK( nearF( info.futureDisplacement.x, rf.futureDisp.x, 2.0e-4f ) );
	}

	// Two clips with different frame rates coexist in one database.
	H3DRes slow = buildWalkClip( "feat_slow.anim", 10.0f, 5, 0.0f, 0.5f, 2.0f, 0.5f );
	H3DRes fast = buildWalkClip( "feat_fast.anim", 30.0f, 9, 0.0f, 0.2f, 2.0f, 0.1f );
	MotionDatabase db2;
	std::vector< MMClip > two;
	two.push_back( MMClip( (AnimationResource *)Modules::resMan().resolveResHandle( slow ), 10.0f ) );
	two.push_back( MMClip( (AnimationResource *)Modules::resMan().resolveResHandle( fast ), 30.0f ) );
	TASK_CHECK( db2.build( makeSkeleton(), two, feat, 0.1f ) );
	TASK_CHECK( db2.getClipCount() == 2 );
	TASK_CHECK( nearF( db2.getClipDuration( 0 ), 0.4f ) );
	TASK_CHECK( nearF( db2.getClipDuration( 1 ), 8.0f / 30.0f ) );
	return 0;
}


static int features_unordered_skeleton_and_rest_branch()
{
	ensureEngine();
	// Joints are not parent-before-child and the side joint has no track at all.
	MMSkeleton skel = makeSkeleton( false );
	TASK_CHECK( skel.joints[0].name == "limb" );
	H3DRes anim = buildWalkClip( "feat_unord.anim", 10.0f, 5, 0.2f, 0.4f, 2.0f, 0.4f );

	MotionDatabase db;
	std::vector< MMClip > clips;
	clips.push_back( MMClip( (AnimationResource *)Modules::resMan().resolveResHandle( anim ), 10.0f ) );
	std::vector< std::string > feat;
	feat.push_back( "limb" );
	feat.push_back( "side" );
	TASK_CHECK( db.build( skel, clips, feat, 0.2f ) );

	RefClip ref = makeRefWalk( 10.0f, 5, 0.2f, 0.4f, 2.0f, 0.4f, skel );
	std::vector< uint32 > refFeat;
	refFeat.push_back( 0 );  // limb is index 0 in this ordering
	refFeat.push_back( 2 );  // side is index 2
	RefFeatures rf = refComputeFeatures( ref, refFeat, 1, 0.2f );

	MMCandidateInfo info = firstCandidate( db );
	TASK_CHECK( nearVec( Vec3f( info.positions[0].x, info.positions[0].y, info.positions[0].z ),
	                     rf.positions[0] ) );
	TASK_CHECK( nearVec( Vec3f( info.positions[1].x, info.positions[1].y, info.positions[1].z ),
	                     rf.positions[1] ) );
	// Rest side branch: its velocity matches the independent reference even though it has no
	// animation track of its own (the rotating root induces tangential root-frame velocity).
	TASK_CHECK( nearVec( Vec3f( info.velocities[1].x, info.velocities[1].y, info.velocities[1].z ),
	                     rf.velocities[1] ) );
	return 0;
}


static int features_database_owns_copied_data()
{
	ensureEngine();
	const float fps = 10.0f, h = 0.2f;
	H3DRes anim = buildWalkClip( "feat_life.anim", fps, 6, 0.2f, 0.4f, 2.0f, 0.3f );
	AnimationResource *res = (AnimationResource *)Modules::resMan().resolveResHandle( anim );

	MotionDatabase db;
	std::vector< MMClip > clips;
	clips.push_back( MMClip( res, fps ) );
	std::vector< std::string > feat( 1, "root" );
	TASK_CHECK( db.build( makeSkeleton(), clips, feat, h ) );

	MMCandidateInfo before;
	TASK_CHECK( db.getCandidate( 0, before ) );

	// Release the animation resource completely; the database keeps no reference to it.
	h3dRemoveResource( anim );
	h3dReleaseUnusedResources();
	TASK_CHECK( Modules::resMan().resolveResHandle( anim ) == 0x0 );

	// Search and candidate inspection still return the identical copied feature data.
	MMCandidateInfo after;
	TASK_CHECK( db.getCandidate( 0, after ) );
	TASK_CHECK( nearVec( Vec3f( after.positions[0].x, after.positions[0].y, after.positions[0].z ),
	                     Vec3f( before.positions[0].x, before.positions[0].y, before.positions[0].z ) ) );
	TASK_CHECK( nearF( after.futureYaw, before.futureYaw ) );
	MMSearchInput q = queryFromCandidate( after );
	MMSearchResult r;
	TASK_CHECK( db.search( q, r ) );

	// A player built afterwards works purely from the copied data and drives a real model.
	Vec3f basePos[3] = { Vec3f( 2, 2, 0 ), Vec3f( 1, 3, 0 ), Vec3f( 1, 2, 1 ) };
	Character ch = buildCharacter( "life", testInvBind(), basePos, 0.0f, Vec3f( 0.0f, 0.0f, 0.0f ) );
	MotionPlayer player;
	TASK_CHECK( player.init( db, ch.modelPtr, after.clipIndex, after.sourceTime,
	                         MMMat4( Matrix4f::TransMat( 1.0f, 0.0f, 1.0f ) ) ) );
	TASK_CHECK( player.advance( 0.1f, queryFromCandidate( after ) ) );
	const float *rootRel = 0x0;
	h3dGetNodeTransMats( ch.rootJoint, &rootRel, 0x0 );
	Matrix4f rootMat;
	std::memcpy( &rootMat.x[0], rootRel, sizeof( float ) * 16 );
	TASK_CHECK( nearMat( rootMat, Matrix4f(), 1.0e-5f ) );
	return 0;
}


// =================================================================================================
// Group: search
// =================================================================================================

static int search_limb_velocity_distinguishes_clips()
{
	ensureEngine();
	// Same root trajectory in both clips, different limb speeds.
	H3DRes slow = buildWalkClip( "srch_slow.anim", 10.0f, 5, 0.1f, 0.5f, 2.0f, 0.2f );
	H3DRes fast = buildWalkClip( "srch_fast.anim", 10.0f, 5, 0.1f, 0.5f, 2.0f, 0.8f );
	MotionDatabase db;
	std::vector< MMClip > clips;
	clips.push_back( MMClip( (AnimationResource *)Modules::resMan().resolveResHandle( slow ), 10.0f ) );
	clips.push_back( MMClip( (AnimationResource *)Modules::resMan().resolveResHandle( fast ), 10.0f ) );
	std::vector< std::string > feat( 1, "limb" );
	TASK_CHECK( db.build( makeSkeleton(), clips, feat, 0.2f ) );

	MMCandidateInfo slowC, fastC;
	TASK_CHECK( db.getCandidate( 0, slowC ) );
	TASK_CHECK( db.getCandidate( db.getCandidateCount() / 2, fastC ) );
	TASK_CHECK( fastC.clipIndex == 1 );
	// Root motion is shared so root positions would be identical; the limb velocities differ.
	TASK_CHECK( !nearVec( Vec3f( slowC.velocities[0].x, slowC.velocities[0].y, slowC.velocities[0].z ),
	                      Vec3f( fastC.velocities[0].x, fastC.velocities[0].y, fastC.velocities[0].z ) ) );

	// Query the fast limb velocity; only the velocity weight is active.
	MMSearchInput q;
	q.positions.push_back( MMVec3( 0.0f, 0.0f, 0.0f ) );
	q.velocities.push_back( MMVec3( fastC.velocities[0].x, fastC.velocities[0].y, fastC.velocities[0].z ) );
	q.weights = MMWeights( 0.0f, 1.0f, 0.0f, 0.0f );
	MMSearchResult r;
	TASK_CHECK( db.search( q, r ) );
	TASK_CHECK( r.clipIndex == 1 );

	// The slow limb velocity selects the first clip instead.
	q.velocities[0] = MMVec3( slowC.velocities[0].x, slowC.velocities[0].y, slowC.velocities[0].z );
	TASK_CHECK( db.search( q, r ) );
	TASK_CHECK( r.clipIndex == 0 );

	// With the position weight zero, arbitrary position data must not influence the velocity-only
	// result, proving selection is not driven by the nearest raw frame or root motion.
	MMSearchInput q2 = q;
	q2.positions[0] = MMVec3( 100.0f, 100.0f, 100.0f );
	q2.weights = MMWeights( 0.0f, 1.0f, 0.0f, 0.0f );
	TASK_CHECK( db.search( q2, r ) );
	TASK_CHECK( r.clipIndex == 0 );

	// The same query is deterministic across repeated calls.
	MMSearchResult rAgain;
	TASK_CHECK( db.search( q2, rAgain ) );
	TASK_CHECK( rAgain.clipIndex == r.clipIndex && nearF( rAgain.cost, r.cost ) &&
	            nearF( rAgain.sourceTime, r.sourceTime ) );
	return 0;
}


static int search_tie_break_order()
{
	ensureEngine();
	H3DRes a = buildWalkClip( "srch_tie_a.anim", 10.0f, 5, 0.1f, 0.5f, 2.0f, 0.4f );
	H3DRes b = buildWalkClip( "srch_tie_b.anim", 10.0f, 5, 0.1f, 0.5f, 2.0f, 0.4f );
	MotionDatabase db;
	std::vector< MMClip > clips;
	clips.push_back( MMClip( (AnimationResource *)Modules::resMan().resolveResHandle( a ), 10.0f ) );
	clips.push_back( MMClip( (AnimationResource *)Modules::resMan().resolveResHandle( b ), 10.0f ) );
	std::vector< std::string > feat( 1, "root" );
	TASK_CHECK( db.build( makeSkeleton(), clips, feat, 0.2f ) );
	const int perClip = db.getCandidateCount() / 2;
	TASK_CHECK( perClip == 2 );

	// A query far from every candidate gives equal costs everywhere: lowest indices win.
	MMSearchInput q;
	q.positions.push_back( MMVec3( 50.0f, 50.0f, 50.0f ) );
	q.velocities.push_back( MMVec3( 0.0f, 0.0f, 0.0f ) );
	q.weights = MMWeights( 1.0f, 0.0f, 0.0f, 0.0f );
	MMSearchResult r;
	TASK_CHECK( db.search( q, r ) );
	TASK_CHECK( r.clipIndex == 0 );
	MMCandidateInfo first;
	TASK_CHECK( db.getCandidate( 0, first ) );
	TASK_CHECK( nearF( r.sourceTime, first.sourceTime ) );

	// An exact match of the second stored candidate (frame 2 of clip 0). Because the two clips
	// are identical, this pose exists at zero cost in both clips; the tie break keeps clip 0 and
	// still returns the matched frame time.
	MMCandidateInfo target;
	TASK_CHECK( db.getCandidate( 1, target ) );
	TASK_CHECK( target.clipIndex == 0 );
	MMSearchInput exact;
	exact.positions.push_back( target.positions[0] );
	exact.velocities.push_back( target.velocities[0] );
	exact.futureDisplacement = target.futureDisplacement;
	exact.futureYaw = target.futureYaw;
	exact.weights = MMWeights( 1.0f, 1.0f, 1.0f, 1.0f );
	TASK_CHECK( db.search( exact, r ) );
	TASK_CHECK( r.clipIndex == 0 );
	TASK_CHECK( nearF( r.sourceTime, target.sourceTime ) );
	TASK_CHECK( nearF( r.cost, 0.0f, 1.0e-10f ) );

	// Candidate storage order is clip by clip and frame ascending.
	MMCandidateInfo order0, order1, order2, order3;
	TASK_CHECK( db.getCandidate( 0, order0 ) );
	TASK_CHECK( db.getCandidate( 1, order1 ) );
	TASK_CHECK( db.getCandidate( 2, order2 ) );
	TASK_CHECK( db.getCandidate( 3, order3 ) );
	TASK_CHECK( order0.clipIndex == 0 && order1.clipIndex == 0 &&
	            order2.clipIndex == 1 && order3.clipIndex == 1 );
	TASK_CHECK( order0.frame < order1.frame && order2.frame < order3.frame );
	return 0;
}


static int search_weights_and_reference_cost()
{
	ensureEngine();
	H3DRes anim = buildWalkClip( "srch_cost.anim", 10.0f, 6, 0.15f, 0.4f, 2.0f, 0.3f );
	MotionDatabase db;
	std::vector< MMClip > clips;
	clips.push_back( MMClip( (AnimationResource *)Modules::resMan().resolveResHandle( anim ), 10.0f ) );
	std::vector< std::string > feat;
	feat.push_back( "root" );
	feat.push_back( "limb" );
	TASK_CHECK( db.build( makeSkeleton(), clips, feat, 0.2f ) );

	MMCandidateInfo info = firstCandidate( db );
	MMSearchInput q;
	q.positions.resize( 2 );
	q.velocities.resize( 2 );
	for( uint32 j = 0; j < 2; ++j )
	{
		q.positions[j] = MMVec3( info.positions[j].x + 0.1f, info.positions[j].y - 0.2f,
		                         info.positions[j].z + 0.3f );
		q.velocities[j] = MMVec3( info.velocities[j].x - 0.4f, info.velocities[j].y + 0.5f,
		                          info.velocities[j].z );
	}
	q.futureDisplacement = MMVec3( info.futureDisplacement.x + 0.6f, 0.0f,
	                               info.futureDisplacement.z - 0.7f );
	q.futureYaw = info.futureYaw + 0.35f;
	q.weights = MMWeights( 0.7f, 1.3f, 2.1f, 0.9f );

	// The reference cost of the queried candidate, computed independently.
	const float expectedInfoCost = refCost( info, q );
	MMSearchResult r;
	TASK_CHECK( db.search( q, r ) );
	TASK_CHECK( nearF( refCost( info, q ), expectedInfoCost ) );

	// Enumerate every candidate and verify the reported winner really has the minimum cost.
	float minCost = Math::MaxFloat;
	for( int i = 0; i < db.getCandidateCount(); ++i )
	{
		MMCandidateInfo ci;
		TASK_CHECK( db.getCandidate( i, ci ) );
		minCost = std::min( minCost, refCost( ci, q ) );
	}
	TASK_CHECK( nearF( r.cost, minCost, 1.0e-3f ) );

	// Disabling the trajectory/facing weights changes the cost accordingly.
	q.weights = MMWeights( 0.7f, 1.3f, 0.0f, 0.0f );
	TASK_CHECK( db.search( q, r ) );
	TASK_CHECK( nearF( r.cost, refCost( info, q ), 1.0e-3f ) );
	return 0;
}


// =================================================================================================
// Group: handoff
// =================================================================================================

static MMSearchInput queryFromCandidate( const MMCandidateInfo &c )
{
	// The player overwrites positions and velocities with the actual pose, so player-driven
	// selections are forced through the future trajectory and facing only.
	MMSearchInput q;
	q.positions = c.positions;
	q.velocities = c.velocities;
	q.futureDisplacement = c.futureDisplacement;
	q.futureYaw = c.futureYaw;
	q.weights = MMWeights( 0.0f, 0.0f, 1.0f, 1.0f );
	return q;
}


static int handoff_alignment_and_turn()
{
	ensureEngine();
	const float fps = 10.0f, h = 0.2f;
	H3DRes a = buildWalkClip( "hand_a.anim", fps, 6, 0.25f, 0.4f, 2.0f, 0.3f );
	H3DRes b = buildWalkClip( "hand_b.anim", fps, 6, -0.2f, 0.3f, 2.0f, 0.5f );
	MotionDatabase db;
	std::vector< MMClip > clips;
	clips.push_back( MMClip( (AnimationResource *)Modules::resMan().resolveResHandle( a ), fps ) );
	clips.push_back( MMClip( (AnimationResource *)Modules::resMan().resolveResHandle( b ), fps ) );
	std::vector< std::string > feat( 1, "root" );
	TASK_CHECK( db.build( makeSkeleton(), clips, feat, h ) );

	Vec3f basePos[3] = { Vec3f( 2, 2, 0 ), Vec3f( 1, 3, 0 ), Vec3f( 1, 2, 1 ) };
	Character ch = buildCharacter( "hand", testInvBind(), basePos, 0.0f, Vec3f( 0.0f, 0.0f, 0.0f ) );

	// Start at the second legal candidate (a non-zero starting point).
	MMCandidateInfo start;
	TASK_CHECK( db.getCandidate( 1, start ) );
	TASK_CHECK( start.clipIndex == 0 );

	// World root includes a real 90-degree yaw so translation cannot be faked component-wise.
	Matrix4f worldRoot = Matrix4f::TransMat( 3.0f, 0.0f, -2.0f );
	worldRoot.rotate( 0.0f, PI * 0.5f, 0.0f );

	MotionPlayer player;
	TASK_CHECK( player.init( db, ch.modelPtr, start.clipIndex, start.sourceTime, MMMat4( worldRoot ) ) );
	TASK_CHECK( player.getCurrentClipIndex() == 0 );

	// The root joint carries no root motion after the pose is applied.
	const float *rel = 0x0;
	h3dGetNodeTransMats( ch.rootJoint, &rel, 0x0 );
	Matrix4f rootRel;
	std::memcpy( &rootRel.x[0], rel, sizeof( float ) * 16 );
	TASK_CHECK( nearMat( rootRel, Matrix4f(), 1.0e-5f ) );

	const MMMat4 worldBefore = player.getWorldRoot();

	// Force a switch into clip 1 by querying its first candidate.
	MMCandidateInfo switchTarget;
	TASK_CHECK( db.getCandidate( db.getCandidateCount() / 2, switchTarget ) );
	TASK_CHECK( switchTarget.clipIndex == 1 );
	MMSearchInput q = queryFromCandidate( switchTarget );

	const float dt = 0.1f;
	TASK_CHECK( player.advance( dt, q ) );
	TASK_CHECK( player.getCurrentClipIndex() == 1 );

	// World root continuity: the advanced old root must equal the new world root exactly.
	// Alignment is W * inverse(G_start), so the advanced world root is
	// W * inverse(G_start) * G_old(t0 + dt).
	RefClip refOld = makeRefWalk( fps, 6, 0.25f, 0.4f, 2.0f, 0.3f, makeSkeleton() );
	std::vector< RefMat > startModels = refModelMats( refOld, start.sourceTime );
	std::vector< RefMat > oldModels = refModelMats( refOld, start.sourceTime + dt );
	RefMat expectedWorld =
		toRef( worldRoot ) * refInverseRigid( startModels[0] ) * oldModels[0];
	MMMat4 actualWorld = player.getWorldRoot();
	TASK_CHECK( nearMat( actualWorld.m, Matrix4f( expectedWorld.m ), 2.0e-4f ) );

	// Rotation is genuinely applied through matrix composition: the world orientation is the
	// 90-degree placement yaw plus the relative clip yaw since the start, never a component-wise
	// translation hack.
	const float startYaw = refYawOf( startModels[0] );
	const float advancedYaw = refYawOf( oldModels[0] );
	const float expectedWorldYaw = PI * 0.5f + ( advancedYaw - startYaw );
	const float *modelAbs = 0x0;
	h3dGetNodeTransMats( ch.model, 0x0, &modelAbs );
	// Flat element x[0] is cos(yaw) while x[2] (c[0][2]) is -sin(yaw).
	TASK_CHECK( nearF( modelAbs[0], std::cos( expectedWorldYaw ), 2.0e-4f ) );
	TASK_CHECK( nearF( modelAbs[2], -std::sin( expectedWorldYaw ), 2.0e-4f ) );
	(void)worldBefore;
	return 0;
}


static int handoff_parent_rotation()
{
	ensureEngine();
	const float fps = 10.0f, h = 0.2f;
	H3DRes anim = buildWalkClip( "parent.anim", fps, 6, 0.2f, 0.4f, 2.0f, 0.3f );
	MotionDatabase db;
	std::vector< MMClip > clips;
	clips.push_back( MMClip( (AnimationResource *)Modules::resMan().resolveResHandle( anim ), fps ) );
	std::vector< std::string > feat( 1, "root" );
	TASK_CHECK( db.build( makeSkeleton(), clips, feat, h ) );

	Vec3f basePos[3] = { Vec3f( 2, 2, 0 ), Vec3f( 1, 3, 0 ), Vec3f( 1, 2, 1 ) };
	Character ch = buildCharacter( "par", testInvBind(), basePos, 30.0f, Vec3f( 7.0f, 0.0f, -3.0f ) );

	MMCandidateInfo start = firstCandidate( db );
	Matrix4f worldRoot = Matrix4f::TransMat( 2.0f, 0.0f, 1.0f );
	worldRoot.rotate( 0.0f, 0.4f, 0.0f );

	MotionPlayer player;
	TASK_CHECK( player.init( db, ch.modelPtr, start.clipIndex, start.sourceTime, MMMat4( worldRoot ) ) );

	// The model's relative transform is worldRoot converted into the (rotated) parent space.
	const float *parentAbs = 0x0, *modelRel = 0x0, *modelAbs = 0x0;
	h3dGetNodeTransMats( ch.parent, 0x0, &parentAbs );
	h3dGetNodeTransMats( ch.model, &modelRel, &modelAbs );
	Matrix4f parentMat, relMat, absMat;
	std::memcpy( &parentMat.x[0], parentAbs, sizeof( float ) * 16 );
	std::memcpy( &relMat.x[0], modelRel, sizeof( float ) * 16 );
	std::memcpy( &absMat.x[0], modelAbs, sizeof( float ) * 16 );
	TASK_CHECK( nearMat( parentMat * relMat, absMat, 2.0e-4f ) );
	TASK_CHECK( nearMat( absMat, worldRoot, 2.0e-4f ) );
	TASK_CHECK( nearMat( relMat, parentMat.inverted() * worldRoot, 2.0e-4f ) );
	return 0;
}


static int handoff_dt_zero_reselect()
{
	ensureEngine();
	const float fps = 10.0f, h = 0.2f;
	H3DRes a = buildWalkClip( "z_a.anim", fps, 6, 0.2f, 0.4f, 2.0f, 0.3f );
	H3DRes b = buildWalkClip( "z_b.anim", fps, 6, 0.3f, 0.5f, 2.0f, 0.6f );
	MotionDatabase db;
	std::vector< MMClip > clips;
	clips.push_back( MMClip( (AnimationResource *)Modules::resMan().resolveResHandle( a ), fps ) );
	clips.push_back( MMClip( (AnimationResource *)Modules::resMan().resolveResHandle( b ), fps ) );
	std::vector< std::string > feat( 1, "root" );
	TASK_CHECK( db.build( makeSkeleton(), clips, feat, h ) );

	Vec3f basePos[3] = { Vec3f( 2, 2, 0 ), Vec3f( 1, 3, 0 ), Vec3f( 1, 2, 1 ) };
	Character ch = buildCharacter( "zero", testInvBind(), basePos, 0.0f, Vec3f( 0.0f, 0.0f, 0.0f ) );

	MMCandidateInfo start = firstCandidate( db );
	Matrix4f worldRoot = Matrix4f::TransMat( 1.0f, 0.0f, 2.0f );
	MotionPlayer player;
	TASK_CHECK( player.init( db, ch.modelPtr, start.clipIndex, start.sourceTime, MMMat4( worldRoot ) ) );
	const MMMat4 worldBefore = player.getWorldRoot();

	MMCandidateInfo target;
	TASK_CHECK( db.getCandidate( db.getCandidateCount() / 2, target ) );
	MMSearchInput q = queryFromCandidate( target );

	// dt == 0 still performs a re-selection.
	TASK_CHECK( player.advance( 0.0f, q ) );
	TASK_CHECK( player.getCurrentClipIndex() == 1 );
	TASK_CHECK( nearMat( player.getWorldRoot().m, worldBefore.m, 2.0e-5f ) );
	return 0;
}


// =================================================================================================
// Group: integration
// =================================================================================================

static int integration_world_vertices_and_classic_path()
{
	ensureEngine();
	const float fps = 10.0f, h = 0.2f;
	H3DRes anim = buildWalkClip( "int.anim", fps, 6, 0.3f, 0.5f, 2.0f, 0.4f );
	MotionDatabase db;
	std::vector< MMClip > clips;
	clips.push_back( MMClip( (AnimationResource *)Modules::resMan().resolveResHandle( anim ), fps ) );
	std::vector< std::string > feat;
	feat.push_back( "root" );
	feat.push_back( "limb" );
	TASK_CHECK( db.build( makeSkeleton(), clips, feat, h ) );

	Vec3f basePos[3] = { Vec3f( 2, 2, 0 ), Vec3f( 1, 3, 0 ), Vec3f( 1, 2, 1 ) };
	Character ch = buildCharacter( "intg", testInvBind(), basePos, 30.0f, Vec3f( 7.0f, 0.0f, -3.0f ) );

	MMCandidateInfo start = firstCandidate( db );
	Matrix4f worldRoot = Matrix4f::TransMat( 2.0f, 0.0f, 1.0f );
	worldRoot.rotate( 0.0f, 0.5f, 0.0f );
	MotionPlayer player;
	TASK_CHECK( player.init( db, ch.modelPtr, start.clipIndex, start.sourceTime, MMMat4( worldRoot ) ) );

	// A morpher continues to work through the player-driven software skinning path.
	TASK_CHECK( h3dSetModelMorpher( ch.model, "lift", 0.25f ) );

	MMCandidateInfo next;
	TASK_CHECK( db.getCandidate( 1, next ) );
	MMSearchInput q = queryFromCandidate( next );
	TASK_CHECK( player.advance( 0.1f, q ) );

	// Independent reference for the final skinned world vertices.
	const float t = player.getCurrentTime();
	RefClip ref = makeRefWalk( fps, 6, 0.3f, 0.5f, 2.0f, 0.4f, makeSkeleton() );
	std::vector< RefMat > models = refModelMats( ref, t );
	// The player strips root motion from the root joint: root local is identity.
	RefMat rootRel;
	RefMat limbRel = refSampleLocal( ref, 1, t );
	RefMat relModelLimb = rootRel * limbRel;
	RefMat skinRef = relModelLimb * toRef( testInvBind() );

	const float *parentAbsF = 0x0, *modelAbsF = 0x0;
	h3dGetNodeTransMats( ch.parent, 0x0, &parentAbsF );
	h3dGetNodeTransMats( ch.model, 0x0, &modelAbsF );
	Matrix4f parentAbs, modelAbs;
	std::memcpy( &parentAbs.x[0], parentAbsF, sizeof( float ) * 16 );
	std::memcpy( &modelAbs.x[0], modelAbsF, sizeof( float ) * 16 );

	GeometryResource *geo = ch.modelPtr->getGeometryResource();
	for( int v = 0; v < 3; ++v )
	{
		Vec3f base = basePos[v];
		if( v == 0 ) base = base + Vec3f( 0.0f, 0.25f, 0.0f );  // Morph contribution
		Vec3f skinned = refTransform( skinRef, base );
		Vec3f expectedWorld = refTransform( toRef( modelAbs ), skinned );

		Vec3f actualSkinned = geo->getVertPosData()[v];
		TASK_CHECK( nearVec( actualSkinned, skinned, 2.0e-4f ) );
		Vec3f actualWorld = modelAbs * actualSkinned;
		TASK_CHECK( nearVec( actualWorld, expectedWorld, 2.0e-4f ) );
		// Parent transform is part of the chain.
		Vec3f viaParent = parentAbs * ( parentAbs.inverted() * modelAbs ) * actualSkinned;
		TASK_CHECK( nearVec( viaParent, expectedWorld, 2.0e-4f ) );
	}

	// Classic animation stages, morphers and ordinary model playback still work on a second model.
	Character classic = buildCharacter( "classic", testInvBind(), basePos, 0.0f, Vec3f( 0.0f, 0.0f, 0.0f ) );
	h3dSetupModelAnimStage( classic.model, 0, anim, 0, "", false );
	h3dSetModelMorpher( classic.model, "lift", 0.25f );
	h3dSetModelAnimParams( classic.model, 0, 1.5f, 1.0f );
	h3dUpdateModel( classic.model, H3DModelUpdateFlags::Animation | H3DModelUpdateFlags::Geometry );
	const float *limbAbs = 0x0;
	h3dGetNodeTransMats( classic.limbJoint, 0x0, &limbAbs );
	// This clip uses limbBase 2.0 and limbStep 0.4; a Y rotation leaves the Y component intact.
	TASK_CHECK( nearF( limbAbs[13], 2.0f + 0.4f * 1.5f ) );
	return 0;
}


static int integration_interleaved_and_recovery()
{
	ensureEngine();
	const float fps = 10.0f, h = 0.2f;
	H3DRes a = buildWalkClip( "il_a.anim", fps, 7, 0.1f, 0.4f, 2.0f, 0.3f );
	H3DRes b = buildWalkClip( "il_b.anim", fps, 7, 0.2f, 0.5f, 2.0f, 0.4f );
	MotionDatabase db;
	std::vector< MMClip > clips;
	clips.push_back( MMClip( (AnimationResource *)Modules::resMan().resolveResHandle( a ), fps ) );
	clips.push_back( MMClip( (AnimationResource *)Modules::resMan().resolveResHandle( b ), fps ) );
	std::vector< std::string > feat( 1, "root" );
	TASK_CHECK( db.build( makeSkeleton(), clips, feat, h ) );

	Vec3f basePos[3] = { Vec3f( 2, 2, 0 ), Vec3f( 1, 3, 0 ), Vec3f( 1, 2, 1 ) };
	Character one = buildCharacter( "il1", testInvBind(), basePos, 0.0f, Vec3f( 0.0f, 0.0f, 0.0f ) );
	Character two = buildCharacter( "il2", testInvBind(), basePos, 0.0f, Vec3f( 10.0f, 0.0f, 10.0f ) );

	MMCandidateInfo start = firstCandidate( db );
	MotionPlayer p1, p2;
	TASK_CHECK( p1.init( db, one.modelPtr, start.clipIndex, start.sourceTime,
	                     MMMat4( Matrix4f::TransMat( 0.0f, 0.0f, 0.0f ) ) ) );
	TASK_CHECK( p2.init( db, two.modelPtr, start.clipIndex, start.sourceTime,
	                     MMMat4( Matrix4f::TransMat( 5.0f, 0.0f, 5.0f ) ) ) );

	MMCandidateInfo t1, t2;
	TASK_CHECK( db.getCandidate( 1, t1 ) );
	TASK_CHECK( db.getCandidate( db.getCandidateCount() / 2, t2 ) );
	MMSearchInput q1 = queryFromCandidate( t1 );
	MMSearchInput q2 = queryFromCandidate( t2 );

	const float time1 = p1.getCurrentTime();
	TASK_CHECK( p1.advance( 0.1f, q1 ) );
	TASK_CHECK( p2.advance( 0.1f, q2 ) );
	TASK_CHECK( p2.getCurrentClipIndex() == 1 );
	TASK_CHECK( p1.advance( 0.1f, q1 ) );

	// The players keep independent states. Re-selection snaps the source time to the matched
	// candidate rather than accumulating time: p1 stays on clip 0 at t1's frame while p2 runs
	// clip 1, and their world roots stay distinct because of their separate placements.
	TASK_CHECK( p1.getCurrentClipIndex() == 0 );
	TASK_CHECK( nearF( p1.getCurrentTime(), t1.sourceTime, 2.0e-5f ) );
	TASK_CHECK( p2.getCurrentClipIndex() == 1 );
	TASK_CHECK( nearF( p2.getCurrentTime(), t2.sourceTime, 2.0e-5f ) );
	TASK_CHECK( !nearMat( p1.getWorldRoot().m, p2.getWorldRoot().m ) );
	(void)time1;

	// p2 is driven past its clip interval: the call must fail and change nothing.
	const float p2Time = p2.getCurrentTime();
	const MMMat4 p2World = p2.getWorldRoot();
	MMSearchInput bad = q2;
	bad.futureYaw = 0.0f;
	TASK_CHECK( !p2.advance( 10.0f, bad ) );
	TASK_CHECK( nearF( p2.getCurrentTime(), p2Time ) );
	TASK_CHECK( nearMat( p2.getWorldRoot().m, p2World.m, 2.0e-5f ) );

	// The other player keeps working, and p2 can still take a legal step.
	TASK_CHECK( p1.advance( 0.1f, q1 ) );
	TASK_CHECK( p2.advance( 0.0f, q2 ) );
	return 0;
}


// =================================================================================================
// Group: invalid
// =================================================================================================

static int invalid_rejections()
{
	ensureEngine();
	const float fps = 10.0f;
	H3DRes good = buildWalkClip( "inv_good.anim", fps, 6, 0.1f, 0.4f, 2.0f, 0.3f );
	AnimationResource *goodRes = (AnimationResource *)Modules::resMan().resolveResHandle( good );
	std::vector< std::string > feat( 1, "root" );
	MMWeights w( 1, 1, 1, 1 );

	// Duplicate joint name
	{
		MMSkeleton s = makeSkeleton();
		s.joints[2].name = "root";
		MotionDatabase db;
		std::vector< MMClip > clips( 1, MMClip( goodRes, fps ) );
		TASK_CHECK( !db.build( s, clips, feat, 0.2f ) );
		TASK_CHECK( !db.isBuilt() );
	}
	// Parent cycle
	{
		MMSkeleton s;
		MMSkeletonJoint a, b;
		a.name = "a"; a.parentIndex = 1; a.restLocal = MMMat4( Matrix4f() );
		b.name = "b"; b.parentIndex = 0; b.restLocal = MMMat4( Matrix4f() );
		s.joints.push_back( a ); s.joints.push_back( b );
		MotionDatabase db;
		std::vector< MMClip > clips( 1, MMClip( goodRes, fps ) );
		TASK_CHECK( !db.build( s, clips, feat, 0.2f ) );
	}
	// Unknown feature joint
	{
		MotionDatabase db;
		std::vector< MMClip > clips( 1, MMClip( goodRes, fps ) );
		std::vector< std::string > bad( 1, "ghost" );
		TASK_CHECK( !db.build( makeSkeleton(), clips, bad, 0.2f ) );
	}
	// Unknown track in the animation resource
	{
		AnimBuilder b;
		b.begin( 2, 3 );
		std::vector< AnimFrame > r, g;
		for( uint32 i = 0; i < 3; ++i )
		{
			r.push_back( rootFrame( 0.1f * (float)i, 0.3f * (float)i ) );
			g.push_back( limbFrame( Vec3f( 0.0f, 2.0f, 0.0f ) ) );
		}
		b.entity( "root", r );
		b.entity( "ghost", g );
		H3DRes badAnim = b.load( "inv_unknown.anim" );
		MotionDatabase db;
		std::vector< MMClip > clips( 1,
			MMClip( (AnimationResource *)Modules::resMan().resolveResHandle( badAnim ), fps ) );
		TASK_CHECK( !db.build( makeSkeleton(), clips, feat, 0.2f ) );
	}
	// Illegal root transforms: Y translation and a non-Y rotation
	{
		AnimBuilder b;
		b.begin( 1, 3 );
		std::vector< AnimFrame > r;
		for( uint32 i = 0; i < 3; ++i )
		{
			AnimFrame f = rootFrame( 0.0f, 0.3f * (float)i );
			f.t.y = 0.5f;
			r.push_back( f );
		}
		b.entity( "root", r );
		H3DRes badAnim = b.load( "inv_rooty.anim" );
		MotionDatabase db;
		std::vector< MMClip > clips( 1,
			MMClip( (AnimationResource *)Modules::resMan().resolveResHandle( badAnim ), fps ) );
		TASK_CHECK( !db.build( makeSkeleton(), clips, feat, 0.2f ) );
	}
	{
		AnimBuilder b;
		b.begin( 1, 3 );
		std::vector< AnimFrame > r;
		for( uint32 i = 0; i < 3; ++i )
		{
			AnimFrame f;
			f.q.x = std::sin( 0.2f ); f.q.y = 0.0f; f.q.z = 0.0f; f.q.w = std::cos( 0.2f );
			f.t = Vec3f( 0.3f * (float)i, 0.0f, 0.0f );
			r.push_back( f );
		}
		b.entity( "root", r );
		H3DRes badAnim = b.load( "inv_roottilt.anim" );
		MotionDatabase db;
		std::vector< MMClip > clips( 1,
			MMClip( (AnimationResource *)Modules::resMan().resolveResHandle( badAnim ), fps ) );
		TASK_CHECK( !db.build( makeSkeleton(), clips, feat, 0.2f ) );
	}
	// Non-finite animation data
	{
		AnimBuilder b;
		b.begin( 1, 3 );
		std::vector< AnimFrame > r;
		for( uint32 i = 0; i < 3; ++i )
			r.push_back( rootFrame( 0.0f, i == 1 ? Math::NaN : 0.3f * (float)i ) );
		b.entity( "root", r );
		H3DRes badAnim = b.load( "inv_nan.anim" );
		MotionDatabase db;
		std::vector< MMClip > clips( 1,
			MMClip( (AnimationResource *)Modules::resMan().resolveResHandle( badAnim ), fps ) );
		TASK_CHECK( !db.build( makeSkeleton(), clips, feat, 0.2f ) );
	}
	// Horizon too large: no legal candidate
	{
		MotionDatabase db;
		std::vector< MMClip > clips( 1, MMClip( goodRes, fps ) );
		TASK_CHECK( !db.build( makeSkeleton(), clips, feat, 5.0f ) );
	}
	// Bad fps / horizon / null resource
	{
		MotionDatabase db;
		std::vector< MMClip > clips( 1, MMClip( goodRes, 0.0f ) );
		TASK_CHECK( !db.build( makeSkeleton(), clips, feat, 0.2f ) );
		std::vector< MMClip > clips2( 1, MMClip( goodRes, -1.0f ) );
		TASK_CHECK( !db.build( makeSkeleton(), clips2, feat, 0.2f ) );
		std::vector< MMClip > clips3( 1, MMClip( 0x0, fps ) );
		TASK_CHECK( !db.build( makeSkeleton(), clips3, feat, 0.2f ) );
		MotionDatabase db4;
		std::vector< MMClip > clips4( 1, MMClip( goodRes, fps ) );
		TASK_CHECK( !db4.build( makeSkeleton(), clips4, feat, 0.0f ) );
	}

	// A good database for the search/player failure cases.
	MotionDatabase db;
	std::vector< MMClip > clips( 1, MMClip( goodRes, fps ) );
	TASK_CHECK( db.build( makeSkeleton(), clips, feat, 0.2f ) );

	MMSearchInput q;
	q.positions.push_back( MMVec3( 0, 0, 0 ) );
	q.velocities.push_back( MMVec3( 0, 0, 0 ) );
	q.weights = w;
	MMSearchResult r;
	// All-zero weights
	MMSearchInput zeroW = q;
	zeroW.weights = MMWeights( 0, 0, 0, 0 );
	TASK_CHECK( !db.search( zeroW, r ) );
	// Negative weight
	MMSearchInput negW = q;
	negW.weights = MMWeights( -1, 1, 1, 1 );
	TASK_CHECK( !db.search( negW, r ) );
	// Size mismatch
	MMSearchInput badSize;
	badSize.positions.push_back( MMVec3( 0, 0, 0 ) );
	badSize.positions.push_back( MMVec3( 0, 0, 0 ) );
	badSize.velocities = q.velocities;
	badSize.weights = w;
	TASK_CHECK( !db.search( badSize, r ) );
	// Non-finite query
	MMSearchInput nanQ = q;
	nanQ.futureYaw = Math::NaN;
	TASK_CHECK( !db.search( nanQ, r ) );
	// Search on an unbuilt database and untouched result
	MMSearchResult untouched;
	untouched.clipIndex = 77;
	MotionDatabase unbuilt;
	TASK_CHECK( !unbuilt.search( q, untouched ) );
	TASK_CHECK( untouched.clipIndex == 77 );

	// Player init failures: bad candidate, missing joint.
	Vec3f basePos[3] = { Vec3f( 2, 2, 0 ), Vec3f( 1, 3, 0 ), Vec3f( 1, 2, 1 ) };
	Character ch = buildCharacter( "inv", testInvBind(), basePos, 0.0f, Vec3f( 0, 0, 0 ) );
	MotionPlayer player;
	TASK_CHECK( !player.init( db, ch.modelPtr, 0, 0.0f, MMMat4( Matrix4f() ) ) );  // Frame 0 is not a candidate
	TASK_CHECK( !player.isActive() );

	Character missing = buildCharacter( "invm", testInvBind(), basePos, 0.0f, Vec3f( 0, 0, 0 ) );
	MMCandidateInfo start = firstCandidate( db );
	TASK_CHECK( !player.init( db, missing.modelPtr, 99, start.sourceTime, MMMat4( Matrix4f() ) ) );

	// Advance on an inactive player fails.
	TASK_CHECK( !player.advance( 0.1f, q ) );
	return 0;
}


static int invalid_failure_leaves_state_intact()
{
	ensureEngine();
	const float fps = 10.0f, h = 0.2f;
	H3DRes anim = buildWalkClip( "fail.anim", fps, 7, 0.15f, 0.4f, 2.0f, 0.3f );
	MotionDatabase db;
	std::vector< MMClip > clips;
	clips.push_back( MMClip( (AnimationResource *)Modules::resMan().resolveResHandle( anim ), fps ) );
	std::vector< std::string > feat;
	feat.push_back( "root" );
	feat.push_back( "limb" );
	TASK_CHECK( db.build( makeSkeleton(), clips, feat, h ) );

	Vec3f basePos[3] = { Vec3f( 2, 2, 0 ), Vec3f( 1, 3, 0 ), Vec3f( 1, 2, 1 ) };
	Character ch = buildCharacter( "fl", testInvBind(), basePos, 15.0f, Vec3f( 1.0f, 0.0f, 2.0f ) );

	MMCandidateInfo start = firstCandidate( db );
	Matrix4f worldRoot = Matrix4f::TransMat( 2.0f, 0.0f, 1.0f );
	MotionPlayer player;
	TASK_CHECK( player.init( db, ch.modelPtr, start.clipIndex, start.sourceTime, MMMat4( worldRoot ) ) );

	const float savedTime = player.getCurrentTime();
	const MMMat4 savedAlign = player.getAlignment();
	const float *rootRelBefore = 0x0, *limbRelBefore = 0x0;
	h3dGetNodeTransMats( ch.rootJoint, &rootRelBefore, 0x0 );
	h3dGetNodeTransMats( ch.limbJoint, &limbRelBefore, 0x0 );
	Matrix4f beforeRoot, beforeLimb;
	std::memcpy( &beforeRoot.x[0], rootRelBefore, sizeof( float ) * 16 );
	std::memcpy( &beforeLimb.x[0], limbRelBefore, sizeof( float ) * 16 );

	// An over-long step fails; the player, alignment and model joints must be untouched.
	MMCandidateInfo next;
	TASK_CHECK( db.getCandidate( 1, next ) );
	MMSearchInput q = queryFromCandidate( next );
	TASK_CHECK( !player.advance( 5.0f, q ) );
	TASK_CHECK( nearF( player.getCurrentTime(), savedTime ) );
	TASK_CHECK( nearMat( player.getAlignment().m, savedAlign.m, 2.0e-6f ) );

	const float *rootRelAfter = 0x0, *limbRelAfter = 0x0;
	h3dGetNodeTransMats( ch.rootJoint, &rootRelAfter, 0x0 );
	h3dGetNodeTransMats( ch.limbJoint, &limbRelAfter, 0x0 );
	Matrix4f afterRoot, afterLimb;
	std::memcpy( &afterRoot.x[0], rootRelAfter, sizeof( float ) * 16 );
	std::memcpy( &afterLimb.x[0], limbRelAfter, sizeof( float ) * 16 );
	TASK_CHECK( nearMat( afterRoot, beforeRoot, 2.0e-6f ) );
	TASK_CHECK( nearMat( afterLimb, beforeLimb, 2.0e-6f ) );

	// A failed search does not poison the player and a subsequent legal step succeeds.
	MMSearchInput badWeights = q;
	badWeights.weights = MMWeights( 0, 0, 0, 0 );
	TASK_CHECK( !player.advance( 0.1f, badWeights ) );
	TASK_CHECK( nearF( player.getCurrentTime(), savedTime ) );
	TASK_CHECK( player.advance( 0.1f, q ) );
	TASK_CHECK( player.getCurrentTime() > savedTime );

	// Failed init on a second character leaves its model transform at the default identity.
	Character other = buildCharacter( "flo", testInvBind(), basePos, 0.0f, Vec3f( 0, 0, 0 ) );
	MotionPlayer stray;
	TASK_CHECK( !stray.init( db, other.modelPtr, 0, 0.0f, MMMat4( Matrix4f() ) ) );
	const float *otherRel = 0x0;
	h3dGetNodeTransMats( other.model, &otherRel, 0x0 );
	Matrix4f otherMat;
	std::memcpy( &otherMat.x[0], otherRel, sizeof( float ) * 16 );
	TASK_CHECK( nearMat( otherMat, Matrix4f(), 2.0e-6f ) );
	return 0;
}


// =================================================================================================
// Registration
// =================================================================================================

const task_case task_cases[] =
{
	{ "features", "basic_feature_values",        features_basic },
	{ "features", "pi_crossing_and_fps",         features_pi_crossing_and_fps },
	{ "features", "unordered_skeleton_rest",     features_unordered_skeleton_and_rest_branch },
	{ "features", "database_owns_copied_data",   features_database_owns_copied_data },
	{ "search",   "limb_velocity_discriminates", search_limb_velocity_distinguishes_clips },
	{ "search",   "tie_break_order",             search_tie_break_order },
	{ "search",   "weights_and_reference_cost",  search_weights_and_reference_cost },
	{ "handoff",  "alignment_and_turn",          handoff_alignment_and_turn },
	{ "handoff",  "parent_rotation",             handoff_parent_rotation },
	{ "handoff",  "dt_zero_reselect",            handoff_dt_zero_reselect },
	{ "integration", "world_vertices_classic",   integration_world_vertices_and_classic_path },
	{ "integration", "interleaved_recovery",     integration_interleaved_and_recovery },
	{ "invalid",  "rejections",                  invalid_rejections },
	{ "invalid",  "state_intact_after_failure",  invalid_failure_leaves_state_intact },
};

const size_t task_case_count = sizeof( task_cases ) / sizeof( task_cases[0] );
