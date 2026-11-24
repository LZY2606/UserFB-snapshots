// This file is part of meshoptimizer library; see meshoptimizer.h for version/license details
#ifndef MESHOPTIMIZER_COLLISION_H
#define MESHOPTIMIZER_COLLISION_H

#include "meshoptimizer.h"

namespace meshopt
{

struct Vector3;

// Observation counters for the continuous self-intersection guard; every counter
// reflects real events inside the simplifier, never quantities derived from output size.
struct CollisionCounters
{
	size_t candidates;       // collapse/update candidates presented to the collision layer
	size_t narrowphase;      // continuous triangle-vs-swept-tetrahedron predicate invocations
	size_t local_updates;    // incremental index mutations (face removals / vertex replacements / moves)
	size_t collapses;        // collapse movements accepted while the guard was active
	size_t position_moves;   // WithUpdate vertex position moves accepted
	size_t collapse_rejected; // edge collapse candidates rejected by the continuous guard
	size_t position_rejected; // WithUpdate position solves rejected by the continuous guard
	size_t index_rebuilds;   // amortized full spatial index rebuilds (not counted as local updates)
};

// Process-wide counters, accumulated across guarded simplification calls.
extern CollisionCounters gCollisionCounters;

struct CollisionWorld;

// Builds a spatially indexed view of the current (alive) triangle set; storage is O(vertex_count + face_count)
// and all allocations go through the simplifier allocator.
CollisionWorld* collisionCreate(const unsigned int* indices, size_t index_count, const Vector3* positions, size_t vertex_count,
                                const unsigned int* remap, const unsigned int* wedge, meshopt_Allocator& allocator,
                                CollisionCounters* counters);
void collisionDestroy(CollisionWorld* world, meshopt_Allocator& allocator);

// Continuous guard for one straight-line move: every vertex whose geometric identity is r0 (all of its
// wedges) travels linearly to target, which carries identity r1 (r0 == r1 for WithUpdate solves).
// Faces sharing a geometric corner identity (through remap/wedge, so seam wedges count as one geometry)
// with any moving face at t=0 are legal vertex/edge adjacency and exempt; every other alive face must
// stay disjoint from the swept volume over the closed interval t in [0,1], contact included.
// Returns true when safe.
bool collisionTestMove(CollisionWorld* world, unsigned int r0, unsigned int r1, const Vector3& target);

// Applies an accepted collapse immediately; r0 is the source geometric identity and collapse_remap uses
// self entries for untouched vertices. Faces that become degenerate are removed, surviving faces get
// their vertex references replaced, and the spatial index is refitted locally before the next candidate.
void collisionCommitCollapse(CollisionWorld* world, unsigned int r0, const unsigned int* collapse_remap);

// Applies an accepted WithUpdate position move immediately; positions were already updated by the caller
// for every wedge of identity r0.
void collisionCommitMove(CollisionWorld* world, unsigned int r0);

} // namespace meshopt

#endif
