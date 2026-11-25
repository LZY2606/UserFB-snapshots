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

// Dual-action root-motion player.
//
// Instead of interpolating the two animations' absolute root transforms, the player integrates
// each action's planar body velocity (linear and angular, expressed in the body frame and scaled
// by the signed playback rate), blends those velocities with the time-varying weight, and moves a
// single world root along the resulting trajectory. The remaining skeleton is still blended with
// the engine's regular same-layer, non-additive local TRS stage blending.

#ifndef _egRootBlend_H_
#define _egRootBlend_H_

#include "egPrerequisites.h"
#include "utMath.h"
#include <vector>


namespace Horde3D {

class AnimationResource;
class ModelNode;
class JointNode;
struct AnimResEntity;

// =================================================================================================
// Root blend weight key
//
// Time is given in absolute seconds, strictly increasing. The weight is validated against
// [0, 1] and is linearly interpolated between keys; queries outside the key range are clamped to
// the first respectively last value. The weight denotes the contribution of the *second* action;
// the first action contributes (1 - weight).
// =================================================================================================

struct RootBlendKey
{
	float  time;
	float  weight;
};

// =================================================================================================
// RootBlendPlayer
//
// Resource lifetime:
//   The two AnimationResource objects, the model and the root joint are borrowed, not owned. They
//   have to stay alive and loaded for as long as the player is bound. The animation resources are
//   never modified by the player. The model may be released only after unbind() has been called.
//
// Time model:
//   Absolute time starts at zero on a successful bind and may be queried in any order, including
//   backwards and repeated queries of the same time. A rate of zero pauses an action. Every
//   evaluate() call recomputes the full pose for the requested time, so interleaved queries never
//   corrupt the internal state.
//
// Failure handling:
//   Every mutating entry point returns a boolean. On failure nothing is changed: the model, its
//   animation stages, the player binding and the previously computed world root stay untouched and
//   a subsequent valid call keeps working. Read accessors return identity / false when the player
//   is not bound.
// =================================================================================================

class RootBlendPlayer
{
public:
	enum Limits
	{
		MaxQueryTime = 30  // Supported absolute query range is [0, 30] seconds
	};

	RootBlendPlayer();
	~RootBlendPlayer();

	// Creates the binding. Returns false and leaves the player unbound on any invalid argument:
	// null or unloaded/non-animation resources, fps <= 0, fewer than two frames, a root joint name
	// that cannot be found as a direct child joint of the model and in both animations, a root
	// track with non-planar translation, non-Y rotation or non-unit scale, a non-rigid model
	// parent chain, non-finite numeric parameters, non-strictly increasing key times or
	// out-of-range weights, or a non-rigid/non-planar start pose. On success the model's two
	// non-additive animation stages are owned exclusively by the player until unbind().
	bool bind( AnimationResource *anim0, AnimationResource *anim1,
	           float fps0, float fps1,
	           float phase0, float phase1,
	           float rate0, float rate1,
	           const char *rootJointName,
	           ModelNode *model,
	           const Matrix4f &startWorldTrans,
	           const std::vector< RootBlendKey > &keys );

	// Evaluates the pose at absolute time t (0 <= t <= MaxQueryTime) and applies it to the bound
	// model: blended local TRS for every joint, planar root motion removed from the root joint,
	// the integrated world root transformed into the model's parent coordinate space, and the
	// joint palette, morph targets and software skinning refreshed through the regular engine
	// paths. Returns false without side effects when unbound, the time is out of range or
	// non-finite, or the scene graph has been made invalid while bound.
	bool evaluate( float time );

	// Copies the last integrated world root transform (rigid, Y rotation only) into outTrans.
	// Returns false and writes identity when not bound.
	bool getWorldRoot( Matrix4f &outTrans ) const;

	// Releases the binding. The model's animation stages are cleared so that the ordinary stage
	// pipeline can be used again. Safe to call when unbound. The cached world root is forgotten.
	void unbind();

	bool isBound() const { return _bound; }

private:
	// Per-action extracted planar root track.
	struct Action
	{
		AnimationResource      *anim;
		AnimResEntity          *rootEntity;
		float                   fps;
		float                   phase;
		float                   rate;
		float                   period;     // (N - 1) / fps
		uint32                  numFrames;  // N, >= 2
		bool                    constant;   // Compressed single-frame root track

		Action() : anim( 0x0 ), rootEntity( 0x0 ), fps( 0.0f ), phase( 0.0f ), rate( 0.0f ),
		           period( 0.0f ), numFrames( 0 ), constant( false ) {}
	};

	bool validateStart( const Matrix4f &m ) const;
	bool validateParentChain( ModelNode *model ) const;
	bool validateRootTrack( const Action &a ) const;
	bool buildAction( AnimationResource *anim, float fps, float phase, float rate,
	                  uint32 rootNameId, Action &out );
	double weightAt( double t ) const;
	void rootDerivatives( const Action &a, double time,
	                      double &bodyVx, double &bodyVz, double &yawRate ) const;
	void systemDeriv( double s, const double state[3], double d[3] ) const;
	bool integrate( float tA, float tB, const Vec3f &x0, float theta0,
	                Vec3f &x1, float &theta1 ) const;
	void applyPose( float time, float weight,
	                const Vec3f &worldPos, float worldYaw );

	ModelNode                   *_model;
	JointNode                   *_rootJoint;
	uint32                       _rootNameId;
	Action                       _actions[2];
	std::vector< RootBlendKey >  _keys;
	Matrix4f                     _startWorld;
	Matrix4f                     _worldRoot;
	bool                         _bound;
};

}  // namespace Horde3D

#endif // _egRootBlend_H_
