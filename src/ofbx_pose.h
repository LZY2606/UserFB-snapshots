#pragma once

// Pose evaluation and skinning helpers built on top of ofbx.h.
//
// All entry points are read-only: neither Object instances, animation curves nor
// previously returned output vectors are modified. Queries may be issued in any
// order, interleaved across different IScene instances.
//
// Space conventions
// -----------------
// NodePose::world is the node's world matrix in the scene. It is produced by the
// same inheritance engine (RrSs / RSrs / Rrs) as Object::getGlobalTransform(), and
// it never contains a Mesh geometric transform: geometric transforms affect only
// the geometry attached to that Mesh and are never propagated to child nodes.
//
// The cluster matrices returned by buildSkinMatrices() accept the *raw* control
// points stored in the Geometry (column vectors, homogeneous w = 1) and implement,
// in this order:
//
//   p_mesh_local = Mesh::getGeometricMatrix() * p_geometry
//   p_bind       = Cluster::getTransformMatrix()    * p_mesh_local
//   p_link_local = inverse(Cluster::getTransformLinkMatrix()) * p_bind
//   p_world      = NodePose.world(link)             * p_link_local
//
// i.e. M = world_link * inverse(TransformLink) * Transform * Geometric.
// The geometric transform is already included exactly once; callers must not apply
// Mesh::getGeometricMatrix() a second time. Weighted blending across clusters is
// left to the caller.
//
// Typical call from an evaluated pose to a skinned point:
//
//   std::vector<ofbx::NodePose> pose;
//   if (!ofbx::evaluatePose(*scene, layer, seconds, pose)) { /* ... */ }
//
//   std::vector<ofbx::SkinPose> skins;
//   if (!ofbx::buildSkinMatrices(*mesh, pose, skins)) { /* ... */ }
//
//   ofbx::DVec3 point = geometry_control_point; // raw Geometry position
//   ofbx::DVec3 deformed{0, 0, 0};
//   for (const ofbx::SkinPose& skin : skins)
//       for (const ofbx::ClusterPose& cp : skin.clusters)
//       {
//           const double w = weight_of(mesh, cp.cluster, point_index); // your data
//           const double* m = cp.matrix.m; // column major, w = 1
//           const ofbx::DVec3 p{
//               m[0] * point.x + m[4] * point.y + m[8]  * point.z + m[12],
//               m[1] * point.x + m[5] * point.y + m[9]  * point.z + m[13],
//               m[2] * point.x + m[6] * point.y + m[10] * point.z + m[14]};
//           deformed.x += w * p.x; deformed.y += w * p.y; deformed.z += w * p.z;
//       }

#include "ofbx.h"
#include <vector>

namespace ofbx
{

struct NodePose
{
	const Object* node = nullptr;
	// Full world matrix of `node`, including every ancestor and the node's
	// InheritType. Pivots, offsets, Pre/PostRotation and the Euler order are
	// already baked in.
	DMatrix world;
};

struct ClusterPose
{
	const Cluster* cluster = nullptr;
	// Matrix turning raw Geometry control points into current world space for
	// this cluster. See the space conventions above.
	DMatrix matrix;
};

struct SkinPose
{
	const Skin* skin = nullptr;
	// One entry per Skin::getCluster(i), in the same order.
	std::vector<ClusterPose> clusters;
};


// Evaluates the world pose of every isNode() object of `scene`.
// The output order matches the relative order of isNode() objects in
// IScene::getAllObjects().
//
// `layer` == nullptr evaluates the static pose (equivalent to
// Object::getGlobalTransform()). Otherwise the Lcl Translation / Lcl Rotation /
// Lcl Scaling curve nodes present in that layer are sampled at `seconds`:
//   * a property without a curve node keeps the node's static value;
//   * a component without an animation curve uses the curve node's default value.
// Curve interpolation stays linear and is not extended here.
// Ancestor chains are followed in full, so the file object order need not be
// parent-first.
//
// Returns false (leaving `out` untouched) when:
//   * `seconds` is not finite;
//   * `layer` belongs to a different IScene;
//   * a node carries an InheritType outside 0..2;
//   * a node's local scaling is not strictly positive (zero scale, reflection),
//     or any sampled value/resulting matrix is not finite.
// The supported local transform domain is finite TRS with positive scaling;
// hand authored shear is not required to be handled.
bool evaluatePose(const IScene& scene, const AnimationLayer* layer, double seconds, std::vector<NodePose>& out);


// Builds skinning matrices for every Skin attached to `mesh` from a pose
// previously produced by evaluatePose() on the same scene.
//
// Returns false (leaving `out` untouched) when:
//   * a cluster has no link Object, or the link is not present in `pose`;
//   * the cluster's TransformLink matrix is not invertible;
//   * any intermediate matrix is not finite.
// A Mesh without a Skin succeeds and produces an empty `out`.
bool buildSkinMatrices(const Mesh& mesh, const std::vector<NodePose>& pose, std::vector<SkinPose>& out);

} // namespace ofbx
