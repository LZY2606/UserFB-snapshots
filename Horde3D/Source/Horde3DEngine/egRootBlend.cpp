// *************************************************************************************************
//
// Horde3D
//   Next-Generation Graphics Engine
// --------------------------------------
// Copyright (C) 2006-2021 Nicolas Schulz and Horde3D team
//
// This software is distributed under the terms of the Eclipse Public License v1.0.
// A copy of the license may be obtained at: http://www.eclipse.org/legal/epl-v10.html
//
// *************************************************************************************************

#include "egRootBlend.h"
#include "egAnimation.h"
#include "egModel.h"
#include "egAnimatables.h"
#include "egScene.h"
#include "egGeometry.h"
#include "egModules.h"
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <vector>


namespace Horde3D {

using namespace std;

namespace
{
	const double MaxQueryTime = 30.0;
	const double IntegratorStep = 1.0 / 2048.0;
	const float RigidEps = 1e-4f;


	static bool finiteF( float v ) { return std::isfinite( (double)v ); }
	static bool finiteD( double v ) { return std::isfinite( v ); }


	static double wrapPi( double a )
	{
		static const double twoPi = 6.28318530717958647692;
		while( a > 3.14159265358979323846 ) a -= twoPi;
		while( a <= -3.14159265358979323846 ) a += twoPi;
		return a;
	}


	// A matrix is accepted as rigid when its last row is (0,0,0,1) and its first three
	// columns form an orthonormal basis. Non-uniform or any other scale makes this fail.
	static bool isRigidMatrix( const Matrix4f &m )
	{
		for( int i = 0; i < 16; ++i ) if( !finiteF( m.x[i] ) ) return false;
		if( fabsf( m.x[3] ) > RigidEps || fabsf( m.x[7] ) > RigidEps ||
		    fabsf( m.x[11] ) > RigidEps || fabsf( m.x[15] - 1.0f ) > RigidEps ) return false;

		Vec3f col[3] =
		{
			Vec3f( m.x[0], m.x[1], m.x[2] ),
			Vec3f( m.x[4], m.x[5], m.x[6] ),
			Vec3f( m.x[8], m.x[9], m.x[10] )
		};
		for( int i = 0; i < 3; ++i )
			if( fabsf( col[i].length() - 1.0f ) > RigidEps ) return false;
		for( int i = 0; i < 3; ++i )
			for( int j = i + 1; j < 3; ++j )
				if( fabsf( col[i].dot( col[j] ) ) > RigidEps ) return false;
		// A proper rigid transform preserves handedness (reflections are not rigid).
		Vec3f cross = col[0].cross( col[1] );
		if( fabsf( cross.dot( col[2] ) - 1.0f ) > RigidEps ) return false;
		return true;
	}


	// The integrated root can only represent planar poses: a rigid transform whose rotation is
	// about the Y axis.
	static bool isPlanarRigidMatrix( const Matrix4f &m )
	{
		if( !isRigidMatrix( m ) ) return false;
		return fabsf( m.x[1] ) <= RigidEps && fabsf( m.x[4] ) <= RigidEps &&
		       fabsf( m.x[6] ) <= RigidEps && fabsf( m.x[9] ) <= RigidEps &&
		       fabsf( m.x[5] - 1.0f ) <= RigidEps;
	}


	// Yaw angle of a (planar) rotation quaternion, extracted from its rotation matrix.
	static double quatYaw( const Quaternion &q )
	{
		Matrix4f m( q );
		return atan2( (double)m.x[8], (double)m.x[10] );
	}


	static Matrix4f yawMatrix( double yaw, const Vec3f &trans )
	{
		// Build directly from the double precision angle to avoid float-quantizing the yaw
		// (a 1e-7 rad error would become a visible position error on long paths).
		double c = cos( yaw ), sn = sin( yaw );
		Matrix4f m( Math::NO_INIT );
		m.x[0] = (float)c;  m.x[1] = 0.0f;    m.x[2] = (float)-sn; m.x[3] = 0.0f;
		m.x[4] = 0.0f;     m.x[5] = 1.0f;    m.x[6] = 0.0f;       m.x[7] = 0.0f;
		m.x[8] = (float)sn; m.x[9] = 0.0f;    m.x[10] = (float)c;  m.x[11] = 0.0f;
		m.x[12] = trans.x; m.x[13] = trans.y; m.x[14] = trans.z;   m.x[15] = 1.0f;
		return m;
	}


	// Registry of models currently driven by a root blend player. The registry is keyed per
	// SceneManager generation: after h3dRelease()/h3dInit() a fresh SceneManager exists and any
	// entries left behind by the previous engine run are stale, so they are purged lazily.
	static vector< ModelNode * > &boundModels()
	{
		static vector< ModelNode * > models;
		static const SceneManager *generation = 0x0;
		const SceneManager *current = &Modules::sceneMan();
		if( generation != current )
		{
			generation = current;
			models.clear();
		}
		return models;
	}


	struct RootTrack
	{
		uint32                  frameCount;   // Resource frame count N (>= 2)
		bool                    compressed;   // Constant track with a single frame
		vector< Quaternion >    quats;        // size 1 (compressed) or N
		vector< Vec3f >         pos;          // size 1 (compressed) or N
		double                  period;       // (N - 1) / fps, strictly positive
	};


// =================================================================================================
// Root track validation / construction
// =================================================================================================

static bool buildRootTrack( AnimationResource *anim, const char *rootName, float fps,
                            RootTrack &track )
{
	uint32 nameId = AnimationController::hashName( rootName );
	AnimResEntity *entity = anim->findEntity( nameId );
	if( entity == 0x0 || entity->frames.empty() ) return false;

	int numFrames = anim->getElemParamI( AnimationResData::EntityElem, 0,
	                                    AnimationResData::EntFrameCountI );
	if( numFrames < 2 ) return false;
	if( !finiteF( fps ) || fps <= 0.0f ) return false;

	track.frameCount = (uint32)numFrames;
	track.period = (double)( numFrames - 1 ) / (double)fps;
	track.compressed = entity->frames.size() == 1;
	track.quats.resize( entity->frames.size() );
	track.pos.resize( entity->frames.size() );

	float constantY = 0.0f;
	for( size_t i = 0; i < entity->frames.size(); ++i )
	{
		const Frame &frame = entity->frames[i];
		const Quaternion &q = frame.rotQuat;

		if( !finiteF( q.x ) || !finiteF( q.y ) || !finiteF( q.z ) || !finiteF( q.w ) ) return false;
		if( !finiteF( frame.transVec.x ) || !finiteF( frame.transVec.y ) || !finiteF( frame.transVec.z ) )
			return false;
		if( !finiteF( frame.scaleVec.x ) || !finiteF( frame.scaleVec.y ) || !finiteF( frame.scaleVec.z ) )
			return false;

		// Unit scale only
		if( fabsf( frame.scaleVec.x - 1.0f ) > 1e-5f ||
		    fabsf( frame.scaleVec.y - 1.0f ) > 1e-5f ||
		    fabsf( frame.scaleVec.z - 1.0f ) > 1e-5f ) return false;

		// Unit quaternion
		float qlen2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
		if( fabsf( qlen2 - 1.0f ) > 1e-4f ) return false;

		// Rotation about Y only: the rotation matrix must have the yaw-only structure
		Matrix4f rm( q );
		if( fabsf( rm.x[1] ) > RigidEps || fabsf( rm.x[4] ) > RigidEps ||
		    fabsf( rm.x[6] ) > RigidEps || fabsf( rm.x[9] ) > RigidEps ||
		    fabsf( rm.x[5] - 1.0f ) > RigidEps ) return false;

		// XZ translation: the Y component has to be constant over the whole track
		if( i == 0 ) constantY = frame.transVec.y;
		else if( fabsf( frame.transVec.y - constantY ) > RigidEps ) return false;

		track.quats[i] = q;
		track.pos[i] = frame.transVec;
	}

	return true;
}


// =================================================================================================
// Root track sampling and exact curve derivatives
// =================================================================================================

// Returns the segment index [0, N-2] and the interpolation amount for local frame time u.
static void segmentAt( const RootTrack &track, double u, uint32 &seg, double &amount )
{
	if( track.compressed ) { seg = 0; amount = 0.0; return; }

	double alpha = u * ( track.frameCount - 1 ) / track.period;  // Same as u * fps
	if( alpha < 0.0 ) alpha = 0.0;
	double maxAlpha = (double)track.frameCount - 1.0;
	if( alpha >= maxAlpha ) { seg = track.frameCount - 2; amount = 1.0; return; }

	seg = (uint32)std::floor( alpha );
	amount = alpha - (double)seg;
}


// Body velocity v = R(yaw)^T * dp/dt and angular velocity omega = dyaw/dt of one clip at the
// given absolute time. All derivatives are taken analytically from the interpolated curves; the
// signed playback rate is already included.
static void clipVelocity( const RootTrack &track, float phase, float rate, double t,
                          double &bodyVx, double &bodyVz, double &omega )
{
	double s = (double)phase + (double)rate * t;
	double cycle = std::floor( s / track.period );
	double u = s - cycle * track.period;
	if( u < 0.0 ) u = 0.0;
	if( u > track.period ) u = track.period;

	if( track.compressed )
	{
		bodyVx = bodyVz = omega = 0.0;
		return;
	}

	uint32 seg;
	double amount;
	segmentAt( track, u, seg, amount );
	const Vec3f &p0 = track.pos[seg];
	const Vec3f &p1 = track.pos[seg + 1];

	// Linear position curve: dp/du = (p1 - p0) * fps
	double dpdUx = ( (double)p1.x - (double)p0.x ) * ( track.frameCount - 1 ) / track.period;
	double dpdUz = ( (double)p1.z - (double)p0.z ) * ( track.frameCount - 1 ) / track.period;
	double dpdTx = dpdUx * (double)rate;
	double dpdTz = dpdUz * (double)rate;

	// Shortest-arc nlerp of two yaw quaternions. The normalized lerp stays on the great circle,
	// so its yaw can be written with the half-angle sine/cosine coefficients; differentiate
	// that expression analytically (the nlerp parameter does not move at constant angular speed).
	double yaw0 = quatYaw( track.quats[seg] );
	double yaw1 = quatYaw( track.quats[seg + 1] );
	double delta = wrapPi( yaw1 - yaw0 );
	double h0 = yaw0 * 0.5;
	double h1 = ( yaw0 + delta ) * 0.5;
	double sh0 = sin( h0 ), ch0 = cos( h0 );
	double sh1 = sin( h1 ), ch1 = cos( h1 );
	double sa = sh0 + ( sh1 - sh0 ) * amount;
	double ca = ch0 + ( ch1 - ch0 ) * amount;
	double dSa = sh1 - sh0;
	double dCa = ch1 - ch0;
	double norm2 = sa * sa + ca * ca;
	double dyawDAmount = 2.0 * ( ca * dSa - sa * dCa ) / norm2;
	double dyawDt = dyawDAmount * ( ( track.frameCount - 1 ) / track.period ) * (double)rate;

	// Yaw of the actual interpolated quaternion, used to rotate into the body frame
	double yaw = 2.0 * atan2( sa, ca );
	double c = cos( yaw ), sn = sin( yaw );
	// v_body = R(yaw)^T * dp/dt with R(yaw) = [[c,0,sn],[0,1,0],[-sn,0,c]]
	bodyVx = c * dpdTx - sn * dpdTz;
	bodyVz = sn * dpdTx + c * dpdTz;
	omega = dyawDt;
}


// =================================================================================================
// Weight curve
// =================================================================================================

}  // namespace


class RootBlendPlayerImpl
{
public:
	PAnimationResource  anim[2];
	ModelNode          *model;
	JointNode          *rootJoint;
	float               fps[2];
	float               phase[2];
	float               rate[2];
	RootTrack           track[2];
	vector< RootBlendWeightKey > keys;
	Matrix4f            startRoot;
	Matrix4f            worldRoot;
};


namespace
{


static float weightAt( const vector< RootBlendWeightKey > &keys, double t )
{
	if( t <= keys.front().t ) return keys.front().w;
	if( t >= keys.back().t ) return keys.back().w;

	size_t lo = 0, hi = keys.size() - 1;
	while( lo + 1 < hi )
	{
		size_t mid = ( lo + hi ) / 2;
		if( keys[mid].t <= t ) lo = mid; else hi = mid;
	}
	const RootBlendWeightKey &a = keys[lo];
	const RootBlendWeightKey &b = keys[lo + 1];
	double f = ( t - a.t ) / ( b.t - a.t );
	return (float)( (double)a.w + ( (double)b.w - (double)a.w ) * f );
}


// Blended planar body velocity and angular velocity at absolute time t.
static void blendedVelocity( const RootBlendPlayerImpl &impl, double t, float w,
                             double &vx, double &vz, double &omega )
{
	double vx0, vz0, om0, vx1, vz1, om1;
	clipVelocity( impl.track[0], impl.phase[0], impl.rate[0], t, vx0, vz0, om0 );
	clipVelocity( impl.track[1], impl.phase[1], impl.rate[1], t, vx1, vz1, om1 );
	double w0 = 1.0 - (double)w;
	vx = w0 * vx0 + (double)w * vx1;
	vz = w0 * vz0 + (double)w * vz1;
	omega = w0 * om0 + (double)w * om1;
}


struct IntegState
{
	double theta, x, z;
};


static IntegState derivative( const RootBlendPlayerImpl &impl, double t, const IntegState &s )
{
	float w = weightAt( impl.keys, t );
	double vx, vz, omega;
	blendedVelocity( impl, t, w, vx, vz, omega );
	IntegState d;
	double c = cos( s.theta ), sn = sin( s.theta );
	d.theta = omega;
	// dx/dt = R(theta) * v_body
	d.x = c * vx + sn * vz;
	d.z = -sn * vx + c * vz;
	return d;
}


static IntegState rk4Step( const RootBlendPlayerImpl &impl, double t, double h, const IntegState &s )
{
	IntegState k1 = derivative( impl, t, s );
	IntegState s2;
	s2.theta = s.theta + 0.5 * h * k1.theta;
	s2.x = s.x + 0.5 * h * k1.x;
	s2.z = s.z + 0.5 * h * k1.z;
	IntegState k2 = derivative( impl, t + 0.5 * h, s2 );
	IntegState s3;
	s3.theta = s.theta + 0.5 * h * k2.theta;
	s3.x = s.x + 0.5 * h * k2.x;
	s3.z = s.z + 0.5 * h * k2.z;
	IntegState k3 = derivative( impl, t + 0.5 * h, s3 );
	IntegState s4;
	s4.theta = s.theta + h * k3.theta;
	s4.x = s.x + h * k3.x;
	s4.z = s.z + h * k3.z;
	IntegState k4 = derivative( impl, t + h, s4 );

	IntegState r;
	r.theta = s.theta + h / 6.0 * ( k1.theta + 2.0 * k2.theta + 2.0 * k3.theta + k4.theta );
	r.x = s.x + h / 6.0 * ( k1.x + 2.0 * k2.x + 2.0 * k3.x + k4.x );
	r.z = s.z + h / 6.0 * ( k1.z + 2.0 * k2.z + 2.0 * k3.z + k4.z );
	return r;
}


// Smallest time strictly after cur where the clip reaches a frame index boundary, i.e.
// phase + rate * t is an integer. The interpolated velocity is continuous only inside one frame
// segment, so RK4 steps must not straddle these kinks. Period boundaries are a subset of these
// events. Returns MaxQueryTime + 1 when there is no further boundary.
static double nextFrameBoundary( float phase, float rate, double cur )
{
	if( rate == 0.0f ) return MaxQueryTime + 1.0;
	double s = (double)phase + (double)rate * cur;
	double k;
	if( rate > 0.0f )
		k = std::floor( s ) + 1.0;
	else
		k = std::ceil( s ) - 1.0;
	return ( k - (double)phase ) / (double)rate;
}


// Piecewise RK4 integration. Pieces are split at weight keys and every frame boundary of both
// clips only; the event sequence is a pure function of absolute time, so integrating from t = 0
// always yields bit-identical results no matter how the queries are split.
static IntegState integrate( const RootBlendPlayerImpl &impl, double endT, const IntegState &start )
{
	IntegState state = start;
	double cur = 0.0;
	size_t keyIdx = 0;  // Next key strictly ahead of cur

	while( cur < endT )
	{
		double next = endT;
		while( keyIdx < impl.keys.size() && impl.keys[keyIdx].t <= cur ) ++keyIdx;
		if( keyIdx < impl.keys.size() && impl.keys[keyIdx].t > cur )
			next = min( next, impl.keys[keyIdx].t );
		double fb0 = nextFrameBoundary( impl.phase[0], impl.rate[0], cur );
		if( fb0 > cur ) next = min( next, fb0 );
		double fb1 = nextFrameBoundary( impl.phase[1], impl.rate[1], cur );
		if( fb1 > cur ) next = min( next, fb1 );
		if( next <= cur ) next = endT;  // No usable event ahead: finish the interval
		if( next > endT ) next = endT;

		while( cur < next )
		{
			double h = min( IntegratorStep, next - cur );
			state = rk4Step( impl, cur, h, state );
			cur += h;
		}
		cur = next;  // Snap to the event so the outer loop always makes progress
	}

	return state;
}


// Checks the model node and its whole parent chain for rigid transforms.
static bool parentChainRigid( ModelNode *model )
{
	SceneNode *node = model;
	while( node != 0x0 )
	{
		if( !isRigidMatrix( node->getRelTrans() ) ) return false;
		node = node->getParent();
	}
	return true;
}


static JointNode *findDirectRootJoint( ModelNode *model, const char *rootName )
{
	const vector< SceneNode * > &children = model->getChildren();
	for( size_t i = 0; i < children.size(); ++i )
	{
		SceneNode *child = children[i];
		if( child->getType() == SceneNodeTypes::Joint && child->getName() == rootName )
			return (JointNode *)child;
	}
	return 0x0;
}


}  // namespace


// =================================================================================================
// RootBlendPlayer
// =================================================================================================

RootBlendPlayer::RootBlendPlayer() : _impl( 0x0 ), _bound( false )
{
}


RootBlendPlayer::~RootBlendPlayer()
{
	if( _bound ) unbind();
	delete _impl;
}


bool RootBlendPlayer::initialize( const RootBlendConfig &config )
{
	if( _bound ) return false;

	// --- Resource validation (read-only; the shared animations are never modified) ---
	AnimationResource *anim[2] = { config.anim[0], config.anim[1] };
	for( int i = 0; i < 2; ++i )
	{
		if( anim[i] == 0x0 ) return false;
		if( anim[i]->getType() != ResourceTypes::Animation || !anim[i]->isLoaded() ) return false;
	}

	// --- Scalar validation ---
	for( int i = 0; i < 2; ++i )
	{
		if( !finiteF( config.fps[i] ) || config.fps[i] <= 0.0f ) return false;
		if( !finiteF( config.phase[i] ) || !finiteF( config.rate[i] ) ) return false;
	}
	if( config.rootName == 0x0 || config.rootName[0] == '\0' ) return false;
	if( config.model == 0x0 ) return false;
	if( !isPlanarRigidMatrix( config.startRoot ) ) return false;

	// --- Weight key validation ---
	if( config.keys == 0x0 || config.numKeys == 0 ) return false;
	for( size_t i = 0; i < config.numKeys; ++i )
	{
		const RootBlendWeightKey &k = config.keys[i];
		if( !finiteD( k.t ) || !finiteF( k.w ) ) return false;
		if( k.w < 0.0f || k.w > 1.0f ) return false;
		if( i > 0 && k.t <= config.keys[i - 1].t ) return false;
	}

	// --- Model / scene validation ---
	ModelNode *model = config.model;
	if( find( boundModels().begin(), boundModels().end(), model ) != boundModels().end() )
		return false;
	if( !parentChainRigid( model ) ) return false;
	JointNode *rootJoint = findDirectRootJoint( model, config.rootName );
	if( rootJoint == 0x0 ) return false;
	if( !isRigidMatrix( rootJoint->getRelTrans() ) ) return false;

	// --- Root track validation ---
	RootTrack tracks[2];
	for( int i = 0; i < 2; ++i )
	{
		if( !buildRootTrack( anim[i], config.rootName, config.fps[i], tracks[i] ) )
			return false;
	}

	// --- All checks passed: commit the binding (model mutation starts here) ---
	RootBlendPlayerImpl *impl = new RootBlendPlayerImpl();
	impl->anim[0] = anim[0];
	impl->anim[1] = anim[1];
	impl->model = model;
	impl->rootJoint = rootJoint;
	for( int i = 0; i < 2; ++i )
	{
		impl->fps[i] = config.fps[i];
		impl->phase[i] = config.phase[i];
		impl->rate[i] = config.rate[i];
		impl->track[i] = tracks[i];
	}
	impl->keys.assign( config.keys, config.keys + config.numKeys );
	impl->startRoot = config.startRoot;
	impl->worldRoot = config.startRoot;

	// Same layer, non-additive stages: stage 0 carries clip 1 with weight 1-w and stage 1 clip 2
	// with weight w.
	model->setupAnimStage( 0, anim[0], 0, "", false );
	model->setupAnimStage( 1, anim[1], 0, "", false );

	_impl = impl;
	_bound = true;
	boundModels().push_back( model );
	return true;
}


bool RootBlendPlayer::evaluate( double t )
{
	if( !_bound || _impl == 0x0 ) return false;
	if( !finiteD( t ) || t < 0.0 || t > MaxQueryTime ) return false;

	RootBlendPlayerImpl &impl = *_impl;
	ModelNode *model = impl.model;

	// Nodes may have been changed outside the player; re-validate before touching anything.
	if( find( boundModels().begin(), boundModels().end(), model ) == boundModels().end() )
		return false;
	if( !parentChainRigid( model ) ) return false;
	if( impl.rootJoint->getParent() != model ||
	    impl.rootJoint->getType() != SceneNodeTypes::Joint ) return false;

	// --- Integration from the start pose over [0, t] (backward and interleaved queries safe) ---
	Matrix4f &sr = impl.startRoot;
	IntegState start;
	start.theta = atan2( (double)sr.x[8], (double)sr.x[10] );
	start.x = (double)sr.x[12];
	start.z = (double)sr.x[14];
	IntegState end = integrate( impl, t, start );

	Vec3f rootPos( (float)end.x, sr.x[13], (float)end.z );
	Matrix4f worldRoot = yawMatrix( end.theta, rootPos );

	// --- Local TRS stage blending for every joint through the existing controller ---
	// Make sure the model's absolute transform reflects its parent chain before the animation
	// update runs updateTree() internally; otherwise the internal traversal would reuse a stale
	// parent absolute matrix and would not be revisited later.
	Modules::sceneMan().updateNodes();
	float wNow = weightAt( impl.keys, t );
	for( int clip = 0; clip < 2; ++clip )
	{
		// Frame index on the looping [0, N-1] interval; identical modulo rule to the one used
		// for the integrated velocities.
		double s = (double)impl.phase[clip] + (double)impl.rate[clip] * t;
		double cycle = std::floor( s / impl.track[clip].period );
		double u = s - cycle * impl.track[clip].period;  // [0, period)
		double frameTime = u * (double)impl.fps[clip];    // [0, N-1)
		if( frameTime >= (double)impl.track[clip].frameCount - 1.0 )
			frameTime = (double)impl.track[clip].frameCount - 1.0;  // Period endpoint
		model->setAnimParams( clip, (float)frameTime, clip == 0 ? 1.0f - wNow : wNow );
	}
	model->update( ModelUpdateFlags::Animation );

	// --- Strip the animated planar root motion and place the integrated world root ---
	// The root joint is a direct child of the model, so its local matrix is expressed in the
	// model's frame: local = inverse(absModel) * world.
	Modules::sceneMan().updateNodes();
	Matrix4f rootLocal = model->getAbsTrans().inverted() * worldRoot;
	impl.rootJoint->setTransform( rootLocal );
	Modules::sceneMan().updateNodes();  // Rebuilds rel-model matrices and the inverse-bind palette

	// --- Morphers and software skinning through the regular geometry pipeline ---
	model->update( ModelUpdateFlags::Geometry );

	impl.worldRoot = worldRoot;
	return true;
}


bool RootBlendPlayer::getWorldRoot( Matrix4f &out ) const
{
	if( !_bound || _impl == 0x0 ) return false;
	out = _impl->worldRoot;
	return true;
}


bool RootBlendPlayer::unbind()
{
	if( !_bound || _impl == 0x0 ) return false;

	ModelNode *model = _impl->model;
	model->setupAnimStage( 0, 0x0, 0, "", false );
	model->setupAnimStage( 1, 0x0, 0, "", false );

	vector< ModelNode * > &registry = boundModels();
	registry.erase( remove( registry.begin(), registry.end(), model ), registry.end() );

	delete _impl;
	_impl = 0x0;
	_bound = false;
	return true;
}

}  // namespace Horde3D
