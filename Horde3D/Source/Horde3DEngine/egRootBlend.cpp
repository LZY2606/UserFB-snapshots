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
#include "egModules.h"
#include "egCom.h"
#include <cmath>
#include <algorithm>
#include <limits>


namespace Horde3D {

using namespace std;

namespace
{
	const float  Eps = 1e-4f;     // Rigid/unit-track tolerance
	const double EpsD = 1e-12;    // Integration event tolerance

	bool finiteF( float v ) { return std::isfinite( v ); }

	bool finiteM( const Matrix4f &m )
	{
		for( int i = 0; i < 16; ++i ) if( !std::isfinite( m.x[i] ) ) return false;
		return true;
	}

	bool orthonormal3x3( const Matrix4f &m )
	{
		for( int c = 0; c < 3; ++c )
		{
			float l2 = m.c[c][0] * m.c[c][0] + m.c[c][1] * m.c[c][1] + m.c[c][2] * m.c[c][2];
			if( fabsf( l2 - 1.0f ) > Eps ) return false;
		}
		for( int c0 = 0; c0 < 3; ++c0 )
			for( int c1 = c0 + 1; c1 < 3; ++c1 )
			{
				float d = m.c[c0][0] * m.c[c1][0] + m.c[c0][1] * m.c[c1][1] +
				          m.c[c0][2] * m.c[c1][2];
				if( fabsf( d ) > Eps ) return false;
			}
		return true;
	}

	// Affine matrix with a homogeneous last row and an orthonormal rotation (no scale at all).
	bool isRigidMatrix( const Matrix4f &m )
	{
		if( !finiteM( m ) ) return false;
		if( fabsf( m.x[3] ) > Eps || fabsf( m.x[7] ) > Eps ||
		    fabsf( m.x[11] ) > Eps || fabsf( m.x[15] - 1.0f ) > Eps ) return false;
		return orthonormal3x3( m );
	}

	// Rigid transform whose rotation contains no tilt: the Y axis maps onto itself.
	bool isRigidPlanarMatrix( const Matrix4f &m )
	{
		if( !isRigidMatrix( m ) ) return false;
		return fabsf( m.c[1][0] ) <= Eps && fabsf( m.c[1][1] - 1.0f ) <= Eps &&
		       fabsf( m.c[1][2] ) <= Eps;
	}
}


// =================================================================================================
// Construction / teardown
// =================================================================================================

RootBlendPlayer::RootBlendPlayer() :
	_model( 0x0 ), _rootJoint( 0x0 ), _rootNameId( 0 ), _bound( false )
{
}


RootBlendPlayer::~RootBlendPlayer()
{
	if( _bound ) unbind();
}


// =================================================================================================
// Validation
// =================================================================================================

bool RootBlendPlayer::validateStart( const Matrix4f &m ) const
{
	return isRigidPlanarMatrix( m );
}


bool RootBlendPlayer::validateParentChain( ModelNode *model ) const
{
	// Every ancestor of the model has to be a rigid body transform; non-uniform scale may still
	// exist on branches that are not part of this chain.
	SceneNode *node = model->getParent();
	while( node != 0x0 )
	{
		if( !isRigidMatrix( node->getRelTrans() ) ) return false;
		node = node->getParent();
	}
	return true;
}


bool RootBlendPlayer::validateRootTrack( const Action &a ) const
{
	AnimResEntity *ent = a.rootEntity;
	if( ent == 0x0 || ent->frames.empty() ) return false;

	const uint32 count = (uint32)ent->frames.size();
	for( uint32 i = 0; i < count; ++i )
	{
		const Frame &f = ent->frames[i];
		if( !finiteF( f.transVec.x ) || !finiteF( f.transVec.y ) || !finiteF( f.transVec.z ) )
			return false;
		if( !finiteF( f.rotQuat.x ) || !finiteF( f.rotQuat.y ) ||
		    !finiteF( f.rotQuat.z ) || !finiteF( f.rotQuat.w ) ) return false;
		if( !finiteF( f.scaleVec.x ) || !finiteF( f.scaleVec.y ) || !finiteF( f.scaleVec.z ) )
			return false;

		// Unit scale on every axis.
		if( fabsf( f.scaleVec.x - 1.0f ) > Eps || fabsf( f.scaleVec.y - 1.0f ) > Eps ||
		    fabsf( f.scaleVec.z - 1.0f ) > Eps ) return false;

		// Unit quaternion with no tilt (rotation around Y only).
		float ql2 = f.rotQuat.x * f.rotQuat.x + f.rotQuat.y * f.rotQuat.y +
		            f.rotQuat.z * f.rotQuat.z + f.rotQuat.w * f.rotQuat.w;
		if( fabsf( ql2 - 1.0f ) > Eps ) return false;
		if( fabsf( f.rotQuat.x ) > Eps || fabsf( f.rotQuat.z ) > Eps ) return false;

		// Planar translation only.
		if( fabsf( f.transVec.y ) > Eps ) return false;
	}
	return true;
}


bool RootBlendPlayer::buildAction( AnimationResource *anim, float fps, float phase, float rate,
                                   uint32 rootNameId, Action &out )
{
	if( anim == 0x0 || !anim->isLoaded() ) return false;
	if( anim->getType() != ResourceTypes::Animation ) return false;
	if( !finiteF( fps ) || fps <= 0.0f ) return false;
	if( !finiteF( phase ) || !finiteF( rate ) ) return false;

	if( anim->getElemCount( AnimationResData::EntityElem ) <= 0 ) return false;
	const uint32 numFrames = (uint32)anim->getElemParamI(
		AnimationResData::EntityElem, 0, AnimationResData::EntFrameCountI );
	if( numFrames < 2 ) return false;

	AnimResEntity *ent = anim->findEntity( rootNameId );
	if( ent == 0x0 ) return false;

	out.anim = anim;
	out.rootEntity = ent;
	out.fps = fps;
	out.phase = phase;
	out.rate = rate;
	out.numFrames = numFrames;
	out.period = ( float )( numFrames - 1 ) / fps;
	out.constant = ent->frames.size() == 1;
	if( !finiteF( out.period ) || out.period <= 0.0f ) return false;

	return validateRootTrack( out );
}


// =================================================================================================
// Binding
// =================================================================================================

bool RootBlendPlayer::bind( AnimationResource *anim0, AnimationResource *anim1,
                            float fps0, float fps1,
                            float phase0, float phase1,
                            float rate0, float rate1,
                            const char *rootJointName,
                            ModelNode *model,
                            const Matrix4f &startWorldTrans,
                            const vector< RootBlendKey > &keys )
{
	if( _bound ) return false;
	if( anim0 == 0x0 || anim1 == 0x0 || model == 0x0 || rootJointName == 0x0 ) return false;
	if( !validateStart( startWorldTrans ) ) return false;
	if( keys.empty() ) return false;

	for( size_t i = 0; i < keys.size(); ++i )
	{
		if( !finiteF( keys[i].time ) || !finiteF( keys[i].weight ) ) return false;
		if( keys[i].weight < 0.0f || keys[i].weight > 1.0f ) return false;
		if( i > 0 && !( keys[i].time > keys[i - 1].time ) ) return false;
	}

	if( !validateParentChain( model ) ) return false;

	const uint32 rootNameId = AnimationController::hashName( rootJointName );

	// The named joint must exist exactly once and be a direct child joint of the model. The
	// internal find keeps a reusable result list, so clear it before scoping the search.
	Modules::sceneMan().clearFindResults();
	const int found = Modules::sceneMan().findNodes( *model, rootJointName, SceneNodeTypes::Joint );
	if( found != 1 ) return false;
	SceneNode *rootNode = Modules::sceneMan().getFindResult( 0 );
	if( rootNode == 0x0 || rootNode->getType() != SceneNodeTypes::Joint ||
	    rootNode->getParent() != model ) return false;

	Action a0, a1;
	if( !buildAction( anim0, fps0, phase0, rate0, rootNameId, a0 ) ) return false;
	if( !buildAction( anim1, fps1, phase1, rate1, rootNameId, a1 ) ) return false;

	// Commit only after every check has succeeded.
	_actions[0] = a0;
	_actions[1] = a1;
	_keys = keys;
	_model = model;
	_rootJoint = (JointNode *)rootNode;
	_rootNameId = rootNameId;
	_startWorld = startWorldTrans;
	_worldRoot = startWorldTrans;
	_bound = true;

	// Two same-layer, non-additive stages; the controller normalizes their weights internally.
	_model->setupAnimStage( 0, anim0, 0, "", false );
	_model->setupAnimStage( 1, anim1, 0, "", false );

	return true;
}


void RootBlendPlayer::unbind()
{
	if( !_bound ) return;

	if( _model != 0x0 )
	{
		_model->setupAnimStage( 0, 0x0, 0, "", false );
		_model->setupAnimStage( 1, 0x0, 0, "", false );
	}

	_model = 0x0;
	_rootJoint = 0x0;
	_keys.clear();
	_worldRoot = Matrix4f();
	_bound = false;
}


// =================================================================================================
// Weight curve
// =================================================================================================

double RootBlendPlayer::weightAt( double t ) const
{
	if( t <= ( double )_keys.front().time ) return ( double )_keys.front().weight;
	if( t >= ( double )_keys.back().time ) return ( double )_keys.back().weight;

	for( size_t i = 1; i < _keys.size(); ++i )
	{
		if( t <= ( double )_keys[i].time )
		{
			const double span = ( double )_keys[i].time - ( double )_keys[i - 1].time;
			const double a = ( t - ( double )_keys[i - 1].time ) / span;
			return ( double )_keys[i - 1].weight +
			       ( ( double )_keys[i].weight - ( double )_keys[i - 1].weight ) * a;
		}
	}
	return _keys.back().weight;
}


// =================================================================================================
// Root track derivatives
//
// Action time is phase + rate * t; the periodic normalized coordinate u wraps with floor().
// Position is linear between frames and rotation uses shortest-arc nlerp of Y-only quaternions.
// Derivatives are taken from those exact interpolation curves, never from finite differences.
// =================================================================================================

void RootBlendPlayer::rootDerivatives( const Action &a, double time,
                                       double &bodyVx, double &bodyVz, double &yawRate ) const
{
	bodyVx = bodyVz = yawRate = 0.0;
	if( a.constant || a.rate == 0.0f ) return;

	const double span = ( double )( a.numFrames - 1 );
	const double rfps = ( double )a.rate * ( double )a.fps;

	// Frame playhead wrapped into [0, N-1); the track is periodic with floor(). At an exact frame
	// integer the wrapped value lands on the boundary; the integrator never subdivides across a
	// boundary (events split the timeline first), so this natural value is the correct one.
	double wrapped = ( ( double )a.phase + ( double )a.rate * ( double )time ) * ( double )a.fps;
	wrapped = wrapped - floor( wrapped / span ) * span;

	uint32 f0 = (uint32)floor( wrapped );
	if( f0 >= a.numFrames - 1 ) f0 = a.numFrames - 2;
	const double alpha = wrapped - ( double )f0;

	const Frame &fr0 = a.rootEntity->frames[f0];
	const Frame &fr1 = a.rootEntity->frames[f0 + 1];

	// dp/dt in root-track space from the linear position curve, kept in double precision.
	const double ddx = ( ( double )fr1.transVec.x - ( double )fr0.transVec.x ) * rfps;
	const double ddz = ( ( double )fr1.transVec.z - ( double )fr0.transVec.z ) * rfps;

	// Shortest-arc nlerp of the Y-only quaternions, evaluated entirely in double precision.
	double qy0 = fr0.rotQuat.y, qw0 = fr0.rotQuat.w;
	double qy1 = fr1.rotQuat.y, qw1 = fr1.rotQuat.w;
	if( qy0 * qy1 + qw0 * qw1 < 0.0 ) { qy1 = -qy1; qw1 = -qw1; }
	const double ly = qy1 - qy0, lw = qw1 - qw0;
	const double yt = qy0 + ly * alpha;
	const double wt = qw0 + lw * alpha;
	double invLen = 1.0 / sqrt( yt * yt + wt * wt );
	double y = yt * invLen, w = wt * invLen;

	// phi = 2 * atan2(y, w); dphi/dt = 2 * (w*y' - y*w') / (y^2 + w^2), then scaled by rate*fps.
	// The angular rate is invariant under the double cover (y,w) -> (-y,-w), so it is correct here.
	const double denom = yt * yt + wt * wt;
	double dPhi = 0.0;
	if( denom > EpsD ) dPhi = 2.0 * ( wt * ly - yt * lw ) / denom;
	yawRate = dPhi * rfps;

	// Canonicalize to w >= 0 so that the extracted yaw (and the R(yaw)^T basis) is the actual
	// small Y rotation rather than its +/-2*pi double-cover representative.
	if( w < 0.0 ) { w = -w; y = -y; }

	// Body yaw and the R(yaw)^T basis (cos, sin) used to bring the world velocity into the frame.
	const double yaw = 2.0 * atan2( y, w );
	const double c = cos( yaw ), s = sin( yaw );
	bodyVx = c * ddx - s * ddz;
	bodyVz = s * ddx + c * ddz;
}


void RootBlendPlayer::systemDeriv( double s, const double state[3], double d[3] ) const
{
	double bx0, bz0, o0;
	double bx1, bz1, o1;
	rootDerivatives( _actions[0], s, bx0, bz0, o0 );
	rootDerivatives( _actions[1], s, bx1, bz1, o1 );

	const double w = weightAt( s );
	const double bx = ( 1.0 - w ) * bx0 + w * bx1;
	const double bz = ( 1.0 - w ) * bz0 + w * bz1;
	const double omega = ( 1.0 - w ) * o0 + w * o1;

	// state = (x, z, theta); world velocity is the body velocity rotated by the current heading.
	const double theta = state[2];
	const double ct = cos( theta ), st = sin( theta );
	d[0] = ct * bx - st * bz;  // dx/dt = R(theta) * v_body
	d[1] = st * bx + ct * bz;  // dz/dt
	d[2] = omega;              // dtheta/dt
}

bool RootBlendPlayer::integrate( float tA, float tB, const Vec3f &x0, float theta0,
                                 Vec3f &x1, float &theta1 ) const
{
	double state[3] = { ( double )x0.x, ( double )x0.z, ( double )theta0 };
	const double dir = tB >= tA ? 1.0 : -1.0;
	double cur = ( double )tA;
	const double target = ( double )tB;

	while( fabs( target - cur ) > EpsD )
	{
		// Nearest event ahead of the current point in the direction of travel. Events are weight
		// keys and frame-segment (integer playhead) boundaries; the curves are smooth between them.
		double nextEvent = target;

		for( size_t i = 0; i < _keys.size(); ++i )
		{
			const double kt = ( double )_keys[i].time;
			if( dir > 0.0 && kt > cur + EpsD && kt < nextEvent ) nextEvent = kt;
			if( dir < 0.0 && kt < cur - EpsD && kt > nextEvent ) nextEvent = kt;
		}

		for( int ai = 0; ai < 2; ++ai )
		{
			const Action &a = _actions[ai];
			if( a.rate == 0.0f ) continue;

			// Frame playhead F(t) = (phase + rate*t) * fps.
			const double fNow = ( ( double )a.phase + ( double )a.rate * cur ) * ( double )a.fps;
			double j;
			if( ( double )a.rate * dir > 0.0 ) j = floor( fNow + EpsD ) + 1.0;  // F increasing
			else j = ceil( fNow - EpsD ) - 1.0;                                  // F decreasing
			const double boundaryT = ( j / ( double )a.fps - ( double )a.phase ) / ( double )a.rate;

			if( dir > 0.0 && boundaryT > cur + EpsD && boundaryT < nextEvent ) nextEvent = boundaryT;
			if( dir < 0.0 && boundaryT < cur - EpsD && boundaryT > nextEvent ) nextEvent = boundaryT;
		}

		const double segEnd = dir > 0.0 ? min( nextEvent, target ) : max( nextEvent, target );
		const double segLen = segEnd - cur;  // Signed; curves are smooth inside this segment.

		// Fixed RK4 substeps within the smooth segment. No discontinuity is crossed, so RK4 is
		// fourth-order here and a coarse step already gives negligible truncation. Integrating from
		// zero on every query makes any subdivision reproduce the identical trajectory, satisfying
		// the split-query determinism requirement.
		const int sub = std::max( 2, (int)ceil( fabs( segLen ) / 1e-6 ) );
		const double h = segLen / ( double )sub;
		for( int q = 0; q < sub; ++q )
		{
			double k1[3], k2[3], k3[3], k4[3], tmp[3];
			systemDeriv( cur, state, k1 );
			for( int c = 0; c < 3; ++c ) tmp[c] = state[c] + h * 0.5 * k1[c];
			systemDeriv( cur + h * 0.5, tmp, k2 );
			for( int c = 0; c < 3; ++c ) tmp[c] = state[c] + h * 0.5 * k2[c];
			systemDeriv( cur + h * 0.5, tmp, k3 );
			for( int c = 0; c < 3; ++c ) tmp[c] = state[c] + h * k3[c];
			systemDeriv( cur + h, tmp, k4 );
			for( int c = 0; c < 3; ++c )
				state[c] += h / 6.0 * ( k1[c] + 2.0 * k2[c] + 2.0 * k3[c] + k4[c] );
			cur += h;
		}
	}

	x1 = Vec3f( (float)state[0], _startWorld.getTrans().y, (float)state[1] );
	theta1 = (float)state[2];
	return true;
}


// =================================================================================================
// Pose application
// =================================================================================================

void RootBlendPlayer::applyPose( float time, float weight,
                                 const Vec3f &worldPos, float worldYaw )
{
	// Convert each action's wrapped normalized time back to the controller's [0, N) frame time.
	float frameTime[2];
	for( int ai = 0; ai < 2; ++ai )
	{
		const Action &a = _actions[ai];
		if( a.constant )
		{
			frameTime[ai] = 0.0f;
		}
		else
		{
			double at = ( double )a.phase + ( double )a.rate * ( double )time;
			double u = at / ( double )a.period;
			u = u - floor( u );
			frameTime[ai] = (float)( u * ( double )( a.numFrames - 1 ) );
		}
	}

	// Local TRS blending requires the interpolating (non-fast) animation path.
	bool &fastAnim = Modules::config().fastAnimation;
	const bool savedFast = fastAnim;
	fastAnim = false;

	_model->setAnimParams( 0, frameTime[0], 1.0f - weight );
	_model->setAnimParams( 1, frameTime[1], weight );
	_model->update( ModelUpdateFlags::Animation );

	fastAnim = savedFast;

	// Strip all planar motion from the animated root joint; keep the height and the Y rotation.
	const Matrix4f &rootRel = _rootJoint->getRelTrans();
	const Vec3f rootTrans = rootRel.getTrans();
	const float jointYaw = atan2f( rootRel.c[2][0], rootRel.c[0][0] );
	Matrix4f stripped = Matrix4f( Quaternion( 0.0f, jointYaw, 0.0f ) );
	stripped = Matrix4f::TransMat( 0.0f, rootTrans.y, 0.0f ) * stripped;
	_rootJoint->setTransform( stripped );

	// Assemble and cache the integrated rigid world root.
	Matrix4f worldRoot = Matrix4f( Quaternion( 0.0f, worldYaw, 0.0f ) );
	worldRoot = Matrix4f::TransMat( worldPos.x, worldPos.y, worldPos.z ) * worldRoot;
	_worldRoot = worldRoot;

	// Move the model so that the world root, expressed in the parent's frame, lands on worldRoot.
	Modules::sceneMan().updateNodes();
	SceneNode *parent = _model->getParent();
	const Matrix4f parentAbs = parent != 0x0 ? parent->getAbsTrans() : Matrix4f();
	const Matrix4f modelLocal = parent != 0x0 ? parentAbs.inverted() * worldRoot : worldRoot;
	_model->setTransform( modelLocal );

	// Refresh absolute matrices, JointNode palette (_relModelMat * inverse-bind), then the regular
	// software skinning / morph pipeline operating on the model's own geometry copy.
	Modules::sceneMan().updateNodes();
	_model->update( ModelUpdateFlags::Geometry );
}


// =================================================================================================
// Evaluation
// =================================================================================================

bool RootBlendPlayer::evaluate( float time )
{
	if( !_bound ) return false;
	if( !finiteF( time ) || time < 0.0f || time > ( float )MaxQueryTime ) return false;
	if( _model == 0x0 || _rootJoint == 0x0 ) return false;
	if( !_actions[0].anim->isLoaded() || !_actions[1].anim->isLoaded() ) return false;
	if( _rootJoint->getParent() != _model ) return false;
	if( !validateParentChain( _model ) ) return false;

	const float w = (float)weightAt( (double)time );

	const Vec3f startPos = _startWorld.getTrans();
	const float startYaw = atan2f( _startWorld.c[2][0], _startWorld.c[0][0] );

	Vec3f endPos;
	float endYaw;
	if( !integrate( 0.0f, time, startPos, startYaw, endPos, endYaw ) ) return false;

	applyPose( time, w, endPos, endYaw );
	return true;
}


bool RootBlendPlayer::getWorldRoot( Matrix4f &outTrans ) const
{
	if( !_bound )
	{
		outTrans = Matrix4f();
		return false;
	}
	outTrans = _worldRoot;
	return true;
}

}  // namespace Horde3D
