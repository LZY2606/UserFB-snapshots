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

#ifndef _egRootBlend_H_
#define _egRootBlend_H_

#include "egPrerequisites.h"
#include "utMath.h"
#include <cstddef>
#include <vector>


namespace Horde3D {

class AnimationResource;
class ModelNode;
class RootBlendPlayerImpl;


// =================================================================================================
// Dual-animation root motion player
// =================================================================================================

/*
	Conventions
	-----------
	- Both clips loop over [0, (N-1)/fps] where N is the resource frame count (N >= 2). The last
	  frame is the period endpoint; no extra segment from the last frame back to the first is
	  introduced.
	- Clip time is phase + rate * t (t in absolute seconds), cycles are taken with floor(); rate
	  may be zero or negative.
	- Root tracks are planar only: XZ translation, rotation about Y, unit scale. Positions are
	  interpolated linearly, rotations use shortest-arc nlerp. Constant (compressed) root tracks
	  are allowed.
	- The per-clip body velocities v = R(u)^T * dp/dt and angular velocities omega = dyaw/dt are
	  taken from the actual interpolated curves (including the signed playback rate), blended with
	  (1-w, w) and integrated from the given start world pose. The world root satisfies
	  dtheta/dt = omega_blend and dx/dt = R(theta) * v_blend; its Y coordinate stays at the start
	  value.
*/

struct RootBlendWeightKey
{
	double  t;  // Absolute time in seconds, keys of one player must be strictly increasing
	float   w;  // Weight of the second clip, in [0, 1]; the first clip gets 1-w
};

struct RootBlendConfig
{
	// Shared, read-only animation resources. The player never modifies them. They have to stay
	// alive (loaded and held by the resource manager) for the whole binding.
	AnimationResource  *anim[2];

	// Per-clip playback data. fps must be positive and finite. phase must be finite. rate must be
	// finite and may be zero or negative.
	float               fps[2];
	float               phase[2];
	float               rate[2];

	// Name of the root joint. Must name a JointNode that is a direct child of the model. The
	// pointer is only dereferenced during initialization; the name string is copied internally.
	const char         *rootName;

	// Model driven by the player. The model and its scene nodes have to stay alive for the whole
	// binding. Its parent chain must consist of rigid transforms.
	ModelNode          *model;

	// Starting world rigid pose of the root. It must be a planar rigid transform: rotation
	// about Y only, no scale or mirror. The root's Y coordinate stays at this value forever.
	Matrix4f            startRoot;

	// Blend weight keys in absolute seconds. Times must be strictly increasing, weights in
	// [0, 1]. Between two keys the weight is linearly interpolated; outside the key range the
	// nearest end value is used. At least one key is required. The array is copied during
	// initialization and may be released afterwards.
	const RootBlendWeightKey *keys;
	size_t                    numKeys;
};

class RootBlendPlayer
{
public:
	RootBlendPlayer();
	~RootBlendPlayer();

	// Initializes and binds the player to its model. On success the two non-additive animation
	// stages 0 and 1 of the model are owned exclusively by this player until unbind() is called.
	//
	// Returns false (without modifying the model, the stages or any world pose) when:
	//   * a resource handle is invalid, not loaded or not an Animation resource,
	//   * the root joint cannot be found as a direct child joint of the model,
	//   * a root track is incomplete or not planar (non-unit scale or rotation not about Y),
	//   * any scalar argument is non-finite, fps is non-positive or a weight key is invalid,
	//   * the start pose or a node on the model's parent chain is not a rigid transform,
	//   * the model already has another root blend player bound.
	bool initialize( const RootBlendConfig &config );

	// Evaluates the player at absolute time t in [0, 30] seconds and applies the result to the
	// bound model: the non-root joints use the existing same-layer non-additive local TRS stage
	// blending, the root joint's planar motion is stripped and replaced by the integrated world
	// root transformed into the model parent's space, and joint nodes, the inverse-bind palette,
	// morphers and software skinning are updated through the regular engine pipeline.
	//
	// Queries may go backwards, pause (repeated equal time) or interleave. A query not continuing
	// the previous one re-integrates from t = 0 and the start pose.
	//
	// Returns false (without modifying the model or the previously computed world root) when the
	// player is not bound, t is non-finite or outside [0, 30], or an engine node touched by the
	// player has become non-rigid/invalid in the meantime.
	bool evaluate( double t );

	// Writes the last integrated world root matrix (the start pose before the first successful
	// evaluate) into out. Returns false when the player is not bound.
	bool getWorldRoot( Matrix4f &out ) const;

	// Releases the model's two animation stages. Afterwards the model can be driven by the
	// regular stage animation again. Safe to call on an unbound player (returns false).
	bool unbind();

	// True between a successful initialize() and unbind().
	bool isBound() const { return _bound; }

private:
	RootBlendPlayer( const RootBlendPlayer & );             // non-copyable
	RootBlendPlayer &operator=( const RootBlendPlayer & );  // non-copyable

	RootBlendPlayerImpl  *_impl;
	bool                  _bound;
};

}  // namespace Horde3D

#endif // _egRootBlend_H_
