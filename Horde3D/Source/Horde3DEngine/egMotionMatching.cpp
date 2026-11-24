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

#include "egMotionMatching.h"
#include "egAnimation.h"
#include "egModel.h"
#include "egAnimatables.h"
#include "egScene.h"
#include "egModules.h"
#include "utMath.h"
#include <cmath>
#include <set>

namespace Horde3D {

using namespace std;


namespace
{
	// Root track tolerance: the root only carries XZ translation and Y rotation.
	const float RootTol = 1.0e-5f;


	bool finiteVec( const Vec3f &v )
	{
		return std::isfinite( v.x ) && std::isfinite( v.y ) && std::isfinite( v.z );
	}


	bool finiteMat( const Matrix4f &m )
	{
		for( uint32 i = 0; i < 16; ++i )
			if( !std::isfinite( m.x[i] ) ) return false;
		return true;
	}


	bool finiteQuat( const Quaternion &q )
	{
		return std::isfinite( q.x ) && std::isfinite( q.y ) && std::isfinite( q.z ) &&
		       std::isfinite( q.w );
	}


	// Extracts the yaw of a matrix whose rotation is guaranteed to be a pure Y rotation.
	// The engine's Y rotation has c[0][0] = cos(yaw) and c[2][0] = sin(yaw).
	float yawFromYRotMat( const Matrix4f &m )
	{
		return atan2f( m.c[2][0], m.c[0][0] );
	}


	// Rotation part of a rigid matrix as a matrix with zero translation.
	Matrix4f rotationPart( const Matrix4f &m )
	{
		Matrix4f r( Math::NO_INIT );
		for( uint32 col = 0; col < 3; ++col )
			for( uint32 row = 0; row < 3; ++row )
				r.c[col][row] = m.c[col][row];
		r.c[3][0] = r.c[3][1] = r.c[3][2] = 0.0f;
		r.c[0][3] = r.c[1][3] = r.c[2][3] = 0.0f;
		r.c[3][3] = 1.0f;
		return r;
	}


	// Builds a local rigid matrix (unit scale) from rotation and translation.
	Matrix4f rigidLocal( const Quaternion &rot, const Vec3f &trans )
	{
		Matrix4f mat( Math::NO_INIT );
		Matrix4f::fastMult43( mat, Matrix4f( rot ), Matrix4f() );
		mat.translate( trans.x, trans.y, trans.z );
		return mat;
	}


	// Converts the rotation part of a rigid (unit-scale) matrix to a quaternion.
	// Storage is column major: R_ij (row i, column j) is m.c[j][i].
	Quaternion quatFromRotMat( const Matrix4f &m )
	{
		const float trace = m.c[0][0] + m.c[1][1] + m.c[2][2];
		Quaternion q;
		if( trace > 0.0f )
		{
			float s = sqrtf( trace + 1.0f ) * 2.0f;  // s = 4w
			q.w = 0.25f * s;
			q.x = ( m.c[1][2] - m.c[2][1] ) / s;
			q.y = ( m.c[2][0] - m.c[0][2] ) / s;
			q.z = ( m.c[0][1] - m.c[1][0] ) / s;
		}
		else if( m.c[0][0] > m.c[1][1] && m.c[0][0] > m.c[2][2] )
		{
			float s = sqrtf( 1.0f + m.c[0][0] - m.c[1][1] - m.c[2][2] ) * 2.0f;  // s = 4x
			q.w = ( m.c[1][2] - m.c[2][1] ) / s;
			q.x = 0.25f * s;
			q.y = ( m.c[1][0] + m.c[0][1] ) / s;
			q.z = ( m.c[2][0] + m.c[0][2] ) / s;
		}
		else if( m.c[1][1] > m.c[2][2] )
		{
			float s = sqrtf( 1.0f + m.c[1][1] - m.c[0][0] - m.c[2][2] ) * 2.0f;  // s = 4y
			q.w = ( m.c[2][0] - m.c[0][2] ) / s;
			q.x = ( m.c[1][0] + m.c[0][1] ) / s;
			q.y = 0.25f * s;
			q.z = ( m.c[2][1] + m.c[1][2] ) / s;
		}
		else
		{
			float s = sqrtf( 1.0f + m.c[2][2] - m.c[0][0] - m.c[1][1] ) * 2.0f;  // s = 4z
			q.w = ( m.c[0][1] - m.c[1][0] ) / s;
			q.x = ( m.c[2][0] + m.c[0][2] ) / s;
			q.y = ( m.c[2][1] + m.c[1][2] ) / s;
			q.z = 0.25f * s;
		}
		return q;
	}


	// Recursively collects the model's joints by name.
	void collectJoints( SceneNode *node, vector< JointNode * > &joints )
	{
		if( node->getType() == SceneNodeTypes::Joint )
			joints.push_back( (JointNode *)node );
		vector< SceneNode * > &children = node->getChildren();
		for( uint32 i = 0; i < (uint32)children.size(); ++i )
			collectJoints( children[i], joints );
	}
}


// =================================================================================================
// Motion database
// =================================================================================================

MotionDatabase::MotionDatabase() :
	_built( false ), _horizon( 0.0f )
{
	_skeleton.rootIndex = -1;
}


MotionDatabase::~MotionDatabase()
{
}


bool MotionDatabase::sampleLocal( const ClipData &clip, uint32 joint, float t,
                                  Quaternion &rot, Vec3f &trans ) const
{
	const JointTrack &track = clip.tracks[joint];

	if( track.constant )
	{
		// A track compressed to one frame is a constant.
		rot = track.constRot;
		trans = track.constTrans;
		return true;
	}

	const uint32 frameCount = clip.frameCount;
	float fframe = t * clip.fps;
	int f0 = ftoi_t( fframe );
	float amount = fframe - (float)f0;
	if( f0 < 0 ) f0 = 0;
	if( (uint32)f0 >= frameCount ) f0 = (int)frameCount - 1;
	if( f0 == (int)frameCount - 1 ) amount = 0.0f;  // Closed interval: last frame never wraps
	uint32 f1 = f0 + 1;
	if( f1 > frameCount - 1 ) f1 = frameCount - 1;

	// Resource rules: linear translation, shortest-arc nlerp rotation.
	trans = track.translations[f0].lerp( track.translations[f1], amount );
	rot = track.rotations[f0].nlerp( track.rotations[f1], amount );
	return true;
}


bool MotionDatabase::samplePose( const SkeletonData &skel, const ClipData &clip, float t,
                                 vector< Matrix4f > &localMats,
                                 vector< Matrix4f > &modelMats, Matrix4f &rootMat ) const
{
	const uint32 n = (uint32)skel.names.size();
	localMats.resize( n );
	modelMats.resize( n );

	for( uint32 j = 0; j < n; ++j )
	{
		Quaternion rot;
		Vec3f trans;
		if( !sampleLocal( clip, j, t, rot, trans ) ) return false;
		localMats[j] = rigidLocal( rot, trans );
		if( !finiteMat( localMats[j] ) ) return false;
	}

	// Model-space accumulation. Joints are not required to be parent-before-child, so each chain
	// is walked to the root independently.
	for( uint32 j = 0; j < n; ++j )
	{
		vector< uint32 > chain;
		int cur = (int)j;
		while( cur != -1 )
		{
			chain.push_back( (uint32)cur );
			cur = skel.parents[cur];
		}
		Matrix4f acc;
		for( int k = (int)chain.size() - 1; k >= 0; --k )
		{
			Matrix4f next( Math::NO_INIT );
			Matrix4f::fastMult43( next, acc, localMats[chain[k]] );
			acc = next;
		}
		modelMats[j] = acc;
	}

	rootMat = modelMats[skel.rootIndex];
	return finiteMat( rootMat );
}


const MotionDatabase::Candidate *MotionDatabase::findCandidate( int clipIndex,
                                                                 float sourceTime ) const
{
	for( uint32 i = 0; i < (uint32)_candidates.size(); ++i )
	{
		const Candidate &c = _candidates[i];
		if( c.clipIndex == clipIndex && fabsf( c.sourceTime - sourceTime ) <= Math::ZeroEpsilon )
			return &c;
	}
	return 0x0;
}


bool MotionDatabase::build( const MMSkeleton &skeleton, const vector< MMClip > &clips,
                            const vector< string > &featureJointNames, float horizonSeconds )
{
	// A build replaces the whole database and a failed build must leave it empty, so reset every
	// piece of state before any validation can fail. All work below happens on local objects.
	_built = false;
	_horizon = 0.0f;
	_skeleton = SkeletonData();
	_skeleton.rootIndex = -1;
	_featureJoints.clear();
	_featureJointNames.clear();
	_clips.clear();
	_candidates.clear();

	if( !std::isfinite( horizonSeconds ) || horizonSeconds <= 0.0f )
	{
		Modules::setError( "MotionDatabase: horizon must be a positive finite number of seconds" );
		return false;
	}
	if( skeleton.joints.empty() )
	{
		Modules::setError( "MotionDatabase: skeleton has no joints" );
		return false;
	}
	if( clips.empty() )
	{
		Modules::setError( "MotionDatabase: at least one clip is required" );
		return false;
	}
	if( featureJointNames.empty() )
	{
		Modules::setError( "MotionDatabase: at least one feature joint is required" );
		return false;
	}

	const uint32 numJoints = (uint32)skeleton.joints.size();

	SkeletonData skel;
	skel.names.resize( numJoints );
	skel.parents.resize( numJoints );
	skel.restLocal.resize( numJoints );
	skel.rootIndex = -1;

	// Unique names, finite rest transforms and a single root
	{
		set< string > names;
		for( uint32 i = 0; i < numJoints; ++i )
		{
			if( skeleton.joints[i].name.empty() ||
			    !names.insert( skeleton.joints[i].name ).second )
			{
				Modules::setError( "MotionDatabase: duplicate or empty joint name in skeleton" );
				return false;
			}
			if( !finiteMat( skeleton.joints[i].restLocal.m ) )
			{
				Modules::setError( "MotionDatabase: non-finite rest transform in skeleton" );
				return false;
			}
			skel.names[i] = skeleton.joints[i].name;
			skel.parents[i] = skeleton.joints[i].parentIndex;
			skel.restLocal[i] = skeleton.joints[i].restLocal.m;
			if( skeleton.joints[i].parentIndex == -1 )
			{
				if( skel.rootIndex != -1 )
				{
					Modules::setError( "MotionDatabase: skeleton has more than one root joint" );
					return false;
				}
				skel.rootIndex = (int)i;
			}
		}
	}
	if( skel.rootIndex == -1 )
	{
		Modules::setError( "MotionDatabase: skeleton has no root joint" );
		return false;
	}

	// Parent range and cycle / reachability checks
	for( uint32 i = 0; i < numJoints; ++i )
	{
		if( (int)i == skel.rootIndex ) continue;
		if( skel.parents[i] < 0 || (uint32)skel.parents[i] >= numJoints )
		{
			Modules::setError( "MotionDatabase: joint parent index out of range" );
			return false;
		}
	}
	{
		vector< bool > reachable( numJoints, false );
		for( uint32 start = 0; start < numJoints; ++start )
		{
			vector< bool > path( numJoints, false );
			int cur = (int)start;
			while( cur != -1 )
			{
				if( path[cur] )
				{
					Modules::setError( "MotionDatabase: cycle detected in skeleton hierarchy" );
					return false;
				}
				path[cur] = true;
				reachable[cur] = true;
				cur = skel.parents[cur];
			}
		}
		for( uint32 i = 0; i < numJoints; ++i )
		{
			if( !reachable[i] )
			{
				Modules::setError( "MotionDatabase: joint is not reachable from the root" );
				return false;
			}
		}
	}

	// Feature joints: known and unique
	vector< uint32 > featureJoints;
	featureJoints.reserve( featureJointNames.size() );
	{
		set< string > seen;
		for( uint32 i = 0; i < (uint32)featureJointNames.size(); ++i )
		{
			if( !seen.insert( featureJointNames[i] ).second )
			{
				Modules::setError( "MotionDatabase: duplicate feature joint name" );
				return false;
			}
			int found = -1;
			for( uint32 j = 0; j < numJoints; ++j )
			{
				if( skel.names[j] == featureJointNames[i] ) { found = (int)j; break; }
			}
			if( found < 0 )
			{
				Modules::setError( "MotionDatabase: unknown feature joint name" );
				return false;
			}
			featureJoints.push_back( (uint32)found );
		}
	}

	vector< ClipData > builtClips;
	builtClips.reserve( clips.size() );

	for( uint32 ci = 0; ci < (uint32)clips.size(); ++ci )
	{
		AnimationResource *anim = clips[ci].anim;
		const float fps = clips[ci].fps;
		if( anim == 0x0 || !anim->isLoaded() )
		{
			Modules::setError( "MotionDatabase: clip animation resource is null or not loaded" );
			return false;
		}
		if( !std::isfinite( fps ) || fps <= 0.0f )
		{
			Modules::setError( "MotionDatabase: clip frame rate must be positive and finite" );
			return false;
		}

		ClipData clip;
		clip.fps = fps;
		clip.tracks.resize( numJoints );
		clip.frameCount = 0;

		// A clip may not contain tracks for joints that do not belong to this skeleton.
		for( uint32 e = 0; e < (uint32)anim->_entities.size(); ++e )
		{
			bool known = false;
			for( uint32 j = 0; j < numJoints; ++j )
			{
				if( anim->_entities[e].nameId ==
				    AnimationController::hashName( skel.names[j].c_str() ) )
				{
					known = true;
					break;
				}
			}
			if( !known )
			{
				Modules::setError( "MotionDatabase: animation contains an unknown joint track" );
				return false;
			}
		}

		for( uint32 j = 0; j < numJoints; ++j )
		{
			AnimResEntity *ent = anim->findEntity(
				AnimationController::hashName( skel.names[j].c_str() ) );
			JointTrack &track = clip.tracks[j];

			if( ent == 0x0 || ent->frames.empty() )
			{
				// Missing track: the joint keeps this skeleton's rest local transform for the
				// whole clip, expressed as a constant rigid pose (animation scales are one).
				track.constant = true;
				track.constRot = quatFromRotMat( skel.restLocal[j] );
				track.constTrans = skel.restLocal[j].getTrans();
				if( !finiteQuat( track.constRot ) || !finiteVec( track.constTrans ) )
				{
					Modules::setError( "MotionDatabase: non-finite rest pose data" );
					return false;
				}
				continue;
			}

			for( uint32 f = 0; f < (uint32)ent->frames.size(); ++f )
			{
				const Quaternion &q = ent->frames[f].rotQuat;
				const Vec3f &s = ent->frames[f].scaleVec;
				if( !finiteQuat( q ) || !finiteVec( ent->frames[f].transVec ) || !finiteVec( s ) )
				{
					Modules::setError( "MotionDatabase: non-finite animation data" );
					return false;
				}
				// Animation scales are always one and rotations are unit quaternions.
				if( fabsf( s.x - 1.0f ) > RootTol || fabsf( s.y - 1.0f ) > RootTol ||
				    fabsf( s.z - 1.0f ) > RootTol ||
				    fabsf( q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w - 1.0f ) > RootTol )
				{
					Modules::setError( "MotionDatabase: animation scale must be one and rotation must be a unit quaternion" );
					return false;
				}
			}

			if( ent->frames.size() == 1 )
			{
				// Compressed single-frame track: constant.
				track.constant = true;
				track.constRot = ent->frames[0].rotQuat;
				track.constTrans = ent->frames[0].transVec;
			}
			else
			{
				track.constant = false;
				track.rotations.resize( ent->frames.size() );
				track.translations.resize( ent->frames.size() );
				for( uint32 f = 0; f < (uint32)ent->frames.size(); ++f )
				{
					track.rotations[f] = ent->frames[f].rotQuat;
					track.translations[f] = ent->frames[f].transVec;
				}
				if( clip.frameCount == 0 )
					clip.frameCount = (uint32)ent->frames.size();
				else if( clip.frameCount != (uint32)ent->frames.size() )
				{
					Modules::setError( "MotionDatabase: inconsistent frame counts in animation tracks" );
					return false;
				}
			}
		}

		if( clip.frameCount < 2 )
		{
			Modules::setError( "MotionDatabase: clip has no multi-frame track and cannot provide candidates" );
			return false;
		}
		clip.duration = (float)( clip.frameCount - 1 ) / clip.fps;

		// The root must have a real track and every frame must be a legal root transform.
		const JointTrack &rootTrack = clip.tracks[skel.rootIndex];
		if( rootTrack.constant )
		{
			Modules::setError( "MotionDatabase: root track must span the clip frames" );
			return false;
		}
		for( uint32 f = 0; f < clip.frameCount; ++f )
		{
			const Quaternion &q = rootTrack.rotations[f];
			const Vec3f &tr = rootTrack.translations[f];
			if( fabsf( tr.y ) > RootTol ||
			    fabsf( q.x ) > RootTol || fabsf( q.z ) > RootTol )
			{
				Modules::setError( "MotionDatabase: illegal root transform (only XZ translation and Y rotation allowed)" );
				return false;
			}
		}

		builtClips.push_back( clip );
	}

	// Candidate generation
	vector< Candidate > candidates;

	for( uint32 ci = 0; ci < (uint32)builtClips.size(); ++ci )
	{
		const ClipData &clip = builtClips[ci];
		const float clipEnd = clip.duration;
		const float delta = 1.0f / clip.fps;

		for( uint32 frame = 0; frame < clip.frameCount; ++frame )
		{
			const float t = (float)frame / clip.fps;

			// One frame of margin on both sides and room for the whole horizon.
			if( t - delta < -RootTol || t + delta > clipEnd + RootTol ||
			    t + horizonSeconds > clipEnd + RootTol )
				continue;

			vector< Matrix4f > localsT, modelT, modelMinus, modelPlus, modelH;
			Matrix4f rootT, rootH;
			{
				vector< Matrix4f > unused;
				Matrix4f unusedRoot;
				if( !samplePose( skel, clip, t, localsT, modelT, rootT ) ||
				    !samplePose( skel, clip, t - delta, unused, modelMinus, unusedRoot ) ||
				    !samplePose( skel, clip, t + delta, unused, modelPlus, unusedRoot ) ||
				    !samplePose( skel, clip, t + horizonSeconds, unused, modelH, rootH ) )
				{
					Modules::setError( "MotionDatabase: failed to evaluate animation sample" );
					return false;
				}
			}

			for( uint32 k = 0; k < numJoints; ++k )
			{
				if( !finiteMat( modelT[k] ) || !finiteMat( modelMinus[k] ) ||
				    !finiteMat( modelPlus[k] ) || !finiteMat( modelH[k] ) )
				{
					Modules::setError( "MotionDatabase: non-finite pose data" );
					return false;
				}
			}

			const Matrix4f invRootT = rootT.inverted();
			const Matrix4f invRootTRot = rotationPart( invRootT );

			Candidate cand;
			cand.clipIndex = (int)ci;
			cand.frame = frame;
			cand.sourceTime = t;
			cand.positions.resize( featureJoints.size() );
			cand.velocities.resize( featureJoints.size() );

			for( uint32 f = 0; f < (uint32)featureJoints.size(); ++f )
			{
				const uint32 j = featureJoints[f];
				const Vec3f pT = modelT[j].getTrans();
				const Vec3f pMinus = modelMinus[j].getTrans();
				const Vec3f pPlus = modelPlus[j].getTrans();

				// Position feature: inverse(G(t)) * p_j(t)
				cand.positions[f] = invRootT * pT;

				// Velocity feature: the model-space central difference is computed from the two
				// unmodified model-space points and only then moved into the current root frame.
				const Vec3f modelVel = ( pPlus - pMinus ) * ( 1.0f / ( 2.0f * delta ) );
				cand.velocities[f] = invRootTRot.mult33Vec( modelVel );

				if( !finiteVec( cand.positions[f] ) || !finiteVec( cand.velocities[f] ) )
				{
					Modules::setError( "MotionDatabase: non-finite feature data" );
					return false;
				}
			}

			// Future feature: inverse(G(t)) * G(t+h), XZ translation and yaw.
			const Matrix4f futureRoot = invRootT * rootH;
			if( !finiteMat( futureRoot ) )
			{
				Modules::setError( "MotionDatabase: non-finite future root data" );
				return false;
			}
			cand.futureDisp = futureRoot.getTrans();
			cand.futureDisp.y = 0.0f;
			cand.futureYaw = yawFromYRotMat( futureRoot );
			if( !finiteVec( cand.futureDisp ) || !std::isfinite( cand.futureYaw ) )
			{
				Modules::setError( "MotionDatabase: non-finite future feature data" );
				return false;
			}

			candidates.push_back( cand );
		}
	}

	if( candidates.empty() )
	{
		Modules::setError( "MotionDatabase: configuration produces no legal candidates" );
		return false;
	}

	// Commit: the database now owns all data and no longer touches the animation resources.
	_built = true;
	_horizon = horizonSeconds;
	_skeleton = skel;
	_featureJoints = featureJoints;
	_featureJointNames = featureJointNames;
	_clips = builtClips;
	_candidates = candidates;
	return true;
}


bool MotionDatabase::getCandidate( int candidateIndex, MMCandidateInfo &out ) const
{
	if( !_built )
	{
		Modules::setError( "MotionDatabase::getCandidate: database is not built" );
		return false;
	}
	if( candidateIndex < 0 || candidateIndex >= (int)_candidates.size() )
	{
		Modules::setError( "MotionDatabase::getCandidate: candidate index out of range" );
		return false;
	}

	const Candidate &c = _candidates[candidateIndex];
	MMCandidateInfo info;
	info.clipIndex = c.clipIndex;
	info.frame = c.frame;
	info.sourceTime = c.sourceTime;
	info.positions.resize( c.positions.size() );
	info.velocities.resize( c.velocities.size() );
	for( uint32 i = 0; i < (uint32)c.positions.size(); ++i )
	{
		info.positions[i] = MMVec3( c.positions[i] );
		info.velocities[i] = MMVec3( c.velocities[i] );
	}
	info.futureDisplacement = MMVec3( c.futureDisp );
	info.futureYaw = c.futureYaw;
	out = info;
	return true;
}


float MotionDatabase::getClipDuration( int clipIndex ) const
{
	if( !_built || clipIndex < 0 || clipIndex >= (int)_clips.size() ) return 0.0f;
	return _clips[clipIndex].duration;
}


bool MotionDatabase::search( const MMSearchInput &input, MMSearchResult &result ) const
{
	if( !_built )
	{
		Modules::setError( "MotionDatabase::search: database is not built" );
		return false;
	}
	const uint32 numFeatures = (uint32)_featureJointNames.size();
	if( input.positions.size() != numFeatures || input.velocities.size() != numFeatures )
	{
		Modules::setError( "MotionDatabase::search: query array size does not match feature joint count" );
		return false;
	}
	if( !std::isfinite( input.futureYaw ) || !std::isfinite( input.futureDisplacement.x ) ||
	    !std::isfinite( input.futureDisplacement.z ) )
	{
		Modules::setError( "MotionDatabase::search: non-finite query data" );
		return false;
	}
	for( uint32 i = 0; i < numFeatures; ++i )
	{
		if( !std::isfinite( input.positions[i].x ) || !std::isfinite( input.positions[i].y ) ||
		    !std::isfinite( input.positions[i].z ) ||
		    !std::isfinite( input.velocities[i].x ) || !std::isfinite( input.velocities[i].y ) ||
		    !std::isfinite( input.velocities[i].z ) )
		{
			Modules::setError( "MotionDatabase::search: non-finite query data" );
			return false;
		}
	}
	const MMWeights &w = input.weights;
	if( !std::isfinite( w.position ) || !std::isfinite( w.velocity ) ||
	    !std::isfinite( w.trajectory ) || !std::isfinite( w.facing ) )
	{
		Modules::setError( "MotionDatabase::search: non-finite weights" );
		return false;
	}
	if( w.position < 0.0f || w.velocity < 0.0f || w.trajectory < 0.0f || w.facing < 0.0f )
	{
		Modules::setError( "MotionDatabase::search: weights must be non-negative" );
		return false;
	}
	if( w.position == 0.0f && w.velocity == 0.0f && w.trajectory == 0.0f &&
	    w.facing == 0.0f )
	{
		Modules::setError( "MotionDatabase::search: at least one weight must be non-zero" );
		return false;
	}

	const float qCos = cosf( input.futureYaw );
	const float qSin = sinf( input.futureYaw );

	int bestIndex = -1;
	float bestCost = 0.0f;

	// Candidates are stored clip by clip and frame ascending, so a strict smaller-than comparison
	// resolves equal costs by clip index and then source frame.
	for( uint32 ci = 0; ci < (uint32)_candidates.size(); ++ci )
	{
		const Candidate &c = _candidates[ci];
		float cost = 0.0f;

		for( uint32 f = 0; f < numFeatures; ++f )
		{
			const float dpx = c.positions[f].x - input.positions[f].x;
			const float dpy = c.positions[f].y - input.positions[f].y;
			const float dpz = c.positions[f].z - input.positions[f].z;
			cost += w.position * ( dpx * dpx + dpy * dpy + dpz * dpz );

			const float dvx = c.velocities[f].x - input.velocities[f].x;
			const float dvy = c.velocities[f].y - input.velocities[f].y;
			const float dvz = c.velocities[f].z - input.velocities[f].z;
			cost += w.velocity * ( dvx * dvx + dvy * dvy + dvz * dvz );
		}

		const float ddx = c.futureDisp.x - input.futureDisplacement.x;
		const float ddz = c.futureDisp.z - input.futureDisplacement.z;
		cost += w.trajectory * ( ddx * ddx + ddz * ddz );

		const float dcx = cosf( c.futureYaw ) - qCos;
		const float dsz = sinf( c.futureYaw ) - qSin;
		cost += w.facing * ( dcx * dcx + dsz * dsz );

		if( !std::isfinite( cost ) )
		{
			Modules::setError( "MotionDatabase::search: cost became non-finite" );
			return false;
		}

		if( bestIndex < 0 || cost < bestCost )
		{
			bestCost = cost;
			bestIndex = (int)ci;
		}
	}

	MMSearchResult out;
	out.clipIndex = _candidates[bestIndex].clipIndex;
	out.sourceTime = _candidates[bestIndex].sourceTime;
	out.cost = bestCost;
	result = out;
	return true;
}


// =================================================================================================
// Motion player
// =================================================================================================

MotionPlayer::MotionPlayer() :
	_db( 0x0 ), _model( 0x0 ), _active( false ), _clipIndex( -1 ), _time( 0.0f )
{
}


MotionPlayer::~MotionPlayer()
{
}


MMMat4 MotionPlayer::getWorldRoot() const
{
	if( !_active ) return MMMat4();

	vector< Matrix4f > locals, models;
	Matrix4f sourceRoot;
	const MotionDatabase::ClipData &clip = _db->_clips[_clipIndex];
	if( !_db->samplePose( _db->_skeleton, clip, _time, locals, models, sourceRoot ) )
		return MMMat4( _alignment );

	return MMMat4( _alignment * sourceRoot );
}


MMMat4 MotionPlayer::getAlignment() const
{
	return MMMat4( _alignment );
}


bool MotionPlayer::applyPoseInternal( const Matrix4f &worldRoot )
{
	// Resolve the model's joints once; a missing skeleton joint fails before anything is written.
	vector< JointNode * > modelJoints;
	collectJoints( _model, modelJoints );

	vector< JointNode * > joints( _db->_skeleton.names.size(), 0x0 );
	for( uint32 i = 0; i < (uint32)modelJoints.size(); ++i )
	{
		for( uint32 j = 0; j < (uint32)_db->_skeleton.names.size(); ++j )
		{
			if( modelJoints[i]->getName() == _db->_skeleton.names[j] )
				joints[j] = modelJoints[i];
		}
	}
	for( uint32 j = 0; j < (uint32)joints.size(); ++j )
	{
		if( joints[j] == 0x0 )
		{
			Modules::setError( "MotionPlayer: model is missing a skeleton joint" );
			return false;
		}
	}

	const MotionDatabase::ClipData &clip = _db->_clips[_clipIndex];
	vector< Matrix4f > locals, models;
	Matrix4f sourceRoot;
	if( !_db->samplePose( _db->_skeleton, clip, _time, locals, models, sourceRoot ) )
	{
		Modules::setError( "MotionPlayer: failed to sample the current pose" );
		return false;
	}

	// World root is converted into the model parent's coordinate space. The parent may carry a
	// non-identity rigid transform and its absolute matrix may still be dirty, so settle the
	// scene graph first.
	Modules::sceneMan().updateNodes();
	SceneNode *parentNode = _model->getParent();
	if( parentNode == 0x0 )
	{
		Modules::setError( "MotionPlayer: model node has no parent" );
		return false;
	}
	Matrix4f modelRel = parentNode->getAbsTrans().inverted() * worldRoot;
	if( !finiteMat( modelRel ) )
	{
		Modules::setError( "MotionPlayer: non-finite model transform" );
		return false;
	}

	// From here on the pose is written out. The root joint receives the identity so that root
	// motion lives only on the model node, never on both.
	_model->setTransform( modelRel );
	for( uint32 j = 0; j < (uint32)joints.size(); ++j )
	{
		if( (int)j == _db->_skeleton.rootIndex )
			joints[j]->setTransform( Matrix4f() );
		else
			joints[j]->setTransform( locals[j] );
	}

	// Refresh absolute transforms, JointNode::_relModelMat and the inverse-bind palette.
	Modules::sceneMan().updateNodes();

	// Run the existing software skinning path (morphers are still applied there as well).
	_model->update( ModelUpdateFlags::Geometry );
	return true;
}


bool MotionPlayer::init( MotionDatabase &database, ModelNode *model, int startClipIndex,
                         float startTimeSeconds, const MMMat4 &worldRoot )
{
	if( !database.isBuilt() )
	{
		Modules::setError( "MotionPlayer::init: database is not built" );
		return false;
	}
	if( model == 0x0 )
	{
		Modules::setError( "MotionPlayer::init: model node must not be null" );
		return false;
	}
	if( model->getType() != SceneNodeTypes::Model )
	{
		Modules::setError( "MotionPlayer::init: node is not a model" );
		return false;
	}
	if( !std::isfinite( startTimeSeconds ) || startTimeSeconds < 0.0f ||
	    !finiteMat( worldRoot.m ) )
	{
		Modules::setError( "MotionPlayer::init: invalid start time or world root" );
		return false;
	}
	if( startClipIndex < 0 || startClipIndex >= database.getClipCount() )
	{
		Modules::setError( "MotionPlayer::init: clip index out of range" );
		return false;
	}

	const MotionDatabase::Candidate *startCand =
		database.findCandidate( startClipIndex, startTimeSeconds );
	if( startCand == 0x0 )
	{
		Modules::setError( "MotionPlayer::init: start point is not a legal database candidate" );
		return false;
	}

	// Verify the model provides every skeleton joint before mutating any player state.
	vector< JointNode * > modelJoints;
	collectJoints( model, modelJoints );
	for( uint32 j = 0; j < (uint32)database._skeleton.names.size(); ++j )
	{
		bool found = false;
		for( uint32 k = 0; k < (uint32)modelJoints.size(); ++k )
		{
			if( modelJoints[k]->getName() == database._skeleton.names[j] ) { found = true; break; }
		}
		if( !found )
		{
			Modules::setError( "MotionPlayer::init: model is missing a skeleton joint" );
			return false;
		}
	}

	// Derive the fixed alignment W * inverse(G_new) from the start candidate's source root.
	vector< Matrix4f > locals, models;
	Matrix4f sourceRoot;
	const MotionDatabase::ClipData &clip = database._clips[startClipIndex];
	if( !database.samplePose( database._skeleton, clip, startTimeSeconds, locals, models, sourceRoot ) )
	{
		Modules::setError( "MotionPlayer::init: failed to sample the start pose" );
		return false;
	}

	_db = &database;
	_model = model;
	_clipIndex = startClipIndex;
	_time = startTimeSeconds;
	_alignment = worldRoot.m * sourceRoot.inverted();
	_active = true;

	if( !applyPoseInternal( worldRoot.m ) )
	{
		// Roll back all player state; the model keeps whatever pose it had before init.
		_active = false;
		_db = 0x0;
		_model = 0x0;
		_clipIndex = -1;
		_time = 0.0f;
		_alignment = Matrix4f();
		return false;
	}
	return true;
}


bool MotionPlayer::advance( float dtSeconds, const MMSearchInput &query )
{
	if( !_active )
	{
		Modules::setError( "MotionPlayer::advance: player is not initialized" );
		return false;
	}
	if( !std::isfinite( dtSeconds ) || dtSeconds < 0.0f )
	{
		Modules::setError( "MotionPlayer::advance: dt must be a non-negative finite number of seconds" );
		return false;
	}

	const MotionDatabase::ClipData &oldClip = _db->_clips[_clipIndex];
	const float delta = 1.0f / oldClip.fps;
	const float advancedTime = _time + dtSeconds;

	// The evaluation point and both central-difference samples must stay inside the closed
	// interval of the current clip. Nothing wraps or clamps.
	if( advancedTime < -RootTol || advancedTime > oldClip.duration + RootTol ||
	    advancedTime - delta < -RootTol || advancedTime + delta > oldClip.duration + RootTol )
	{
		Modules::setError( "MotionPlayer::advance: step leaves the valid interval of the current clip" );
		return false;
	}

	// Actual pose at the advanced time and the two difference poses, all evaluated before any
	// state change.
	vector< Matrix4f > localsT, modelT, localsM, modelMinus, modelsP, modelPlus;
	Matrix4f rootT, rootUnused;
	if( !_db->samplePose( _db->_skeleton, oldClip, advancedTime, localsT, modelT, rootT ) ||
	    !_db->samplePose( _db->_skeleton, oldClip, advancedTime - delta, localsM, modelMinus, rootUnused ) ||
	    !_db->samplePose( _db->_skeleton, oldClip, advancedTime + delta, modelsP, modelPlus, rootUnused ) )
	{
		Modules::setError( "MotionPlayer::advance: failed to sample the current clip" );
		return false;
	}

	// Feature extraction in the advanced current root frame G(t).
	const Matrix4f invRoot = rootT.inverted();
	const Matrix4f invRootRot = rotationPart( invRoot );

	MMSearchInput effectiveQuery = query;
	effectiveQuery.positions.resize( _db->_featureJoints.size() );
	effectiveQuery.velocities.resize( _db->_featureJoints.size() );
	for( uint32 f = 0; f < (uint32)_db->_featureJoints.size(); ++f )
	{
		const uint32 j = _db->_featureJoints[f];
		const Vec3f pT = modelT[j].getTrans();
		const Vec3f pMinus = modelMinus[j].getTrans();
		const Vec3f pPlus = modelPlus[j].getTrans();

		effectiveQuery.positions[f] = MMVec3( invRoot * pT );
		const Vec3f modelVel = ( pPlus - pMinus ) * ( 1.0f / ( 2.0f * delta ) );
		effectiveQuery.velocities[f] = MMVec3( invRootRot.mult33Vec( modelVel ) );
	}

	// Search with the caller's future trajectory and four weights.
	MMSearchResult searchResult;
	if( !_db->search( effectiveQuery, searchResult ) )
		return false;  // search() already reported the error and changed nothing

	// Resolve the selected source root G_new and compute the fixed alignment.
	const MotionDatabase::ClipData &newClip = _db->_clips[searchResult.clipIndex];
	vector< Matrix4f > newLocals, newModels;
	Matrix4f newSourceRoot;
	if( !_db->samplePose( _db->_skeleton, newClip, searchResult.sourceTime, newLocals, newModels,
	                      newSourceRoot ) )
	{
		Modules::setError( "MotionPlayer::advance: failed to sample the selected clip" );
		return false;
	}

	// Advanced old world root W; the new alignment maps the candidate root frame onto it exactly.
	const Matrix4f advancedWorldRoot = _alignment * rootT;
	const Matrix4f newAlignment = advancedWorldRoot * newSourceRoot.inverted();
	if( !finiteMat( newAlignment ) )
	{
		Modules::setError( "MotionPlayer::advance: non-finite alignment matrix" );
		return false;
	}

	// Commit the switch (root pose continuity only; no inertial blending is attempted).
	const int oldClipIndex = _clipIndex;
	const float oldTime = _time;
	Matrix4f oldAlignment = _alignment;

	_clipIndex = searchResult.clipIndex;
	_time = searchResult.sourceTime;
	_alignment = newAlignment;

	if( !applyPoseInternal( advancedWorldRoot ) )
	{
		// Restore the previous state on failure; no half-written animation stage remains.
		_clipIndex = oldClipIndex;
		_time = oldTime;
		_alignment = oldAlignment;
		return false;
	}
	return true;
}


bool MotionPlayer::applyPose()
{
	if( !_active )
	{
		Modules::setError( "MotionPlayer::applyPose: player is not initialized" );
		return false;
	}

	vector< Matrix4f > locals, models;
	Matrix4f sourceRoot;
	const MotionDatabase::ClipData &clip = _db->_clips[_clipIndex];
	if( !_db->samplePose( _db->_skeleton, clip, _time, locals, models, sourceRoot ) )
	{
		Modules::setError( "MotionPlayer::applyPose: failed to sample the current pose" );
		return false;
	}
	return applyPoseInternal( _alignment * sourceRoot );
}

}  // namespace Horde3D
