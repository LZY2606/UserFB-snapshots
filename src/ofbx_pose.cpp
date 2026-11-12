#include "ofbx_pose.h"
#include "ofbx_pose_internal.h"

#include <cmath>
#include <unordered_map>

namespace ofbx
{

using namespace pose_detail;

namespace
{

struct EvaluatedNode
{
	const Object* object = nullptr;
	NodeLocal local;
	const Object* parent = nullptr;
	int parent_index = -1; // -1 when the parent is the implicit scene Root (identity world)
	DMatrix world;
	unsigned char state = 0; // 0 = unvisited, 1 = visiting, 2 = done
};


bool evaluateLocal(const Object& object, const AnimationLayer* layer, double seconds, EvaluatedNode& node)
{
	DVec3 translation = object.getLocalTranslation();
	DVec3 rotation = object.getLocalRotation();
	DVec3 scaling = object.getLocalScaling();

	if (layer)
	{
		if (const AnimationCurveNode* cn = layer->getCurveNode(object, "Lcl Translation"))
		{
			translation = cn->getNodeLocalTransform(seconds);
		}
		if (const AnimationCurveNode* cn = layer->getCurveNode(object, "Lcl Rotation"))
		{
			rotation = cn->getNodeLocalTransform(seconds);
		}
		if (const AnimationCurveNode* cn = layer->getCurveNode(object, "Lcl Scaling"))
		{
			scaling = cn->getNodeLocalTransform(seconds);
		}
	}

	if (!isFinite(translation.x) || !isFinite(translation.y) || !isFinite(translation.z)) return false;
	if (!isFinite(rotation.x) || !isFinite(rotation.y) || !isFinite(rotation.z)) return false;
	// Supported domain: strictly positive local scaling. Zero scale and reflections
	// are rejected; hand authored shear cannot be expressed through TRS properties.
	if (!supportedScale(scaling)) return false;

	const int inherit = object.getInheritType();
	if (inherit < 0 || inherit > 2) return false;

	const DMatrix matrix = object.evalLocal(translation, rotation, scaling);
	if (!finiteMatrix(matrix)) return false;

	node.local = NodeLocal{ matrix, scaling, inherit };
	return true;
}


bool resolveWorld(std::vector<EvaluatedNode>& nodes, int index)
{
	EvaluatedNode& node = nodes[index];
	if (node.state == 2) return true;
	if (node.state == 1) return false; // cycle, defensive only; loading rejects these
	node.state = 1;

	if (node.parent_index < 0 || !node.parent)
	{
		node.world = node.local.matrix;
	}
	else
	{
		if (!resolveWorld(nodes, node.parent_index)) return false;
		const EvaluatedNode& parent = nodes[node.parent_index];
		node.world = inheritCompose(parent.world, parent.local.scale, node.local);
	}

	if (!finiteMatrix(node.world)) return false;
	node.state = 2;
	return true;
}

} // namespace


bool evaluatePose(const IScene& scene, const AnimationLayer* layer, double seconds, std::vector<NodePose>& out)
{
	if (!std::isfinite(seconds)) return false;
	if (layer && &(layer->getScene()) != &scene) return false;

	const int object_count = scene.getAllObjectCount();
	const Object* const* objects = scene.getAllObjects();

	std::vector<EvaluatedNode> nodes;
	std::unordered_map<const Object*, int> index_by_object;
	nodes.reserve(object_count);
	index_by_object.reserve(object_count * 2);

	// Output order is the relative isNode() order inside getAllObjects().
	for (int i = 0; i < object_count; ++i)
	{
		const Object* object = objects[i];
		if (!object->isNode()) continue;

		EvaluatedNode node;
		if (!evaluateLocal(*object, layer, seconds, node)) return false;

		node.object = object;
		node.parent = object->getParent();

		index_by_object.emplace(object, (int)nodes.size());
		nodes.push_back(node);
	}

	// Parent indices are resolved only after every node is indexed: the file
	// object order is not guaranteed to be parent-first. A parent missing from
	// the index is the synthetic scene Root, whose world matrix is identity.
	for (EvaluatedNode& node : nodes)
	{
		node.parent_index = -1;
		if (!node.parent) continue;
		auto it = index_by_object.find(node.parent);
		if (it != index_by_object.end()) node.parent_index = it->second;
	}

	// A parent can appear later in the file than its children, so resolve every
	// chain explicitly instead of assuming parent-first ordering.
	for (int i = 0; i < (int)nodes.size(); ++i)
	{
		if (!resolveWorld(nodes, i)) return false;
	}

	std::vector<NodePose> result;
	result.reserve(nodes.size());
	for (const EvaluatedNode& node : nodes)
	{
		result.push_back(NodePose{ node.object, node.world });
	}

	out = std::move(result);
	return true;
}


bool buildSkinMatrices(const Mesh& mesh, const std::vector<NodePose>& pose, std::vector<SkinPose>& out)
{
	std::unordered_map<const Object*, const DMatrix*> world_by_node;
	world_by_node.reserve(pose.size() * 2);
	for (const NodePose& np : pose)
	{
		world_by_node.emplace(np.node, &np.world);
	}

	const DMatrix geometric = mesh.getGeometricMatrix();
	if (!finiteMatrix(geometric)) return false;

	std::vector<SkinPose> result;
	const Skin* skin = mesh.getSkin();
	if (skin)
	{
		SkinPose skin_pose;
		skin_pose.skin = skin;
		skin_pose.clusters.reserve(skin->getClusterCount());

		for (int i = 0; i < skin->getClusterCount(); ++i)
		{
			const Cluster* cluster = skin->getCluster(i);
			const Object* link = cluster->getLink();
			if (!link) return false;

			auto it = world_by_node.find(link);
			if (it == world_by_node.end()) return false;

			const DMatrix transform = cluster->getTransformMatrix();
			const DMatrix transform_link = cluster->getTransformLinkMatrix();
			if (!finiteMatrix(transform) || !finiteMatrix(transform_link)) return false;

			DMatrix inv_link;
			if (!invertAffine(transform_link, inv_link)) return false;

			// Raw geometry control point -> mesh local (geometric) -> bind scene
			// (Transform) -> link local (inverse TransformLink) -> current scene
			// (link world pose). Geometric is included exactly once.
			DMatrix matrix = multiply(*it->second, multiply(inv_link, multiply(transform, geometric)));
			if (!finiteMatrix(matrix)) return false;

			skin_pose.clusters.push_back(ClusterPose{ cluster, matrix });
		}
		result.push_back(std::move(skin_pose));
	}

	out = std::move(result);
	return true;
}

} // namespace ofbx
