// TinyScene node-removal / empty-TLAS tests.
//
// This file has no main(): the cases are registered through
// task_support/check.h, and the task_support driver runs them in the topology,
// lights and empty_reuse groups. Every assertion checks a real result with
// TASK_CHECK.
//
// Build and run:  bash task_support/run.sh target

#include "scene_api.hpp"
#include "check.h"
#include <cstdio>

namespace remove_test
{
using namespace tinyscene;

static bool Near( const float a, const float b, const float eps = 1e-4f )
{
	return std::fabs( a - b ) <= eps;
}
static bool Near( const ts_vec3& a, const ts_vec3& b, const float eps = 1e-4f )
{
	return Near( a.x, b.x, eps ) && Near( a.y, b.y, eps ) && Near( a.z, b.z, eps );
}

// Column-major translation matrix (tiny_scene's ts_mat4 / tinybvh::bvhmat4 layout).
static ts_mat4 Translate( const ts_vec3& p )
{
	ts_mat4 m;
	m[3] = p.x, m[7] = p.y, m[11] = p.z;
	return m;
}

// Column-major non-uniform scale matrix.
static ts_mat4 Scale( const ts_vec3& s )
{
	ts_mat4 m;
	m[0] = s.x, m[5] = s.y, m[10] = s.z;
	return m;
}

// Transform a point by a matrix, using the same convention as tiny_scene's
// ts_transform_point (row-vector access into the column-major matrix).
static ts_vec3 TransformPoint( const ts_vec3& v, const ts_mat4& T )
{
	return ts_vec3(
		T[0] * v.x + T[1] * v.y + T[2] * v.z + T[3],
		T[4] * v.x + T[5] * v.y + T[6] * v.z + T[7],
		T[8] * v.x + T[9] * v.y + T[10] * v.z + T[11] );
}

static float Length( const ts_vec3& v ) { return sqrtf( v.x * v.x + v.y * v.y + v.z * v.z ); }

// A small centred quad in the z = 0 plane:
//   t0 = (-h,-h,0), (h,-h,0), (h,h,0)   -> local centre (h/3, -h/3, 0), area h*h
//   t1 = (-h,-h,0), (h,h,0), (-h,h,0)
static int MakeQuadMesh( const int material, const float h = 1.0f )
{
	const int mesh = Scene::AddMesh( 0 );
	Scene::AddTriToMesh( mesh, ts_vec3( -h, -h, 0 ), ts_vec3( h, -h, 0 ), ts_vec3( h, h, 0 ), material );
	Scene::AddTriToMesh( mesh, ts_vec3( -h, -h, 0 ), ts_vec3( h, h, 0 ), ts_vec3( -h, h, 0 ), material );
	Scene::meshPool[mesh]->BuildMaterialList();
	return mesh;
}

// World position of a node-local point, using the combined transform from the
// last UpdateSceneGraph walk.
static ts_vec3 WorldOf( const int nodeId, const ts_vec3& local )
{
	return TransformPoint( local, Scene::nodePool[nodeId]->combinedTransform );
}

// Cast a ray from the origin plane along +z; return the node id behind the
// nearest hit (via the TLAS instance table), or -1 when nothing is hit.
static int Shoot( const float x, const float y )
{
	tinybvh::Ray ray( tinybvh::bvhvec3( x, y, 0 ), tinybvh::bvhvec3( 0, 0, 1 ) );
	Scene::tlas->Intersect( ray );
	if ( ray.hit.t >= 1e30f ) return -1;
	return Scene::NodeIdFromInstance( (unsigned)ray.hit.inst );
}

// True when a ray at world position (x, y) hits the given node first.
static bool ShootHitsNode( const int nodeId, const float x, const float y )
{
	return Shoot( x, y ) == nodeId;
}

// Remove all root nodes so a case starts from an empty scene graph. (The groups
// share one Scene; each case only relies on its own nodes.)
static void ClearRoots()
{
	const std::vector<int> roots = Scene::rootNodes;
	for ( int id : roots ) Scene::RemoveNode( id );
	Scene::UpdateSceneGraph( 0.0f );
}

// ---------------------------------------------------------------- topology
// A root with descendants: RemoveNode removes the entire subtree.
static int TopologyRootSubtree()
{
	ClearRoots();
	const int mat = Scene::AddMaterial( ts_vec3( 0.6f ), "topology-normal" );
	const int mesh = MakeQuadMesh( mat );
	const ts_vec3 localCentre( 1.0f / 3.0f, -1.0f / 3.0f, 0 );

	// World x positions: root -4, child 0, grandchild 4, survivor 8 (y = 0).
	const int root = Scene::AddNode( new Node( mesh, Translate( ts_vec3( -4, 0, 4 ) ) ) );
	const int child = Scene::AddNode( new Node( mesh, Translate( ts_vec3( 4, 0, 4 ) ) ) );
	const int grandChild = Scene::AddNode( new Node( mesh, Translate( ts_vec3( 4, 0, 4 ) ) ) );
	const int survivor = Scene::AddNode( new Node( mesh, Translate( ts_vec3( 8, 0, 4 ) ) ) );
	Scene::AddChildNode( root, child );
	Scene::AddChildNode( child, grandChild );
	Scene::AddInstance( root );
	Scene::AddInstance( survivor );
	Scene::UpdateSceneGraph( 0.0f );

	// World-space centres, recorded while all nodes are alive.
	const ts_vec3 rootC = WorldOf( root, localCentre );
	const ts_vec3 childC = WorldOf( child, localCentre );
	const ts_vec3 grandC = WorldOf( grandChild, localCentre );
	const ts_vec3 survivorC = WorldOf( survivor, localCentre );
	TASK_CHECK( Near( rootC, ts_vec3( -4 + 1.0f / 3.0f, -1.0f / 3.0f, 4 ) ) );
	TASK_CHECK( Near( childC, ts_vec3( 1.0f / 3.0f, -1.0f / 3.0f, 8 ) ) );
	TASK_CHECK( Near( grandC, ts_vec3( 4 + 1.0f / 3.0f, -1.0f / 3.0f, 12 ) ) );
	TASK_CHECK( Scene::instPool.size() == 4 );
	TASK_CHECK( ShootHitsNode( root, rootC.x, rootC.y ) );
	TASK_CHECK( ShootHitsNode( child, childC.x, childC.y ) );
	TASK_CHECK( ShootHitsNode( grandChild, grandC.x, grandC.y ) );
	TASK_CHECK( ShootHitsNode( survivor, survivorC.x, survivorC.y ) );
	TASK_CHECK( Scene::NodeIdFromInstance( 0 ) == root );	// depth-first scene order

	// Remove the root: the whole subtree goes, including descendant slots.
	Scene::RemoveNode( root );
	TASK_CHECK( Scene::nodePool[root] == 0 );
	TASK_CHECK( Scene::nodePool[child] == 0 );
	TASK_CHECK( Scene::nodePool[grandChild] == 0 );
	TASK_CHECK( Scene::nodePool[survivor] != 0 );
	TASK_CHECK( Scene::rootNodes.size() == 1 );
	TASK_CHECK( Scene::rootNodes[0] == survivor );
	Scene::RemoveNode( root );								// repeated removal: no-op
	TASK_CHECK( Scene::rootNodes.size() == 1 );
	Scene::UpdateSceneGraph( 0.0f );

	// Only the surviving mesh instance is built; the pool is dense, and the
	// survivor's internal instance slot was updated to match.
	TASK_CHECK( Scene::instPool.size() == 1 );
	TASK_CHECK( Scene::NodeIdFromInstance( 0 ) == survivor );
	TASK_CHECK( Scene::NodeIdFromInstance( 1 ) == -1 );
	TASK_CHECK( Scene::nodePool[survivor]->GetInstanceID() == 0 );
	// Removed nodes cannot be hit any more; the survivor still can.
	TASK_CHECK( !ShootHitsNode( root, rootC.x, rootC.y ) );
	TASK_CHECK( !ShootHitsNode( child, childC.x, childC.y ) );
	TASK_CHECK( !ShootHitsNode( grandChild, grandC.x, grandC.y ) );
	TASK_CHECK( ShootHitsNode( survivor, survivorC.x, survivorC.y ) );
	// Node ids did not move: the removed slots are holes, not renumbered entries.
	TASK_CHECK( (int)Scene::nodePool.size() == survivor + 1 );
	return 0;
}

// Removing an internal node keeps its branch's sibling and its ancestors.
static int TopologyInternalSubtree()
{
	ClearRoots();
	const int mat = Scene::AddMaterial( ts_vec3( 0.5f ), "topology-internal" );
	const int mesh = MakeQuadMesh( mat );
	const ts_vec3 localCentre( 1.0f / 3.0f, -1.0f / 3.0f, 0 );

	// World x: root -4, branch -4, leafA 0, leafB 4, keep -8 (y = 0). Quads are 2
	// wide, so the x ranges [-5,-3], [-1,1], [3,5] and [-9,-7] are disjoint.
	const int root = Scene::AddNode( new Node( mesh, Translate( ts_vec3( -4, 0, 2 ) ) ) );
	const int branch = Scene::AddNode( new Node( mesh, Translate( ts_vec3( 0, 0, 2 ) ) ) );
	const int leafA = Scene::AddNode( new Node( mesh, Translate( ts_vec3( 4, 0, 2 ) ) ) );
	const int leafB = Scene::AddNode( new Node( mesh, Translate( ts_vec3( 8, 0, 2 ) ) ) );
	const int keep = Scene::AddNode( new Node( mesh, Translate( ts_vec3( -4, 0, 2 ) ) ) );
	Scene::AddChildNode( root, branch );
	Scene::AddChildNode( branch, leafA );
	Scene::AddChildNode( branch, leafB );
	Scene::AddChildNode( root, keep );
	Scene::AddInstance( root );
	Scene::UpdateSceneGraph( 0.0f );

	const ts_vec3 leafAC = WorldOf( leafA, localCentre );
	const ts_vec3 leafBC = WorldOf( leafB, localCentre );
	const ts_vec3 keepC = WorldOf( keep, localCentre );
	TASK_CHECK( Near( leafAC.x, 0 + 1.0f / 3.0f ) );
	TASK_CHECK( Near( leafBC.x, 4 + 1.0f / 3.0f ) );
	TASK_CHECK( Near( keepC.x, -8 + 1.0f / 3.0f ) );
	TASK_CHECK( ShootHitsNode( leafA, leafAC.x, leafAC.y ) );
	TASK_CHECK( ShootHitsNode( leafB, leafBC.x, leafBC.y ) );
	TASK_CHECK( ShootHitsNode( keep, keepC.x, keepC.y ) );

	// Remove the internal branch: it and both leaves disappear; the sibling stays.
	Scene::RemoveNode( branch );
	TASK_CHECK( Scene::nodePool[branch] == 0 );
	TASK_CHECK( Scene::nodePool[leafA] == 0 );
	TASK_CHECK( Scene::nodePool[leafB] == 0 );
	TASK_CHECK( Scene::nodePool[root] != 0 );
	TASK_CHECK( Scene::nodePool[keep] != 0 );
	// The surviving parent must no longer reference the removed branch.
	TASK_CHECK( Scene::nodePool[root]->childIdx.size() == 1 );
	TASK_CHECK( Scene::nodePool[root]->childIdx[0] == keep );
	Scene::UpdateSceneGraph( 0.0f );

	TASK_CHECK( !ShootHitsNode( branch, -4 + 1.0f / 3.0f, -1.0f / 3.0f ) );
	TASK_CHECK( !ShootHitsNode( leafA, leafAC.x, leafAC.y ) );
	TASK_CHECK( !ShootHitsNode( leafB, leafBC.x, leafBC.y ) );
	TASK_CHECK( ShootHitsNode( keep, keepC.x, keepC.y ) );
	// The instance table is dense and maps only to the two surviving mesh nodes.
	TASK_CHECK( Scene::instPool.size() == 2 );
	for ( unsigned i = 0; i < (unsigned)Scene::instPool.size(); i++ )
	{
		const int id = Scene::NodeIdFromInstance( i );
		TASK_CHECK( id == root || id == keep );
	}
	return 0;
}

// ------------------------------------------------------------------ lights
// The same emissive mesh under two parents gets two independent light sets.
static int LightsSharedMesh()
{
	ClearRoots();
	const ts_vec3 radiance( 4.0f, 2.0f, 1.0f );
	const int mat = Scene::AddMaterial( radiance, "lights-emissive" );
	const int mesh = MakeQuadMesh( mat );

	const int parentA = Scene::AddNode( new Node() );
	const int parentB = Scene::AddNode( new Node() );
	Scene::nodePool[parentA]->localTransform = Translate( ts_vec3( -3, 0, 5 ) );
	Scene::nodePool[parentB]->localTransform = Translate( ts_vec3( 3, 1, 5 ) );
	const int nodeA = Scene::AddNode( new Node( mesh, ts_mat4() ) );
	const int nodeB = Scene::AddNode( new Node( mesh, ts_mat4() ) );
	Scene::AddChildNode( parentA, nodeA );
	Scene::AddChildNode( parentB, nodeB );
	Scene::AddInstance( parentA );
	Scene::AddInstance( parentB );
	Scene::UpdateSceneGraph( 0.0f );

	// Per node and per source triangle there is exactly one light, with a unique
	// index; the association is not stored on the shared FatTri.
	const int a0 = Scene::GetTriLightIndex( nodeA, 0 );
	const int a1 = Scene::GetTriLightIndex( nodeA, 1 );
	const int b0 = Scene::GetTriLightIndex( nodeB, 0 );
	const int b1 = Scene::GetTriLightIndex( nodeB, 1 );
	TASK_CHECK( a0 >= 0 && a1 >= 0 && b0 >= 0 && b1 >= 0 );
	TASK_CHECK( a0 != a1 && b0 != b1 );
	TASK_CHECK( a0 != b0 && a0 != b1 && a1 != b0 && a1 != b1 );
	TASK_CHECK( Scene::GetTriLightIndex( parentA, 0 ) == -1 );	// no mesh -> no light
	TASK_CHECK( Scene::GetTriLightIndex( nodeA, 2 ) == -1 );	// no such source triangle
	TASK_CHECK( Scene::GetTriLightIndex( -1, 0 ) == -1 );		// dead id
	// TriLight::instIdx is the stable node id; triIdx is the source triangle id.
	TASK_CHECK( Scene::triLights[a0]->instIdx == nodeA );
	TASK_CHECK( Scene::triLights[b1]->instIdx == nodeB );
	TASK_CHECK( Scene::triLights[a0]->triIdx == 0 );
	TASK_CHECK( Scene::triLights[b1]->triIdx == 1 );

	// Each light sits at its own node's world-space position.
	TASK_CHECK( Near( Scene::triLights[a0]->vertex0, ts_vec3( -4, -1, 5 ) ) );
	TASK_CHECK( Near( Scene::triLights[b0]->vertex0, ts_vec3( 2, 0, 5 ) ) );
	TASK_CHECK( Near( Scene::triLights[a0]->centre, ts_vec3( -3, 0, 5 ) + ts_vec3( 1.0f / 3.0f, -1.0f / 3.0f, 0 ) ) );
	TASK_CHECK( Near( Scene::triLights[b0]->centre, ts_vec3( 3, 1, 5 ) + ts_vec3( 1.0f / 3.0f, -1.0f / 3.0f, 0 ) ) );

	// World vertices, unit geometric normal, area and energy.
	const int allLights[4] = { a0, a1, b0, b1 };
	for ( int i = 0; i < 4; i++ )
	{
		const TriLight* light = Scene::triLights[allLights[i]];
		TASK_CHECK( Near( Length( light->N ), 1.0f ) );
		TASK_CHECK( Near( light->area, 2.0f ) );			// quad of 2 x 2 = two are-2 triangles
		TASK_CHECK( Near( light->energy, ( radiance.x + radiance.y + radiance.z ) * 2.0f, 1e-2f ) );
		TASK_CHECK( Near( light->radiance, radiance, 1e-5f ) );
	}

	// Move only parent A: A's light follows; B's lights are untouched.
	Scene::nodePool[parentA]->localTransform = Translate( ts_vec3( -3, 2, 7 ) );
	Scene::UpdateSceneGraph( 0.0f );
	TASK_CHECK( Near( Scene::triLights[a0]->vertex0, ts_vec3( -4, 1, 7 ) ) );
	TASK_CHECK( Near( Scene::triLights[b0]->vertex0, ts_vec3( 2, 0, 5 ) ) );
	// Triangle 1 is (-1,-1,0),(1,1,0),(-1,1,0): centre (-1/3, 1/3, 0).
	TASK_CHECK( Near( Scene::triLights[b1]->centre, ts_vec3( 3, 1, 5 ) + ts_vec3( -1.0f / 3.0f, 1.0f / 3.0f, 0 ) ) );

	// Delete parent B: the light table compacts and A's association must survive.
	const float areaABefore = Scene::triLights[a0]->area;
	Scene::RemoveNode( parentB );
	TASK_CHECK( Scene::GetTriLightIndex( nodeB, 0 ) == -1 );
	TASK_CHECK( Scene::GetTriLightIndex( nodeB, 1 ) == -1 );
	TASK_CHECK( Scene::GetTriLightIndex( nodeA, 0 ) >= 0 );
	const int a0After = Scene::GetTriLightIndex( nodeA, 0 );
	const int a1After = Scene::GetTriLightIndex( nodeA, 1 );
	TASK_CHECK( a0After >= 0 && a1After >= 0 && a0After != a1After );
	TASK_CHECK( Scene::triLights[a0After]->instIdx == nodeA );
	TASK_CHECK( Scene::triLights[a0After]->triIdx == 0 );
	TASK_CHECK( Scene::triLights[a1After]->triIdx == 1 );
	TASK_CHECK( Near( Scene::triLights[a0After]->area, areaABefore ) );
	TASK_CHECK( Near( Scene::triLights[a0After]->vertex0, ts_vec3( -4, 1, 7 ) ) );
	Scene::UpdateSceneGraph( 0.0f );
	TASK_CHECK( Scene::GetTriLightIndex( nodeA, 0 ) == a0After );
	TASK_CHECK( ShootHitsNode( nodeA, -3 + 1.0f / 3.0f, 2 - 1.0f / 3.0f ) );
	TASK_CHECK( Shoot( 3, 1 ) == -1 );						// B is no longer traced
	return 0;
}

// An animated ancestor moves the light; removing it clears the channel.
static int LightsAnimatedAncestor()
{
	ClearRoots();
	const ts_vec3 radiance( 10.0f, 10.0f, 10.0f );
	const int mat = Scene::AddMaterial( radiance, "lights-animated" );
	const int mesh = MakeQuadMesh( mat );

	const int parent = Scene::AddNode( new Node() );
	const int child = Scene::AddNode( new Node( mesh, ts_mat4() ) );
	Scene::AddChildNode( parent, child );
	Scene::AddInstance( parent );

	// One translation channel on the parent: (0,0,0) -> (2,0,0).
	Animation* anim = new Animation();
	Animation::Sampler* sampler = new Animation::Sampler();
	sampler->t = { 0.0f, 1.0f };
	sampler->vec3Key = { ts_vec3( 0 ), ts_vec3( 2, 0, 0 ) };
	anim->sampler.push_back( sampler );
	Animation::Channel* channel = new Animation::Channel();
	channel->nodeIdx = parent;
	channel->samplerIdx = 0;
	channel->target = 0;
	anim->channel.push_back( channel );
	Scene::animations.push_back( anim );
	const int animId = (int)Scene::animations.size() - 1;

	Scene::UpdateSceneGraph( 0.0f );
	const int lightIdx = Scene::GetTriLightIndex( child, 0 );
	TASK_CHECK( lightIdx >= 0 );
	TASK_CHECK( Near( Scene::triLights[lightIdx]->vertex0, ts_vec3( -1, -1, 0 ) ) );

	// Halfway through the animation the parent is at x = 1; the light moved too.
	Scene::UpdateAnimation( animId, 0.5f );
	Scene::UpdateSceneGraph( 0.0f );		// deltaTime 0: the animation timer stays where set
	TASK_CHECK( Near( Scene::triLights[lightIdx]->vertex0, ts_vec3( 0, -1, 0 ), 1e-3f ) );
	TASK_CHECK( Near( Scene::triLights[lightIdx]->centre,
		ts_vec3( 1, 0, 0 ) + ts_vec3( 1.0f / 3.0f, -1.0f / 3.0f, 0 ), 1e-3f ) );
	TASK_CHECK( Scene::triLights[lightIdx]->instIdx == child );
	TASK_CHECK( ShootHitsNode( child, 1 + 1.0f / 3.0f, -1.0f / 3.0f ) );

	// Remove the animated parent: its channel and its light go. Advancing the
	// animation afterwards must not touch the removed node.
	Scene::RemoveNode( parent );
	TASK_CHECK( Scene::GetTriLightIndex( child, 0 ) == -1 );
	for ( Animation* a : Scene::animations )
		for ( Animation::Channel* c : a->channel )
			TASK_CHECK( c->nodeIdx != parent );
	TASK_CHECK( Scene::animations[animId]->channel.empty() );
	TASK_CHECK( Scene::animations[animId]->sampler.size() == 1 );	// sampler preserved
	Scene::UpdateAnimation( animId, 0.25f );
	Scene::UpdateSceneGraph( 0.0f );								// must not crash
	return 0;
}

// Non-uniform scaling: area, energy, centre and the unit geometric normal.
static int LightsNonUniformScale()
{
	ClearRoots();
	const ts_vec3 radiance( 3.0f, 3.0f, 3.0f );
	const int mat = Scene::AddMaterial( radiance, "lights-scale" );
	const int mesh = MakeQuadMesh( mat );

	const int parent = Scene::AddNode( new Node() );
	Scene::nodePool[parent]->localTransform = Scale( ts_vec3( 2, 4, 1 ) );
	const int child = Scene::AddNode( new Node( mesh, ts_mat4() ) );
	Scene::AddChildNode( parent, child );
	Scene::AddInstance( parent );
	Scene::UpdateSceneGraph( 0.0f );

	const int lightIdx = Scene::GetTriLightIndex( child, 0 );
	TASK_CHECK( lightIdx >= 0 );
	// Local triangle area is 2; a planar x2 / y4 scale multiplies it by 8.
	TASK_CHECK( Near( Scene::triLights[lightIdx]->area, 16.0f, 1e-3f ) );
	TASK_CHECK( Near( Length( Scene::triLights[lightIdx]->N ), 1.0f ) );
	TASK_CHECK( Near( Scene::triLights[lightIdx]->N, ts_vec3( 0, 0, 1 ) ) );
	TASK_CHECK( Near( Scene::triLights[lightIdx]->energy, 9.0f * 16.0f, 1e-1f ) );
	TASK_CHECK( Near( Scene::triLights[lightIdx]->vertex0, ts_vec3( -2, -4, 0 ) ) );
	TASK_CHECK( Near( Scene::triLights[lightIdx]->centre, ts_vec3( 2.0f / 3.0f, -4.0f / 3.0f, 0 ) ) );
	return 0;
}

// ------------------------------------------------------------- empty_reuse
// Removing the last instance keeps Scene::tlas valid; it turns empty in place
// and can be populated again afterwards through the same pointer.
static int EmptyThenReuse()
{
	// Start from a known-empty scene regardless of earlier cases.
	ClearRoots();
	TASK_CHECK( Scene::rootNodes.empty() );
	TASK_CHECK( Scene::instPool.empty() );

	// A pointer saved before the scene emptied stays valid and usable.
	tinybvh::BVH* saved = Scene::tlas;
	TASK_CHECK( saved != 0 );
	tinybvh::Ray probe( tinybvh::bvhvec3( 0, 0, 1 ), tinybvh::bvhvec3( 0, 0, 1 ) );
	probe.hit.t = 7.0f;								// an empty intersect must leave this intact
	saved->Intersect( probe );
	TASK_CHECK( probe.hit.t == 7.0f );
	tinybvh::Ray shadow( tinybvh::bvhvec3( 0, 0, 1 ), tinybvh::bvhvec3( 0, 0, 1 ) );
	TASK_CHECK( !saved->IsOccluded( shadow ) );

	// A zero-instance build is accepted, including null instance input.
	Scene::tlas->Build( (tinybvh::BLASInstance*)0, 0, (tinybvh::BVHBase**)0, 0 );
	tinybvh::Ray emptyProbe( tinybvh::bvhvec3( 0, 0, 1 ), tinybvh::bvhvec3( 0, 0, 1 ) );
	emptyProbe.hit.t = 5.0f;
	Scene::tlas->Intersect( emptyProbe );
	TASK_CHECK( emptyProbe.hit.t == 5.0f );
	TASK_CHECK( !Scene::tlas->IsOccluded( shadow ) );

	// Add a node that reuses an existing mesh built by an earlier case.
	const int mat = Scene::AddMaterial( ts_vec3( 0.4f ), "empty-reuse" );
	const int mesh = MakeQuadMesh( mat );
	const int node = Scene::AddNode( new Node( mesh, Translate( ts_vec3( 0, 0, 3 ) ) ) );
	Scene::AddInstance( node );
	Scene::UpdateSceneGraph( 0.0f );

	// The same TLAS object now yields a real hit that maps back to the node.
	TASK_CHECK( Scene::tlas == saved );
	TASK_CHECK( Scene::instPool.size() == 1 );
	TASK_CHECK( Scene::NodeIdFromInstance( 0 ) == node );
	tinybvh::Ray ray( tinybvh::bvhvec3( 0, 0, 0 ), tinybvh::bvhvec3( 0, 0, 1 ) );
	Scene::tlas->Intersect( ray );
	TASK_CHECK( ray.hit.t < 1e30f );
	TASK_CHECK( Scene::NodeIdFromInstance( (unsigned)ray.hit.inst ) == node );
	TASK_CHECK( ShootHitsNode( node, 1.0f / 3.0f, -1.0f / 3.0f ) );
	TASK_CHECK( Scene::tlas->IsOccluded( tinybvh::Ray( tinybvh::bvhvec3( 0, 0, 0 ), tinybvh::bvhvec3( 0, 0, 1 ) ) ) );

	// Remove it again: valid empty TLAS on the same object, no hit, no occlusion.
	Scene::RemoveNode( node );
	Scene::UpdateSceneGraph( 0.0f );
	TASK_CHECK( Scene::tlas == saved );
	tinybvh::Ray gone( tinybvh::bvhvec3( 0, 0, 0 ), tinybvh::bvhvec3( 0, 0, 1 ) );
	gone.hit.t = 9.0f;
	Scene::tlas->Intersect( gone );
	TASK_CHECK( gone.hit.t == 9.0f );
	TASK_CHECK( !Scene::tlas->IsOccluded( shadow ) );
	TASK_CHECK( Scene::instPool.empty() );
	// A subsequent reuse must work once more: the empty state is not a dead end.
	const int again = Scene::AddNode( new Node( mesh, Translate( ts_vec3( 1, 0, 3 ) ) ) );
	Scene::AddInstance( again );
	Scene::UpdateSceneGraph( 0.0f );
	TASK_CHECK( Scene::tlas == saved );
	TASK_CHECK( Scene::instPool.size() == 1 );
	TASK_CHECK( Scene::NodeIdFromInstance( 0 ) == again );
	TASK_CHECK( ShootHitsNode( again, 1 + 1.0f / 3.0f, -1.0f / 3.0f ) );
	return 0;
}

} // namespace remove_test

const struct task_case task_cases[] =
{
	{ "topology", "root_subtree", remove_test::TopologyRootSubtree },
	{ "topology", "internal_subtree", remove_test::TopologyInternalSubtree },
	{ "lights", "shared_mesh", remove_test::LightsSharedMesh },
	{ "lights", "animated_ancestor", remove_test::LightsAnimatedAncestor },
	{ "lights", "nonuniform_scale", remove_test::LightsNonUniformScale },
	{ "empty_reuse", "empty_then_reuse", remove_test::EmptyThenReuse },
};

const size_t task_case_count = sizeof( task_cases ) / sizeof( task_cases[0] );
