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

// -------------------------------------------------------------------------------------------------
//
// Motion matching
//
// A windowless motion matching system. A MotionDatabase is built from several AnimationResource
// clips that share one skeleton. Every legal integer frame of every clip becomes a candidate.
// Candidates store pose-relative positions and central-difference velocities of a set of feature
// joints, together with the root's XZ displacement and yaw change over a fixed future horizon.
// MotionDatabase::search() finds the candidate with the lowest weighted squared error against a
// query pose and is deterministic. MotionPlayer drives a real ModelNode: it advances the current
// clip, extracts features, runs a search and applies the selected clip through the engine's
// existing JointNode hierarchy, inverse-bind palette and software skinning.
//
// Conventions and units
// ----------------------
// - Distances/translations are in the same world units as the scene graph (typically meters).
// - All times are seconds. A clip with N frames and frame rate fps covers the closed,
//   non-cyclic interval [0, (N-1)/fps]. The last frame never wraps to the first frame.
// - Angles are radians, right-handed, positive counter-clockwise when looking from the positive
//   end of the rotation axis towards the origin. Root rotation is only about the positive Y axis.
// - Rotation interpolation follows the animation resources: linear translations and shortest-arc
//   normalized linear quaternion interpolation (nlerp). A track compressed to a single frame is
//   treated as a constant. Joint scales are always one.
// - The root joint has no parent. Its track contains only XZ translation and Y rotation: the Y
//   translation is zero and the quaternion is a pure Y-axis rotation. All other joints may rotate
//   and translate.
// - Feature positions/velocities are expressed in the model-space root frame G(t): the rotation
//   part of inverse(G(t)) applied to model-space quantities. The future feature is the XZ
//   translation and yaw of inverse(G(t)) * G(t+h).
//
// Resource lifetime
// -----------------
// - The skeleton description, clips and query pointers passed to a MotionDatabase only need to be
//   valid for the duration of the constructing or searching call. All required data is copied;
//   the database never dereferences those pointers afterwards.
// - AnimationResource pointers given to clips must stay loaded while MotionDatabase::build() runs.
//   The database keeps no reference to them afterwards, so they may be released independently.
// - A MotionPlayer holds a non-owning pointer to its database and ModelNode. Both must outlive the
//   player. Several players may share one database and may be advanced in any interleaved order;
//   they never mutate the database or each other.
//
// Failure behavior
// ----------------
// - All validity checks are performed before any state changes. On failure, build() leaves the
//   database empty, search() leaves its result output untouched and MotionPlayer calls leave the
//   player, the database and the ModelNode (including animation stages and morphers) exactly as
//   they were. Failing functions return false (or an invalid result) and call
//   Modules::setError(); no exceptions are thrown.
// - Motion is never cyclic: a step whose evaluation point or either central-difference sample
//   leaves the valid interval of the current clip fails explicitly instead of clamping or
//   wrapping.
//
// -------------------------------------------------------------------------------------------------

#ifndef _egMotionMatching_H_
#define _egMotionMatching_H_

#include "egPrerequisites.h"
#include "utMath.h"
#include <string>
#include <vector>


namespace Horde3D {

class AnimationResource;
class ModelNode;


// =================================================================================================
// Plain value types
// =================================================================================================

// Three-component vector in engine units (meters, or meters/second for velocities).
struct MMVec3
{
	float  x, y, z;

	MMVec3() : x( 0.0f ), y( 0.0f ), z( 0.0f ) {}
	MMVec3( float x_, float y_, float z_ ) : x( x_ ), y( y_ ), z( z_ ) {}
	explicit MMVec3( const Vec3f &v ) : x( v.x ), y( v.y ), z( v.z ) {}

	Vec3f toVec3f() const { return Vec3f( x, y, z ); }
};


// Rigid transform wrapper around the engine's column-major Matrix4f.
// Only rigid transformations (rotation + translation, unit scale) are produced by this module.
struct MMMat4
{
	Matrix4f  m;

	MMMat4() {}
	explicit MMMat4( const Matrix4f &mat ) : m( mat ) {}

	static MMMat4 identity() { return MMMat4( Matrix4f() ); }

	// Pointer to the 16 column-major floats, compatible with the engine layout.
	const float *data() const { return &m.x[0]; }
	float *data() { return &m.x[0]; }
};


// =================================================================================================
// Skeleton description
// =================================================================================================

// One joint of the shared skeleton.
// parentIndex is -1 for the root joint; every other index must refer to a joint in the same
// description. Joints do not need to be stored parent-before-child.
struct MMSkeletonJoint
{
	std::string  name;
	int          parentIndex;   // -1 for the root joint
	MMMat4       restLocal;     // Rest pose local transform (parent space; model space for root)
};


// Complete skeleton. Exactly one joint must have parentIndex == -1 (the root).
// A joint that has no track in a clip keeps its restLocal transform in that clip.
struct MMSkeleton
{
	std::vector< MMSkeletonJoint >  joints;
};


// =================================================================================================
// Clips and queries
// =================================================================================================

// One non-cyclic animation clip.
// anim must be a loaded AnimationResource whose tracks use the skeleton's joint names. fps is the
// frames-per-second rate of that resource and must be strictly positive. The clip spans the
// closed interval [0, (frameCount-1)/fps].
struct MMClip
{
	AnimationResource  *anim;
	float               fps;

	MMClip() : anim( 0x0 ), fps( 0.0f ) {}
	MMClip( AnimationResource *anim_, float fps_ ) : anim( anim_ ), fps( fps_ ) {}
};


// The four non-negative weights of a search. At least one must be strictly positive. Terms are
// summed component by component without any additional normalization.
struct MMWeights
{
	float  position;   // Weight of feature-joint position squared errors
	float  velocity;   // Weight of feature-joint velocity squared errors
	float  trajectory; // Weight of future XZ displacement squared error
	float  facing;     // Weight of future yaw unit-vector (cos,sin) squared error

	MMWeights() : position( 0.0f ), velocity( 0.0f ), trajectory( 0.0f ), facing( 0.0f ) {}
	MMWeights( float position_, float velocity_, float trajectory_, float facing_ ) :
		position( position_ ), velocity( velocity_ ), trajectory( trajectory_ ), facing( facing_ ) {}
};


// A search query. Array sizes must match the feature joint count given to the database and the
// order used there. Positions and velocities are in the current root frame; the future trajectory
// is given relative to that same frame. All values must be finite.
struct MMSearchInput
{
	std::vector< MMVec3 >  positions;   // One position per feature joint, root frame
	std::vector< MMVec3 >  velocities;  // One velocity per feature joint, root frame
	MMVec3                 futureDisplacement;  // XZ displacement over the horizon (y is ignored)
	float                  futureYaw;           // Yaw change over the horizon in radians
	MMWeights              weights;
};


// Result of a successful search.
struct MMSearchResult
{
	int    clipIndex;   // Index of the selected clip in the order given to build()
	float  sourceTime;  // Source time inside that clip in seconds (always an integer frame time)
	float  cost;        // Total weighted squared error

	MMSearchResult() : clipIndex( -1 ), sourceTime( 0.0f ), cost( 0.0f ) {}
	bool valid() const { return clipIndex >= 0; }
};


// Read-only description of one stored candidate, used to inspect database contents.
// Arrays use the feature joint order given to build(). Positions and velocities are expressed in
// the candidate's root frame; futureDisplacement is the XZ component (y always zero).
struct MMCandidateInfo
{
	int                    clipIndex;
	uint32                 frame;
	float                  sourceTime;
	std::vector< MMVec3 >  positions;
	std::vector< MMVec3 >  velocities;
	MMVec3                 futureDisplacement;
	float                  futureYaw;

	MMCandidateInfo() : clipIndex( -1 ), frame( 0 ), sourceTime( 0.0f ), futureYaw( 0.0f ) {}
};


// =================================================================================================
// Motion database
// =================================================================================================

class MotionDatabase
{
public:
	MotionDatabase();
	~MotionDatabase();

	// Builds the database from clips sharing skeleton. The feature joints are identified by name
	// and looked up in skeleton; horizonSeconds (h) must be strictly positive. A clip frame is a
	// candidate only when it has at least one frame of margin on both sides and the whole horizon
	// can still be sampled: t-delta >= 0, t+delta <= clipEnd and t+h <= clipEnd, with
	// delta = 1/fps of that clip. The build fails (leaving the database empty) on duplicate joint
	// names, parent cycles, unknown feature joints, unknown or malformed animation tracks, illegal
	// root transforms, non-finite data, zero/negative fps or horizon, or when no legal candidate
	// exists. Animation resources and the skeleton are only read and never modified.
	bool build( const MMSkeleton &skeleton, const std::vector< MMClip > &clips,
	            const std::vector< std::string > &featureJointNames, float horizonSeconds );

	bool isBuilt() const { return _built; }
	int getClipCount() const { return (int)_clips.size(); }
	int getFeatureJointCount() const { return (int)_featureJointNames.size(); }
	float getHorizonSeconds() const { return _horizon; }
	int getCandidateCount() const { return (int)_candidates.size(); }

	// Copies the candidate at candidateIndex (0 .. getCandidateCount()-1) into out. Fails and
	// leaves out untouched when the database is not built or the index is out of range.
	bool getCandidate( int candidateIndex, MMCandidateInfo &out ) const;

	// Duration (seconds) of one clip: the time of its last frame, (frameCount-1)/fps.
	float getClipDuration( int clipIndex ) const;

	// Runs a deterministic search. The result is the candidate with the smallest weighted sum of
	// squared errors: positions, velocities, future XZ displacement and the squared error of the
	// yaw unit vectors (cos(yaw),sin(yaw)), each term summed component by component. Ties are
	// broken by ascending clip index and then ascending source frame. Fails (returning false and
	// leaving result untouched) when the database is not built, array sizes do not match the
	// feature joint count, any value is non-finite or all four weights are zero.
	bool search( const MMSearchInput &input, MMSearchResult &result ) const;

private:
	struct JointTrack
	{
		bool                  constant;   // Track compressed to one frame
		Quaternion            constRot;
		Vec3f                 constTrans;
		std::vector< Quaternion >  rotations;
		std::vector< Vec3f >       translations;

		JointTrack() : constant( false ) {}
	};

	struct ClipData
	{
		float                    fps;
		float                    duration;    // (frameCount - 1) / fps
		uint32                   frameCount;
		std::vector< JointTrack >  tracks;    // One entry per skeleton joint
	};

	struct SkeletonData
	{
		std::vector< std::string >  names;
		std::vector< int >          parents;
		std::vector< Matrix4f >     restLocal;
		int                         rootIndex;
	};

	struct Candidate
	{
		int    clipIndex;
		uint32 frame;       // Integer source frame
		float  sourceTime;  // frame / fps

		std::vector< Vec3f > positions;   // Feature joint positions in root frame
		std::vector< Vec3f > velocities;  // Feature joint velocities in root frame
		Vec3f                futureDisp;  // XZ displacement of root over the horizon
		float                futureYaw;   // Yaw of inverse(G(t)) * G(t+h)
	};

	// Samples one joint's local transform at time t using the resource interpolation rules.
	bool sampleLocal( const ClipData &clip, uint32 joint, float t,
	                  Quaternion &rot, Vec3f &trans ) const;
	// Evaluates local and model-space matrices of every joint and the root matrix G(t).
	bool samplePose( const SkeletonData &skel, const ClipData &clip, float t,
	                 std::vector< Matrix4f > &localMats,
	                 std::vector< Matrix4f > &modelMats, Matrix4f &rootMat ) const;
	// Finds a stored candidate by clip and source time.
	const Candidate *findCandidate( int clipIndex, float sourceTime ) const;

	bool                                   _built;
	float                                  _horizon;
	SkeletonData                           _skeleton;
	std::vector< uint32 >                  _featureJoints;
	std::vector< std::string >             _featureJointNames;
	std::vector< ClipData >                _clips;
	std::vector< Candidate >               _candidates;

	friend class MotionPlayer;
};


// =================================================================================================
// Motion player
// =================================================================================================

class MotionPlayer
{
public:
	MotionPlayer();
	~MotionPlayer();

	// Connects the player to a built database and a ModelNode whose skeleton uses the same joint
	// names as the database skeleton. The model must contain a joint named after the skeleton's
	// root joint. The initial candidate is (startClipIndex, startTimeSeconds), which must match a
	// candidate stored in the database exactly (startTimeSeconds is an integer frame time).
	// worldRoot is the character's world root matrix W for that candidate. The fixed alignment
	// W * inverse(G_new) is derived from it. Fails without touching the model when the database is
	// not built, the candidate does not exist or a skeleton joint is missing from the model.
	bool init( MotionDatabase &database, ModelNode *model, int startClipIndex,
	           float startTimeSeconds, const MMMat4 &worldRoot );

	bool isActive() const { return _active; }
	int getCurrentClipIndex() const { return _clipIndex; }
	float getCurrentTime() const { return _time; }

	// Current world root matrix W (model-space root transformed into world space).
	MMMat4 getWorldRoot() const;
	// Fixed alignment matrix W * inverse(G_new) established at the latest clip switch.
	MMMat4 getAlignment() const;

	// Advances the current clip by dtSeconds (which may be zero to re-select without moving),
	// evaluates the actual pose, extracts feature positions and central-difference velocities,
	// searches with query (its weights configure the search) and switches to the selected source
	// time. The new clip's source root is aligned exactly to the advanced world root, then the
	// pose is applied to the ModelNode using its existing JointNode hierarchy, inverse-bind
	// palette and software skinning. The root joint's local matrix is stripped of root motion
	// (identity), so root motion is applied once through the model node only. Fails explicitly,
	// changing nothing, when the step or either difference sample leaves the clip interval, when
	// the query is invalid for the database weights or when the model cannot be updated.
	bool advance( float dtSeconds, const MMSearchInput &query );

	// Re-applies the current pose without advancing or searching. Useful after external scene
	// graph changes. Fails without side effects when the player is inactive.
	bool applyPose();

private:
	// Evaluates the current clip at the current time and writes the full pose into the model:
	// the model node receives the world root (converted to the model parent's space) while the
	// root joint is stripped of root motion (identity local matrix). Returns false without
	// touching the model when the time lies outside the clip interval or the model is missing a
	// skeleton joint.
	bool applyPoseInternal( const Matrix4f &worldRoot );

	MotionDatabase  *_db;
	ModelNode       *_model;
	bool             _active;
	int              _clipIndex;
	float            _time;
	Matrix4f         _alignment;  // Fixed world alignment W * inverse(G_new) of the current clip
};

}  // namespace Horde3D

#endif // _egMotionMatching_H_
