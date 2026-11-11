This implementation is 100% Public Domain.

Feel free to use.
 
C++11 is needed to compile it.

Basic usage:

	#include "quickhull/quickhull.hpp"

	using namespace quickhull;
	QuickHull<float> qh; // Could be double as well
	std::vector<Vector3<float>> pointCloud;
	// Add points to point cloud
	...
	auto hull = qh.getConvexHull(pointCloud, true, false);
	const auto& indexBuffer = hull.getIndexBuffer();
	const auto& vertexBuffer = hull.getVertexBuffer();
	// Do what you want with the convex triangle mesh

Vertex data can be passed as a pointer to float/double as long as the data is in X_0,Y_0,Z_0,X_1,Y_1,Z_1,...,X_N,Y_N_Z_N format:

	auto hull = qh.getConvexHull(&pointCloud[0].x, pointCloud.size(), true, false);

The first boolean parameter of getConvexHull specifies whether the resulting mesh should have its triangles in CCW orientation.

The second boolean parameter specifies whether the mesh should use vertex indices of the original point cloud. If it is false, a new vertex buffer is generated which consists only of those vertices that are part of the convex hull. In this case, the new vertex buffer is owned by the returned ConvexHull object. Otherwise, the original point cloud is used as vertex buffer and since the vertices are not copied, make sure you don't call ConvexHull::getVertexBuffer after releasing the memory that contains the original point cloud data.

This implementation is fast, because the convex hull is internally built using a half edge mesh representation which provides quick access to adjacent faces. It is also possible to get the output convex hull as a half edge mesh:

	auto mesh = qh.getConvexHullAsMesh(&pointCloud[0].x, pointCloud.size(), true);

Incremental hull
---------------

`IncrementalHull<T>` (in `IncrementalHull.hpp`) is a stateful session for point
clouds that grow over time.  It owns its point storage and keeps the half-edge
hull alive between batches, so an append only expands the part of the hull that
the new points actually change.  The one-shot `QuickHull` entry points are
unaffected and keep their previous behaviour; both implementations share the
same visible-face/horizon/half-edge expansion code in `HullExpansion.hpp`.

Construction requires an initial point batch and a strictly positive, finite
**absolute** distance tolerance.  The initial set must be finite and span
three-dimensional space; degenerate (coincident, collinear or coplanar) initial
sets throw `std::invalid_argument`.  The tolerance is fixed for the lifetime of
the session: it is not rescaled when later, far-away points are appended.

Every point given to the constructor or to `append` receives a permanent index,
starting at zero and growing monotonically.  Duplicate points and points inside
the hull are stored and numbered too.  The input arrays are copied, so they may
be released or modified immediately after the call.  A batch containing NaN or
infinity is rejected atomically before any topology is touched; a later valid
`append` can still be used.

	#include "IncrementalHull.hpp"

	using namespace quickhull;

	std::vector<Vector3<double>> initial = { ... }; // must span 3D
	IncrementalHull<double> hull(initial, 1e-6);   // absolute tolerance

	// Build the client-side face table once, from the initial snapshot.
	auto first = hull.snapshot();
	// first.points        : copy of all stored points
	// first.triangles[i]  : { faceId, vertexIndices }
	// Triangles are outward-facing CCW and reference permanent point indices.

	std::vector<Vector3<double>> more = { ... };
	auto update = hull.append(more);
	// update.deletedFaceIds : externally visible face ids to remove
	// update.addedFaces[i]  : { faceId, vertexIndices } to add

External face ids are never reused, even though the internal `MeshBuilder`
recycles disabled face slots and half edges.  Consequently a client may keep
per-face state such as a material id keyed by external id: a new face can never
inherit the state of a deleted face that used the same internal slot.  Faces
which are not eliminated by an expansion keep their id and corner order.

Applying the returned updates to the initial snapshot (remove deleted ids, add
new ids) always yields exactly the session's current snapshot, including face
ids, winding and point references.  An interior, duplicate-only or empty batch
returns an empty update.

`harness/check_incremental.sh` builds the original test suite and the
incremental harness in a temporary directory and runs both.  The harness checks
closed oriented manifold structure, Euler characteristic, supporting-plane
containment, one-shot volume/support equivalence and client-side update
application for the same point set under two different batching orders.

