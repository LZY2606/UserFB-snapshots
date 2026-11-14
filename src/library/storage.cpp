/***************************************************************************************************
* Cubiquity - A micro-voxel engine for games and other interactive applications                    *
*                                                                                                  *
* Written in 2019 by David Williams                                                                *
*                                                                                                  *
* To the extent possible under law, the author(s) have dedicated all copyright and related and     *
* neighboring rights to this software to the public domain worldwide. This software is distributed *
* without any warranty.                                                                            *
*                                                                                                  *
* You should have received a copy of the CC0 Public Domain Dedication along with this software.    *
* If not, see http://creativecommons.org/publicdomain/zero/1.0/.                                   *
***************************************************************************************************/
#include "storage.h"
#include "utility.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

#ifdef CUBIQUITY_USE_POOLSTL
#define POOLSTL_STD_SUPPLEMENT
#define POOLSTL_STD_SUPPLEMENT_FORCE
#include "poolstl.hpp"
#else
#include <execution>
#endif // CUBIQUITY_USE_POOLSTL

namespace Cubiquity
{
	using namespace Internals;

	// It is desirable for the spatial relationship between child nodes to be consistent
	// throughout the tree, as this makes it easier to e.g compute normals from children
	// and to compute intersections during ray traversal. However, the use of signed integers
	// for positions results in a different spatial relationship for children of the root
	// node due to the the sign bit being set for negative numbers.
	//
	// Therefore we map the signed ints to unsigned ints. The offset is required because 
	// otherwise negative ints get mapped to large unsigned values, inverting the spatial
	// relationship between negative and positive positions.
	void make_position_unsigned(i32 ix, i32 iy, i32 iz, u32& ux, u32& uy, u32& uz)
	{
		ux = static_cast<u32>(ix) + UINT32_C(0x80000000);
		uy = static_cast<u32>(iy) + UINT32_C(0x80000000);
		uz = static_cast<u32>(iz) + UINT32_C(0x80000000);
	}

	// Potentially optimise this by bit-interleaving these three ints into a
	// larger  96-bit int (std::bitset or 2x u64) in advance of the loop calling
	// this function. Then we would just have one shift-by-three and a mask on
	// each iteration. Shorter/simpler loop body but some setup cost. SIMD on 
	// three variables could be an alternative optimisation.
	u32 extract_next_child_id(u32& ux, u32& uy, u32& uz)
	{
		u32 childX = ux & 0x80000000;
		ux = ux << 1;
		u32 childY = uy & 0x80000000;
		uy = uy << 1;
		u32 childZ = uz & 0x80000000;
		uz = uz << 1;

		return childZ >> 29 | childY >> 30 | childX >> 31;
	}

	bool Internals::isMaterialNode(const Node& node)
	{
		// Less than MaterialCount and all matching
		return isMaterialNode(node[0]) &&
			std::all_of(node.begin(), node.end(),
				[&](int x) { return x == node[0]; });
	}

	// Not allowed to be an std::hash specialisation as Node is just a typedef.
	struct Internals::NodeHasher
	{
		u64 operator()(const Internals::Node& node) const noexcept
		{
			// Reference version baed on FNV-1a
			// return Internals::fnv1a(node.data(), sizeof(node));

			// Simple XOR-multiply hash with constants picked from MurmurHash3.
			Cubiquity::u64 hash = 0;
			hash = (hash ^ node[0]) * 0x87c37b91114253d5ULL;
			hash = (hash ^ node[1]) * 0x4cf5ad432745937fULL;
			hash = (hash ^ node[2]) * 0xff51afd7ed558ccdULL;
			hash = (hash ^ node[3]) * 0xc4ceb9fe1a85ec53ULL;
			hash = (hash ^ node[4]) * 0x87c37b91114253d5ULL;
			hash = (hash ^ node[5]) * 0x4cf5ad432745937fULL;
			hash = (hash ^ node[6]) * 0xff51afd7ed558ccdULL;
			hash = (hash ^ node[7]) * 0xc4ceb9fe1a85ec53ULL;
			hash = bit_mix(hash);
			return hash;
		}
	};

	// We populate the node vector with material nodes in which the children
	// point at themselves. This has a few uses:
	// 
	//   1. It lets us recurse to a single voxel even if the tree has been
	//      pruned (used when setting a voxel).
	//   2. New material nodes can be created by copying these (rather than
	//      needing to call 'make_node(...)') which reduces conditional
	//      logic during traversal.
	//   3. During merging they can be inserted into the map in the same way as
	//      non-material nodes (as they have a valid location in the vector).
	NodeVector Internals::make_node_vector(u32 count)
	{
		// Init with invalid nodes
		Node invalid_node = make_node(InvalidNodeIndex);
		NodeVector nodeVector(count, invalid_node);

		// Set material nodes
		assert(count >= MaterialCount);
		for (u32 i = 0; i < MaterialCount; i++) {
			nodeVector[i] = make_node(i);
		}
		return nodeVector;
	}

	NodeStore::NodeStore()
		:mNodeVector(make_node_vector())
		,mSharedNodesEnd(mNodeVector.size())
	{
		/*std::mt19937 rng;
		rng.seed(42);
		std::uniform_int_distribution<u32> u32_dist;

		NodeVector test_vec;
		int count = 100000000;
		for (int i = 0; i < count; i++)
		{
			test_vec.push_back(Node{ u32_dist(rng), u32_dist(rng),  u32_dist(rng),  u32_dist(rng),  u32_dist(rng),  u32_dist(rng),  u32_dist(rng),  u32_dist(rng) });
		}

		NodeLookupSorted node_lookup(test_vec, count);
		exit(0);*/
	}

	u32 NodeStore::getNodeChild(u32 nodeIndex, u32 childId) const
	{
		if (nodeIndex < MaterialCount) { // Can bypass memory access
			return nodeIndex;
		} else {
			return mNodeVector[nodeIndex][childId];
		}
	}

	u32 NodeStore::setNodeChild(u32 nodeIndex, u32 childId, u32 value)
	{
		// Sanity checks
		//assert(!isMaterialNode(nodeIndex) && "Can't modify material nodes");
		assert(value != nodeIndex && "New child points at parent");
		assert(value != mNodeVector[nodeIndex][childId] && "Self-assignment");

		// Copy the existing node if it is shared
		if (isShared(nodeIndex)) {
			nodeIndex = append(mNodeVector[nodeIndex]);
		}

		// Update the child
		mNodeVector[nodeIndex][childId] = value;
		return nodeIndex;
	}

	u32 NodeStore::countNodes(u32 startNodeIndex) const
	{
		std::unordered_set<u32> usedIndices;
		countNodes(startNodeIndex, usedIndices);
		return usedIndices.size();
	}

	// Counts the number of nodes at distinct locations (node indices).
	// A tree which is not fully merged will contain idetical nodes at different
	// locations in memory, and these will be counted seperately.
	void NodeStore::countNodes(u32 startNodeIndex, std::unordered_set<u32>& usedIndices) const
	{
		// It may have been more efficient to have done this test before calling
		// into this function, but the implementation is simpler this way around.
		if (isMaterialNode(startNodeIndex)) { return; }

		usedIndices.insert(startNodeIndex);
		for (const u32& childNodeIndex : mNodeVector[startNodeIndex])
		{
			countNodes(childNodeIndex, usedIndices);
		}
	}

	void NodeStore::read(std::ifstream& file)
	{
		u32 nodeCount = 0;
		file.read(reinterpret_cast<char*>(&nodeCount), sizeof(nodeCount));
		if (!file) { return; }

		// A truncated or corrupt file can claim an arbitrary node count. Reject
		// counts which cannot possibly fit in the remaining bytes of the file
		// before allocating, so a bad load cannot exhaust memory.
		if (nodeCount > (UINT32_MAX - MaterialCount)) {
			file.setstate(std::ios::failbit);
			return;
		}

		std::streampos current = file.tellg();
		if (current != std::streampos(-1)) {
			file.seekg(0, std::ios::end);
			const std::streampos end = file.tellg();
			file.seekg(current, std::ios::beg);
			if ((end == std::streampos(-1)) || (current == std::streampos(-1))) {
				file.setstate(std::ios::failbit);
				return;
			}
			const std::streamoff remaining = end - current;
			if (remaining < static_cast<std::streamoff>(sizeof(Node)) * nodeCount) {
				file.setstate(std::ios::failbit);
				return;
			}
		}

		mNodeVector.resize(MaterialCount + nodeCount);
		if (nodeCount > 0) {
			file.read(reinterpret_cast<char*>(&(mNodeVector[sharedNodesBegin()])), sizeof(Node) * nodeCount);
		}
		mSharedNodesEnd = mNodeVector.size(); // One past end, no unshared nodes.
	}

	void NodeStore::write(std::ofstream& file)
	{
		u32 nodeCount = sharedNodesEnd() - sharedNodesBegin();
		file.write(reinterpret_cast<const char*>(&nodeCount), sizeof(nodeCount));
		file.write(reinterpret_cast<const char*>(&mNodeVector[sharedNodesBegin()]), sizeof(Node) * nodeCount);
	}

	u32 NodeStore::append(const Node& node)
	{
		mNodeVector.push_back(node);
		return mNodeVector.size() - 1; // Index of new node
	}

	// Produce a compacted node vector which contains (once each) every node which is
	// reachable from any of 'roots'. Identical nodes are deduplicated, so after this
	// call the nodes reachable from the returned roots are fully shared. The input
	// store is left untouched.
	//
	// The implementation is deliberately independent of the shared/unshared split,
	// because such a split no longer exists once the result replaces the store.
	NodeVector NodeStore::mergeNodes(const std::vector<u32>& roots, std::vector<u32>& mergedRoots) const
	{
		// A DAG can be exponentially larger than the number of nodes it contains, so
		// we memoise every node which has already been copied to the destination.
		std::unordered_map<u32, u32> nodeMap;
		std::unordered_map<Node, u32, NodeHasher> nodeLookup;
		NodeVector merged = make_node_vector();
		merged.reserve(mNodeVector.size()); // Number of distinct nodes can never exceed the source.

		std::function<u32(u32)> mergeNode = [&](u32 nodeIndex) -> u32 {
			if (isMaterialNode(nodeIndex)) { return nodeIndex; }

			const auto existing = nodeMap.find(nodeIndex);
			if (existing != nodeMap.end()) { return existing->second; }

			Node node = mNodeVector[nodeIndex];
			for (u32 childId = 0; childId < 8; childId++) {
				node[childId] = mergeNode(node[childId]);
			}

			// If the children all ended up identical then collapse this node into a
			// material reference. This keeps the result pruned while still handling
			// nodes which were not pruned in the source store.
			u32 newNodeIndex;
			if (isMaterialNode(node)) {
				newNodeIndex = node[0];
			}
			else {
				const auto lookup = nodeLookup.find(node);
				if (lookup != nodeLookup.end()) {
					newNodeIndex = lookup->second;
				}
				else {
					newNodeIndex = static_cast<u32>(merged.size());
					merged.push_back(node);
					nodeLookup.emplace(node, newNodeIndex);
				}
			}

			nodeMap.emplace(nodeIndex, newNodeIndex);
			return newNodeIndex;
		};

		mergedRoots.clear();
		mergedRoots.reserve(roots.size());
		for (const u32 root : roots) {
			mergedRoots.push_back(mergeNode(root));
		}

		// Nodes below a subtree which collapsed to a material (because it became
		// uniform) were merged before we knew the parent would be pruned. Drop
		// everything which is not actually reachable from a returned root, both to
		// avoid leaking nodes into the compacted store and to guarantee that no
		// unreferenced nodes remain.
		std::vector<u32> remap(merged.size(), InvalidNodeIndex);
		std::vector<u32> pending;
		for (const u32 root : mergedRoots) {
			if (!isMaterialNode(root)) { pending.push_back(root); }
		}
		while (!pending.empty()) {
			const u32 nodeIndex = pending.back();
			pending.pop_back();
			if (remap[nodeIndex] != InvalidNodeIndex) { continue; }
			remap[nodeIndex] = 0; // Marked, assigned below.
			for (const u32 child : merged[nodeIndex]) {
				if (!isMaterialNode(child)) { pending.push_back(child); }
			}
		}

		u32 nextNodeIndex = MaterialCount;
		for (u32 i = MaterialCount; i < merged.size(); i++) {
			if (remap[i] != InvalidNodeIndex) { remap[i] = nextNodeIndex++; }
		}

		NodeVector compacted = make_node_vector(nextNodeIndex);
		for (u32 i = MaterialCount; i < merged.size(); i++) {
			if (remap[i] == InvalidNodeIndex) { continue; }
			Node node = merged[i];
			for (u32 childId = 0; childId < 8; childId++) {
				if (!isMaterialNode(node[childId])) {
					node[childId] = remap[node[childId]];
				}
			}
			compacted[remap[i]] = node;
		}

		for (u32& root : mergedRoots) {
			if (!isMaterialNode(root)) { root = remap[root]; }
		}

		return compacted;
	}

	void NodeStore::replaceNodes(NodeVector&& nodes)
	{
		mNodeVector = std::move(nodes);
		mSharedNodesEnd = mNodeVector.size(); // Everything is shared / merged.
	}

	u32 NodeStore::cloneRoot(u32 rootIndex)
	{
		mSharedNodesEnd = mNodeVector.size(); // Mark everything as potentially shared
		mNodeVector.push_back(mNodeVector[rootIndex]); // Append unshared copy of root
		return mNodeVector.size() - 1; // Return position of new node
	}

	void NodeStore::print(std::ostream& os)
	{
		for (u32 i = 0; i < mNodeVector.size(); i++)
		{
			if (i == mSharedNodesEnd)
			{
				os << "\n-------------------- Shared nodes end / unshared nodes begin -------------------\n\n";
			}

			os << std::setw(4) << i << ": ";
			for (int c = 0; c < 8; c++)
			{
				os << std::setw(4) << mNodeVector[i][c] << " ";
			}
			os << "\n";
		}

		os << "\nShared Nodes end = " << mSharedNodesEnd << "\n";
	}

	////////////////////////////////////////////////////////////////////////////////
	// Public member functions
	////////////////////////////////////////////////////////////////////////////////

	Volume::Volume()
	{
		mRootNodeIndices.resize(1);
		mCurrentRoot = 0;
	}

	Volume::Volume(const std::string& filename)
	{
		mRootNodeIndices.resize(1);
		mCurrentRoot = 0;

		load(filename);
	}

	void Volume::fill(MaterialId matId)
	{
		setRootNodeIndex(matId);
	}

	u32 Volume::rootNodeIndex() const
	{
		return mRootNodeIndices[mCurrentRoot];
	}

	void Volume::setRootNodeIndex(u32 newRootNodeIndex)
	{
		mRootNodeIndices[mCurrentRoot] = newRootNodeIndex;
		++mRevision;
	}

	void Volume::checkpoint()
	{
		// If we 'undo' a few times and then create a checkpoint then the 'redo'
		// checkpoints are invalidated. Discard them by cropping the vector.
		mRootNodeIndices.resize(mCurrentRoot + 1); // Clear any 'redo' items

		// Clone the current root
		u32 root = mRootNodeIndices[mCurrentRoot];
		u32 newRoot = mNodeStore.cloneRoot(root);

		// Store it
		mRootNodeIndices.push_back(newRoot);
		mCurrentRoot++;

		// Navigating history changes the visible scene even if the root index is
		// unchanged, so readers must be told to refresh.
		++mRevision;
	}

	void Volume::undo()
	{
		if (mCurrentRoot > 0) {
			mCurrentRoot--;
			++mRevision;
		}
	}

	void Volume::redo()
	{
		if (mCurrentRoot < mRootNodeIndices.size() - 1)	{
			mCurrentRoot++;
			++mRevision;
		}
	}

	// Compact the node store while retaining the entire history (including redo
	// states). All roots are remapped into a single merged store so they continue
	// to share nodes; the current history position is preserved.
	void Volume::bake()
	{
		Cubiquity::Timer timer;

		std::vector<u32> mergedRoots;
		NodeVector mergedNodes = mNodeStore.mergeNodes(mRootNodeIndices, mergedRoots);

		const u32 nodeCount = static_cast<u32>(mergedNodes.size());
		mNodeStore.replaceNodes(std::move(mergedNodes));
		mRootNodeIndices = std::move(mergedRoots);
		++mRevision;

		log_debug("Baked %u nodes in %.3f seconds", nodeCount, timer.elapsed_seconds());
	}

	void Volume::setVoxel(i32 x, i32 y, i32 z, MaterialId matId)
	{
		// Map to unsigned positions
		u32 ux, uy, uz;
		make_position_unsigned(x, y, z, ux, uy, uz);

		const int rootHeight = log_base_2(VolumeSideLength);
		u32 newRootNodeIndex = setVoxel(ux, uy, uz, matId, rootNodeIndex(), rootHeight);
		setRootNodeIndex(newRootNodeIndex);
	}

	// I previously had iterative versions of ths function which do perform
	// better but are a little harder to reason about. One version gives the
	// same behaviour but is faster (possibly due to easier handling of the
	// early out), and another version writes nodes on the way down the tree
	// (it assumes the voxel isn't already set) rather on the way back up - this
	// is faster still. But I don't want to maintain differnt versions at the
	// moment, and the recursive version is easier. The others can be found in
	// the commit history on the NodeStore branch (present in Jan 2026).
	u32 Volume::setVoxel(u32 ux, u32 uy, u32 uz, MaterialId matId, u32 nodeIndex, int nodeHeight)
	{
		// Also reachable directly (e.g. from brush code), so make sure caches see
		// the change even when the caller commits the returned index itself.
		++mRevision;

		// Get the relevant child node
		u32 childId = extract_next_child_id(ux, uy, uz); // Modifies params
		u32 childNodeIndex = mNodeStore.getNodeChild(nodeIndex, childId);

		// Check if already set to desired material
		if (childNodeIndex == matId) { return nodeIndex; }

		// Recusively process the child node until we reach a just-above-leaf node (height of 1).
		if (nodeHeight > 1)
		{
			u32 newChildNodeIndex = setVoxel(ux, uy, uz, matId, childNodeIndex, nodeHeight - 1);

			// If the child hasn't changed then we don't need to update the current node.
			if (newChildNodeIndex == childNodeIndex) { return nodeIndex; }

			return mNodeStore.setNodeChild(nodeIndex, childId, newChildNodeIndex);
		}
		else
		{
			return mNodeStore.setNodeChild(nodeIndex, childId, matId);
		}
	}

	void Volume::combine(const Volume& rhsVolume, const MaterialCombiner& combiner)
	{
		u32 newRootNodeIndex = combine(rhsVolume, rhsVolume.rootNodeIndex(), rootNodeIndex(), combiner);
		setRootNodeIndex(newRootNodeIndex);
	}

	// Fixme - This function can prbably be more efficient. Firstly through better early out when whole nodes are full/empty, and also
	// if seperate volumes shared a common memory space it would be easier to copy node directly across (maybe in compressd space?).
	u32 Volume::combine(const Volume& rhsVolume, u32 rhsNodeIndex, u32 nodeIndex, const MaterialCombiner& combiner)
	{
		++mRevision; // Conservative; see setVoxel().

		for (u32 childId = 0; childId < 8; childId++)
		{
			// If current node is a material then just propergate it. Otherwise get the true child.
			u32 childNodeIndex = mNodeStore.getNodeChild(nodeIndex, childId);
			u32 rhsChildNodeIndex = rhsVolume.mNodeStore.getNodeChild(rhsNodeIndex, childId);

			// Process children
			u32 newChildNodeIndex = childNodeIndex;

			if (isMaterialNode(childNodeIndex) && isMaterialNode(rhsChildNodeIndex))
			{
				newChildNodeIndex = combiner(childNodeIndex, rhsChildNodeIndex);
			}
			else
			{
				newChildNodeIndex = combine(rhsVolume, rhsChildNodeIndex, childNodeIndex, combiner);
			}

			// If the child has changed then we need to update the current node.
			if (childNodeIndex != newChildNodeIndex)
			{
				nodeIndex = mNodeStore.setNodeChild(nodeIndex, childId, newChildNodeIndex);
			}
		}

		return nodeIndex;
	}

	MaterialId Volume::voxel(i32 x, i32 y, i32 z) const
	{
		// TODO: Consider starting from split point instead of root
		u32 nodeIndex = rootNodeIndex();

		// Map to unsigned positions
		u32 ux, uy, uz;
		make_position_unsigned(x, y, z, ux, uy, uz);

		// Descend tree. Memory access is apparent bottleneck. Improve node
		// ordering for better cache coherency or try memory prefetch?
		while (!isMaterialNode(nodeIndex))
		{
			u32 childId = extract_next_child_id(ux, uy, uz); // Modifies params
			nodeIndex = mNodeStore[nodeIndex][childId];
		}

		return static_cast<MaterialId>(nodeIndex);
	}

	////////////////////////////////////////////////////////////////////////////////
	// Private member functions
	////////////////////////////////////////////////////////////////////////////////

	bool Volume::load(const std::string& filename)
	{
		std::ifstream file(filename, std::ios::binary);

		// FIXME - What should we do for error handling in this function? Return codes or exceptions?
		if(!file.is_open())
		{
			return false; // Leave the existing scene and history untouched.
		}

		u32 rootNodeIndex = InvalidNodeIndex;
		file.read(reinterpret_cast<char*>(&rootNodeIndex), sizeof(rootNodeIndex));
		if (!file)
		{
			return false; // Leave the existing scene and history untouched.
		}

		// Read into a temporary store so a truncated/failed read does not corrupt
		// the volume which is currently in memory.
		Internals::NodeStore loadedStore;
		loadedStore.read(file);

		if (!file)
		{
			return false;
		}

		if (rootNodeIndex >= MaterialCount)
		{
			rootNodeIndex += loadedStore.sharedNodesBegin() - MaterialCount;
		}

		// A successful load replaces both the scene and its history. Previously
		// recorded states are intentionally no longer reachable through undo.
		mNodeStore = std::move(loadedStore);
		mRootNodeIndices.assign(1, rootNodeIndex);
		mCurrentRoot = 0;
		++mRevision;

		return true;
	}

	void Volume::save(const std::string& filename)
	{
		// Only the current scene is written, and the on-disk layout is unchanged
		// (root index followed by the node array starting at MaterialCount).
		// Compact a copy rather than baking the live store, so saving does not
		// destroy in-memory history.
		std::vector<u32> roots = { rootNodeIndex() };
		std::vector<u32> mergedRoots;
		const NodeVector mergedNodes = mNodeStore.mergeNodes(roots, mergedRoots);

		const u32 root = mergedRoots[0];
		const u32 nodeCount = static_cast<u32>(mergedNodes.size()) - MaterialCount;

		// Write compacted nodes directly. Note they start at 'MaterialCount' but the
		// on-disk format deliberately omits the material slots.
		std::ofstream file(filename, std::ios::out | std::ios::binary);
		file.write(reinterpret_cast<const char*>(&root), sizeof(root));
		file.write(reinterpret_cast<const char*>(&nodeCount), sizeof(nodeCount));
		for (u32 i = MaterialCount; i < mergedNodes.size(); ++i) {
			file.write(reinterpret_cast<const char*>(&mergedNodes[i]), sizeof(Node));
		}
		file.close();
	}

	void Volume::print(std::ostream& os)
	{
		mNodeStore.print(os);

		os << "Root nodes: [";
		for (const auto& rootNodeIndex : mRootNodeIndices)
		{
			os << rootNodeIndex << ", ";
		}
		os << "]\n";

		os << "Current root index = " << mCurrentRoot << "\n";
		os << "Current root value = " << mRootNodeIndices[mCurrentRoot] << "\n";
	}

	namespace Internals
	{
		/// This is an advanced function which should only be used if you
		/// understand the internal memory layout of Cubiquity's volume data.
		NodeStore& getNodes(Volume& volume)
		{
			return volume.mNodeStore;
		}

		const NodeStore& getNodes(const Volume& volume)
		{
			return volume.mNodeStore;
		}

		const u32 getRootNodeIndex(const Volume& volume)
		{
			return volume.rootNodeIndex();
		}
	}
}
