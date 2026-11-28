// Tests for rigid scene subtree deletion in TinyScene.
//
// Covers: Scene::RemoveNode (root and internal subtrees, empty nodePool slots,
// double removal as a no-op, animation channel release), the dense instance
// list rebuild in Scene::UpdateSceneGraph with Scene::NodeIdFromInstance,
// per-(node, source triangle) emissive light tracking with
// Scene::GetTriLightIndex, and the empty-TLAS state of Scene::tlas.
//
// This file defines no main(); the cases are registered through task_cases
// and driven by task_support/driver.c via task_support/run.sh.

#include "task_support/scene_api.hpp"
#include "task_support/check.h"

using namespace tinyscene;

namespace
{

// --- small helpers ---------------------------------------------------------

// Scene is a static class; reset the relevant pools between cases. The TLAS
// object is kept on purpose: UpdateSceneGraph reuses (and empties) it.
void ResetScene()
{
	for (Node* node : Scene::nodePool) delete node;
	Scene::nodePool.clear();
	Scene::rootNodes.clear();
	for (Mesh* mesh : Scene::meshPool) delete mesh;
	Scene::meshPool.clear();
	for (Material* material : Scene::materials) delete material;
	Scene::materials.clear();
	for (TriLight* light : Scene::triLights) delete light;
	Scene::triLights.clear();
	for (Animation* anim : Scene::animations)
	{
		for (auto* ch : anim->channel) delete ch;
		for (auto* sa : anim->sampler) delete sa;
		delete anim;
	}
	Scene::animations.clear();
	Scene::instPool.clear();
	Scene::instNodeIds.clear();
}

bool Near( float a, float b, float eps = 1e-3f ) { return fabsf( a - b ) < eps; }
bool Near3( const ts_vec3& a, const ts_vec3& b, float eps = 1e-3f )
{
	return Near( a.x, b.x, eps ) && Near( a.y, b.y, eps ) && Near( a.z, b.z, eps );
}

ts_mat4 Translation( float x, float y, float z )
{
	ts_mat4 m;
	m[3] = x, m[7] = y, m[11] = z;
	return m;
}

// a single-triangle procedural mesh
int MakeTriMesh( int matId, const ts_vec3& v0, const ts_vec3& v1, const ts_vec3& v2 )
{
	const int meshId = Scene::AddMesh( 0 );
	Scene::AddTriToMesh( meshId, v0, v1, v2, matId );
	Scene::meshPool[meshId]->BuildMaterialList();
	return meshId;
}

struct Hit { bool hit; float t; int node; };

// trace a ray in the +z direction through the scene TLAS
Hit Trace( float ox, float oy, float oz )
{
	tinybvh::Ray ray( tinybvh::bvhvec3( ox, oy, oz ), tinybvh::bvhvec3( 0, 0, 1 ) );
	Scene::tlas->Intersect( ray );
	Hit h;
	h.t = ray.hit.t;
	h.hit = ray.hit.t < 1e29f;
	h.node = h.hit ? Scene::NodeIdFromInstance( ray.hit.inst ) : -1;
	return h;
}

// a one-channel LINEAR translation animation, keys at t=0 and t=1
Animation* MakeTranslationAnim( int nodeId, const ts_vec3& from, const ts_vec3& to )
{
	tinygltf::Model model;
	tinygltf::Buffer buffer;
	buffer.data.resize( 8 + 24 );
	float* f = (float*)buffer.data.data();
	f[0] = 0.0f, f[1] = 1.0f; // key frame times
	f[2] = from.x, f[3] = from.y, f[4] = from.z;
	f[5] = to.x, f[6] = to.y, f[7] = to.z;
	model.buffers.push_back( buffer );
	tinygltf::BufferView timeView;
	timeView.buffer = 0, timeView.byteOffset = 0, timeView.byteLength = 8;
	tinygltf::BufferView keyView;
	keyView.buffer = 0, keyView.byteOffset = 8, keyView.byteLength = 24;
	model.bufferViews.push_back( timeView );
	model.bufferViews.push_back( keyView );
	tinygltf::Accessor timeAcc;
	timeAcc.bufferView = 0, timeAcc.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
	timeAcc.count = 2, timeAcc.type = TINYGLTF_TYPE_SCALAR;
	tinygltf::Accessor keyAcc;
	keyAcc.bufferView = 1, keyAcc.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
	keyAcc.count = 2, keyAcc.type = TINYGLTF_TYPE_VEC3;
	model.accessors.push_back( timeAcc );
	model.accessors.push_back( keyAcc );
	tinygltf::Animation gltfAnim;
	tinygltf::AnimationSampler sampler;
	sampler.input = 0, sampler.output = 1, sampler.interpolation = "LINEAR";
	gltfAnim.samplers.push_back( sampler );
	tinygltf::AnimationChannel channel;
	channel.sampler = 0, channel.target_node = nodeId, channel.target_path = "translation";
	gltfAnim.channels.push_back( channel );
	return new Animation( gltfAnim, model, 0 );
}

// --- topology --------------------------------------------------------------

int CaseRootSubtreeDelete()
{
	ResetScene();
	const int mat = Scene::AddMaterial( ts_vec3( 0.4f ), "normal" );
	const int mesh = MakeTriMesh( mat, ts_vec3( -1, -1, 4 ), ts_vec3( 1, -1, 4 ), ts_vec3( 0, 1, 4 ) );
	ts_mat4 identity;
	const int left = Scene::AddNode( new Node( mesh, identity ) );
	const int right = Scene::AddNode( new Node( mesh, Translation( 4, 0, 0 ) ) );
	Scene::AddInstance( left );
	Scene::AddInstance( right );
	Scene::UpdateSceneGraph( 0 );
	TASK_CHECK( Trace( 0, 0, 0 ).node == left );
	TASK_CHECK( Trace( 4, 0, 0 ).node == right );
	// remove the left root; nodePool numbering must be preserved
	const size_t poolSize = Scene::nodePool.size();
	Scene::RemoveNode( left );
	TASK_CHECK( Scene::nodePool.size() == poolSize );
	TASK_CHECK( Scene::nodePool[left] == 0 ); // empty slot, no renumbering
	TASK_CHECK( Scene::rootNodes.size() == 1 );
	// removing the same node again is a no-op
	Scene::RemoveNode( left );
	TASK_CHECK( Scene::rootNodes.size() == 1 );
	// shared scene data survives the removal of one referrer
	TASK_CHECK( Scene::meshPool[mesh] != 0 );
	TASK_CHECK( Scene::materials[mat] != 0 );
	Scene::UpdateSceneGraph( 0 );
	TASK_CHECK( Scene::instPool.size() == 1 ); // dense, live instances only
	TASK_CHECK( !Trace( 0, 0, 0 ).hit ); // the old position is no longer hit
	Hit live = Trace( 4, 0, 0 );
	TASK_CHECK( live.hit && live.node == right );
	TASK_CHECK( Near( live.t, 4 ) );
	return 0;
}

int CaseInternalSubtreeDelete()
{
	ResetScene();
	const int mat = Scene::AddMaterial( ts_vec3( 0.4f ), "normal" );
	const int meshA = MakeTriMesh( mat, ts_vec3( -1, -1, 4 ), ts_vec3( 1, -1, 4 ), ts_vec3( 0, 1, 4 ) );
	const int meshB = MakeTriMesh( mat, ts_vec3( 9, -1, 4 ), ts_vec3( 11, -1, 4 ), ts_vec3( 10, 1, 4 ) );
	ts_mat4 identity;
	const int root = Scene::AddNode( new Node( -1, identity ) );
	const int childA = Scene::AddNode( new Node( -1, identity ) );
	const int grandA = Scene::AddNode( new Node( meshA, identity ) );
	const int childB = Scene::AddNode( new Node( meshB, identity ) );
	Scene::AddChildNode( root, childA );
	Scene::AddChildNode( root, childB );
	Scene::AddChildNode( childA, grandA );
	Scene::AddInstance( root );
	// one animation channel into the doomed subtree, one into the survivor
	Scene::animations.push_back( MakeTranslationAnim( grandA, ts_vec3( 0 ), ts_vec3( 50, 0, 0 ) ) );
	Scene::animations.push_back( MakeTranslationAnim( childB, ts_vec3( 0 ), ts_vec3( 0, 0, -2 ) ) );
	Scene::UpdateSceneGraph( 0 );
	TASK_CHECK( Scene::instPool.size() == 2 );
	TASK_CHECK( Trace( 0, 0, 0 ).hit );
	TASK_CHECK( Trace( 10, 0, 0 ).hit );
	TASK_CHECK( Scene::animations[0]->channel.size() == 1 );
	TASK_CHECK( Scene::animations[1]->channel.size() == 1 );
	// remove an internal node: the whole subtree (childA + grandA) goes with it
	Scene::RemoveNode( childA );
	TASK_CHECK( Scene::nodePool[childA] == 0 && Scene::nodePool[grandA] == 0 );
	TASK_CHECK( Scene::nodePool.size() == 4 ); // slots are preserved
	TASK_CHECK( Scene::nodePool[root]->childIdx.size() == 1 );
	TASK_CHECK( Scene::nodePool[root]->childIdx[0] == childB );
	TASK_CHECK( Scene::animations[0]->channel.size() == 0 ); // channel to grandA released
	TASK_CHECK( Scene::animations[1]->channel.size() == 1 ); // survivor channel intact
	// advancing the animations must not touch the deleted nodes
	Scene::UpdateSceneGraph( 0.5f );
	TASK_CHECK( Scene::instPool.size() == 1 );
	TASK_CHECK( !Trace( 0, 0, 0 ).hit );
	Hit moved = Trace( 10, 0, 0 );
	TASK_CHECK( moved.hit && moved.node == childB );
	TASK_CHECK( Near( moved.t, 3 ) ); // survivor translated by (0,0,-1)
	return 0;
}

int CaseDenseInstancesAndMapping()
{
	ResetScene();
	const int mat = Scene::AddMaterial( ts_vec3( 0.4f ), "normal" );
	const int mesh = MakeTriMesh( mat, ts_vec3( -1, -1, 4 ), ts_vec3( 1, -1, 4 ), ts_vec3( 0, 1, 4 ) );
	const int n0 = Scene::AddNode( new Node( mesh, Translation( 0, 0, 0 ) ) );
	const int n1 = Scene::AddNode( new Node( mesh, Translation( 4, 0, 0 ) ) );
	const int n2 = Scene::AddNode( new Node( mesh, Translation( 8, 0, 0 ) ) );
	Scene::AddInstance( n0 );
	Scene::AddInstance( n1 );
	Scene::AddInstance( n2 );
	Scene::UpdateSceneGraph( 0 );
	TASK_CHECK( Scene::instPool.size() == 3 );
	Scene::RemoveNode( n1 ); // the middle instance
	Scene::UpdateSceneGraph( 0 );
	TASK_CHECK( Scene::instPool.size() == 2 ); // dense again, no dead slots
	TASK_CHECK( !Trace( 4, 0, 0 ).hit );
	Hit a = Trace( 0, 0, 0 ), b = Trace( 8, 0, 0 );
	TASK_CHECK( a.hit && a.node == n0 );
	TASK_CHECK( b.hit && b.node == n2 );
	// instance indices are dense and map back to the producing nodes
	TASK_CHECK( Scene::NodeIdFromInstance( 0 ) == n0 );
	TASK_CHECK( Scene::NodeIdFromInstance( 1 ) == n2 );
	TASK_CHECK( Scene::NodeIdFromInstance( 2 ) == -1 );
	return 0;
}

int CaseRigidMeshDelete()
{
	ResetScene();
	Scene::SetBVHDefault( BVH_RIGID ); // rigid meshes: BVH4_CPU BLAS
	const int mat = Scene::AddMaterial( ts_vec3( 0.4f ), "normal" );
	const int mesh = MakeTriMesh( mat, ts_vec3( -1, -1, 4 ), ts_vec3( 1, -1, 4 ), ts_vec3( 0, 1, 4 ) );
	Scene::SetBVHDefault( BVH_DYNAMIC );
	ts_mat4 identity;
	const int left = Scene::AddNode( new Node( mesh, identity ) );
	const int right = Scene::AddNode( new Node( mesh, Translation( 4, 0, 0 ) ) );
	Scene::AddInstance( left );
	Scene::AddInstance( right );
	Scene::UpdateSceneGraph( 0 );
	TASK_CHECK( Trace( 0, 0, 0 ).node == left );
	TASK_CHECK( Trace( 4, 0, 0 ).node == right );
	Scene::RemoveNode( left );
	Scene::UpdateSceneGraph( 0 );
	TASK_CHECK( Scene::instPool.size() == 1 );
	TASK_CHECK( !Trace( 0, 0, 0 ).hit );
	Hit live = Trace( 4, 0, 0 );
	TASK_CHECK( live.hit && live.node == right );
	return 0;
}

// --- lights ----------------------------------------------------------------

int CaseSharedEmissiveMeshLights()
{
	ResetScene();
	const int emit = Scene::AddMaterial( ts_vec3( 4, 2, 1 ), "emit" );
	const int mesh = MakeTriMesh( emit, ts_vec3( 0, 0, 0 ), ts_vec3( 1, 0, 0 ), ts_vec3( 0, 1, 0 ) );
	// node A: pure translation; node B: translation + shear (z' = 0.5x + z)
	const int nodeA = Scene::AddNode( new Node( mesh, Translation( 0, 0, 5 ) ) );
	ts_mat4 shear;
	shear[3] = 10, shear[8] = 0.5f, shear[11] = 5;
	const int nodeB = Scene::AddNode( new Node( mesh, shear ) );
	Scene::AddInstance( nodeA );
	Scene::AddInstance( nodeB );
	Scene::UpdateSceneGraph( 0 );
	// each node has its own world-space light for source triangle 0
	const int la = Scene::GetTriLightIndex( nodeA, 0 );
	const int lb = Scene::GetTriLightIndex( nodeB, 0 );
	TASK_CHECK( la > -1 && lb > -1 && la != lb );
	TASK_CHECK( (int)Scene::triLights.size() == 2 );
	const TriLight* a = Scene::triLights[la];
	const TriLight* b = Scene::triLights[lb];
	TASK_CHECK( a->instIdx == nodeA && a->triIdx == 0 );
	TASK_CHECK( b->instIdx == nodeB && b->triIdx == 0 );
	// node A light: a translated copy of the source triangle
	TASK_CHECK( Near3( a->vertex0, ts_vec3( 0, 0, 5 ) ) );
	TASK_CHECK( Near3( a->vertex1, ts_vec3( 1, 0, 5 ) ) );
	TASK_CHECK( Near3( a->vertex2, ts_vec3( 0, 1, 5 ) ) );
	TASK_CHECK( Near3( a->centre, ts_vec3( 1.0f / 3, 1.0f / 3, 5 ) ) );
	TASK_CHECK( Near3( a->N, ts_vec3( 0, 0, 1 ) ) );
	TASK_CHECK( Near( a->area, 0.5f ) );
	TASK_CHECK( Near3( a->radiance, ts_vec3( 4, 2, 1 ) ) );
	TASK_CHECK( Near( a->energy, 3.5f ) );
	// node B light: sheared; the normal follows the inverse-transpose
	TASK_CHECK( Near3( b->vertex0, ts_vec3( 10, 0, 5 ) ) );
	TASK_CHECK( Near3( b->vertex1, ts_vec3( 11, 0, 5.5f ) ) );
	TASK_CHECK( Near3( b->vertex2, ts_vec3( 10, 1, 5 ) ) );
	TASK_CHECK( Near3( b->centre, ts_vec3( 31.0f / 3, 1.0f / 3, 15.5f / 3 ) ) );
	TASK_CHECK( Near3( b->N, ts_vec3( -0.4472136f, 0, 0.8944272f ) ) );
	TASK_CHECK( Near( b->area, 0.5590169f ) );
	TASK_CHECK( Near( b->energy, 3.9131184f ) );
	// the shared FatTri is not abused as a per-instance light index
	TASK_CHECK( Scene::meshPool[mesh]->triangles[0].ltriIdx == -1 );
	// moving one node rewrites only its own light
	Scene::SetNodeTransform( nodeA, Translation( 0, 0, 8 ) );
	Scene::UpdateSceneGraph( 0 );
	TASK_CHECK( Scene::GetTriLightIndex( nodeB, 0 ) == lb );
	TASK_CHECK( Near3( Scene::triLights[la]->vertex0, ts_vec3( 0, 0, 8 ) ) );
	TASK_CHECK( Near3( Scene::triLights[lb]->vertex0, ts_vec3( 10, 0, 5 ) ) );
	// deleting one node removes only its lights; the other stays correct
	Scene::RemoveNode( nodeA );
	Scene::UpdateSceneGraph( 0 );
	TASK_CHECK( Scene::GetTriLightIndex( nodeA, 0 ) == -1 );
	TASK_CHECK( (int)Scene::triLights.size() == 1 );
	const int lb2 = Scene::GetTriLightIndex( nodeB, 0 );
	TASK_CHECK( lb2 == 0 ); // light table was compacted
	const TriLight* b2 = Scene::triLights[lb2];
	TASK_CHECK( b2->instIdx == nodeB && b2->triIdx == 0 );
	TASK_CHECK( Near3( b2->vertex1, ts_vec3( 11, 0, 5.5f ) ) );
	TASK_CHECK( Near3( b2->N, ts_vec3( -0.4472136f, 0, 0.8944272f ) ) );
	TASK_CHECK( Near( b2->area, 0.5590169f ) );
	// the geometry side agrees: nodeA is gone, nodeB still intersects
	TASK_CHECK( !Trace( 0, 0, 0 ).hit );
	TASK_CHECK( Trace( 10.2f, 0.1f, 0 ).node == nodeB );
	return 0;
}

int CaseLightAssociationHierarchy()
{
	ResetScene();
	const int emit = Scene::AddMaterial( ts_vec3( 2, 2, 2 ), "emit" );
	const int mesh = MakeTriMesh( emit, ts_vec3( 0, 0, 0 ), ts_vec3( 1, 0, 0 ), ts_vec3( 0, 1, 0 ) );
	ts_mat4 identity;
	// three emissive nodes; the middle one will be deleted
	const int nodeA = Scene::AddNode( new Node( mesh, Translation( 0, 0, 5 ) ) );
	const int nodeB = Scene::AddNode( new Node( mesh, Translation( 10, 0, 5 ) ) );
	// nodeC sits below a parent, to verify ancestor motion and scaling
	const int parent = Scene::AddNode( new Node( -1, identity ) );
	const int nodeC = Scene::AddNode( new Node( mesh, Translation( 20, 0, 5 ) ) );
	Scene::AddChildNode( parent, nodeC );
	Scene::AddInstance( nodeA );
	Scene::AddInstance( nodeB );
	Scene::AddInstance( parent );
	Scene::UpdateSceneGraph( 0 );
	TASK_CHECK( (int)Scene::triLights.size() == 3 );
	TASK_CHECK( Scene::GetTriLightIndex( nodeA, 0 ) > -1 );
	TASK_CHECK( Scene::GetTriLightIndex( nodeB, 0 ) > -1 );
	TASK_CHECK( Scene::GetTriLightIndex( nodeC, 0 ) > -1 );
	// delete the middle node: the surviving associations stay correct
	Scene::RemoveNode( nodeB );
	Scene::UpdateSceneGraph( 0 );
	TASK_CHECK( (int)Scene::triLights.size() == 2 );
	TASK_CHECK( Scene::GetTriLightIndex( nodeB, 0 ) == -1 );
	const int la = Scene::GetTriLightIndex( nodeA, 0 );
	const int lc = Scene::GetTriLightIndex( nodeC, 0 );
	TASK_CHECK( la > -1 && lc > -1 );
	TASK_CHECK( Near3( Scene::triLights[la]->vertex0, ts_vec3( 0, 0, 5 ) ) );
	TASK_CHECK( Near3( Scene::triLights[lc]->vertex0, ts_vec3( 20, 0, 5 ) ) );
	// no light refers to a deleted node
	bool orphans = false;
	for (TriLight* light : Scene::triLights)
		if (light->instIdx != nodeA && light->instIdx != nodeC) orphans = true;
	TASK_CHECK( !orphans );
	// ancestor motion propagates to the light, and only to that light
	Scene::SetNodeTransform( parent, Translation( 0, 0, 4 ) );
	Scene::UpdateSceneGraph( 0 );
	TASK_CHECK( Near3( Scene::triLights[lc]->vertex0, ts_vec3( 20, 0, 9 ) ) );
	TASK_CHECK( Near3( Scene::triLights[la]->vertex0, ts_vec3( 0, 0, 5 ) ) );
	// non-uniform scaling of the ancestor: area and energy scale along
	ts_mat4 scale;
	scale[0] = 2, scale[5] = 3;
	Scene::SetNodeTransform( parent, scale );
	Scene::UpdateSceneGraph( 0 );
	const TriLight* c = Scene::triLights[lc];
	TASK_CHECK( Near3( c->vertex0, ts_vec3( 40, 0, 5 ) ) );
	TASK_CHECK( Near3( c->vertex1, ts_vec3( 42, 0, 5 ) ) );
	TASK_CHECK( Near3( c->vertex2, ts_vec3( 40, 3, 5 ) ) );
	TASK_CHECK( Near3( c->N, ts_vec3( 0, 0, 1 ) ) );
	TASK_CHECK( Near( c->area, 3.0f ) );
	TASK_CHECK( Near( c->energy, 18.0f ) );
	return 0;
}

// --- empty_reuse -----------------------------------------------------------

int CaseEmptyTlasReuse()
{
	ResetScene();
	const int mat = Scene::AddMaterial( ts_vec3( 0.4f ), "normal" );
	const int mesh = MakeTriMesh( mat, ts_vec3( -1, -1, 4 ), ts_vec3( 1, -1, 4 ), ts_vec3( 0, 1, 4 ) );
	ts_mat4 identity;
	const int node = Scene::AddNode( new Node( mesh, identity ) );
	Scene::AddInstance( node );
	Scene::UpdateSceneGraph( 0 );
	tinybvh::BVH* savedTlas = Scene::tlas;
	TASK_CHECK( savedTlas != 0 );
	TASK_CHECK( Trace( 0, 0, 0 ).hit );
	// remove the last instance
	Scene::RemoveNode( node );
	Scene::UpdateSceneGraph( 0 );
	TASK_CHECK( Scene::tlas == savedTlas ); // same object, now empty
	TASK_CHECK( Scene::instPool.size() == 0 );
	// Intersect keeps the incoming hit record; IsOccluded is false
	tinybvh::Ray ray( tinybvh::bvhvec3( 0, 0, 0 ), tinybvh::bvhvec3( 0, 0, 1 ) );
	ray.hit.t = 123.0f;
	Scene::tlas->Intersect( ray );
	TASK_CHECK( ray.hit.t == 123.0f );
	TASK_CHECK( !Scene::tlas->IsOccluded( ray ) );
	// adding a new node with the shared mesh revives the same object
	const int node2 = Scene::AddNode( new Node( mesh, identity ) );
	Scene::AddInstance( node2 );
	Scene::UpdateSceneGraph( 0 );
	TASK_CHECK( Scene::tlas == savedTlas );
	Hit h = Trace( 0, 0, 0 );
	TASK_CHECK( h.hit && h.node == node2 );
	TASK_CHECK( Near( h.t, 4 ) );
	return 0;
}

int CaseDeleteAllAndRebuild()
{
	ResetScene();
	const int mat = Scene::AddMaterial( ts_vec3( 0.4f ), "normal" );
	const int mesh = MakeTriMesh( mat, ts_vec3( -1, -1, 4 ), ts_vec3( 1, -1, 4 ), ts_vec3( 0, 1, 4 ) );
	const int a = Scene::AddNode( new Node( mesh, Translation( 0, 0, 0 ) ) );
	const int b = Scene::AddNode( new Node( mesh, Translation( 4, 0, 0 ) ) );
	Scene::AddInstance( a );
	Scene::AddInstance( b );
	Scene::UpdateSceneGraph( 0 );
	TASK_CHECK( Scene::instPool.size() == 2 );
	// delete everything; repeated deletes of the same id are no-ops
	Scene::RemoveNode( a );
	Scene::RemoveNode( b );
	Scene::RemoveNode( a );
	Scene::RemoveNode( b );
	TASK_CHECK( Scene::rootNodes.size() == 0 );
	TASK_CHECK( Scene::nodePool.size() == 2 ); // slots stay, numbering stable
	TASK_CHECK( Scene::nodePool[a] == 0 && Scene::nodePool[b] == 0 );
	Scene::UpdateSceneGraph( 0 );
	TASK_CHECK( Scene::instPool.size() == 0 );
	tinybvh::Ray ray( tinybvh::bvhvec3( 0, 0, 0 ), tinybvh::bvhvec3( 0, 0, 1 ) );
	ray.hit.t = 7.0f;
	Scene::tlas->Intersect( ray );
	TASK_CHECK( ray.hit.t == 7.0f );
	TASK_CHECK( !Scene::tlas->IsOccluded( ray ) );
	// rebuild from the emptied state: new nodes share the surviving mesh
	const int c = Scene::AddNode( new Node( mesh, Translation( 0, 0, 0 ) ) );
	const int d = Scene::AddNode( new Node( mesh, Translation( 4, 0, 0 ) ) );
	Scene::AddInstance( c );
	Scene::AddInstance( d );
	Scene::UpdateSceneGraph( 0 );
	TASK_CHECK( Scene::instPool.size() == 2 );
	TASK_CHECK( Trace( 0, 0, 0 ).node == c );
	TASK_CHECK( Trace( 4, 0, 0 ).node == d );
	return 0;
}

} // anonymous namespace

const task_case task_cases[] =
{
	{ "topology", "root_subtree_delete", &CaseRootSubtreeDelete },
	{ "topology", "internal_subtree_delete", &CaseInternalSubtreeDelete },
	{ "topology", "dense_instances_and_mapping", &CaseDenseInstancesAndMapping },
	{ "topology", "rigid_mesh_delete", &CaseRigidMeshDelete },
	{ "lights", "shared_emissive_mesh_lights", &CaseSharedEmissiveMeshLights },
	{ "lights", "light_association_hierarchy", &CaseLightAssociationHierarchy },
	{ "empty_reuse", "empty_tlas_reuse", &CaseEmptyTlasReuse },
	{ "empty_reuse", "delete_all_and_rebuild", &CaseDeleteAllAndRebuild },
};
const size_t task_case_count = sizeof( task_cases ) / sizeof( task_cases[0] );
