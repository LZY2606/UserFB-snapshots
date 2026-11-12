Packet Traversal
================

`FastBVH::PacketTraverser` traces four rays through a BVH using a single
traversal stack shared by all four lanes. It is the batch counterpart of
`FastBVH::Traverser`, which traces one ray per call.

The implementation is portable C++: it uses no SIMD intrinsics and works for
`float` and `double`. Geometry is still supplied entirely by the user's
intersector; the library makes no assumption about primitive shape.

Basic Use
---------

The constructor takes the same arguments as the single-ray traverser:

```cxx
FastBVH::PacketTraverser<float, Primitive, Intersector> packet(bvh, intersector);

std::array<FastBVH::Ray<float>, 4> rays = { /* four rays */ };
std::array<float, 4> tmin = { 0, 0, 0, 0 };
std::array<float, 4> tmax = {
  std::numeric_limits<float>::infinity(),
  std::numeric_limits<float>::infinity(),
  std::numeric_limits<float>::infinity(),
  std::numeric_limits<float>::infinity()
};

FastBVH::PacketStats stats;
auto hits = packet.traverse(rays, tmin, tmax, 0x0F /* all four lanes */, &stats);
```

`traverse()` returns four `Intersection` instances, one per lane.
`occluded()` takes the same arguments and returns a `uint8_t` mask whose low
four bits mark the lanes that found an intersection inside their window.

Contract
--------

 - Each lane has its own nearest hit distance. A hit is accepted only when
   its parameter lies inside that lane's **closed** interval `[tmin, tmax]`.
 - Direction vectors need not be normalized. Coordinates must be finite and
   directions non-zero. `tmin` must be non-negative, `tmin <= tmax` per lane,
   and `tmax` may be positive infinity.
 - The intersector returns a single candidate intersection per primitive. If
   the candidate lies outside the lane's window it is ignored; the library
   does not ask the intersector for the primitive's other root.
 - Axis-parallel directions, including negative zero, are handled with an
   explicit branch so a ray lying exactly on a box boundary is never lost.
 - On an exact distance tie, either hit is acceptable.
 - Lanes outside the `active` mask return an empty intersection and **never**
   invoke the intersector.
 - `active == 0` and an empty tree return immediately.

Sharing The Stack
-----------------

Each stack entry records the set of lanes that still need the node, along
with that lane's own `near`/`far` interval for the node. Because the lane
sets and windows travel with the stack entry:

 - A near hit found by one lane never tightens the distance window of another
   lane.
 - When two children are relevant to different lanes, both children are
   pushed with their own lane sets.

A query only reads the BVH, the rays and the primitives. Working storage
(stack, statistics) belongs to the call, so concurrent queries against the
same read-only tree are safe.

Statistics
----------

`PacketStats` is declared in the same header. Every query starts counting at
zero when a `PacketStats*` is supplied; omitting the pointer skips counting.

| Counter | Meaning |
| --- | --- |
| `node_loads` | Number of node records read from the node array. This is the node being processed plus the two child records examined during a box test. A stack entry whose lanes have all been resolved is skipped before its record is read and contributes nothing. |
| `box_lane_tests` | Number of individual (bounding box, ray lane) intersection tests. Each lane is counted separately. |
| `primitive_lane_tests` | Number of calls to the user's primitive intersector. One call per (primitive, served lane) pair. |

The accounting is done per lane rather than per four-wide batch sweep: there
is no "free" scan of the other lanes whose tests go uncounted. Stats are reset
on entry to every `traverse()` and `occluded()` call.

Example
-------

`examples/PacketPPM.cpp` renders a small, window-less PPM image by tracing
four horizontally adjacent pixels per query and printing the packet
statistics. Build it with `cmake` or `make` and it writes `packet.ppm`.
