#include "task_support/check.h"

#include "libs/yocto/yocto_bvh.h"
#include "libs/yocto/yocto_scene.h"
#include "libs/yocto/yocto_sceneio.h"
#include "libs/yocto/yocto_shape.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <queue>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace yocto;

namespace {

// -----------------------------------------------------------------------------
// TOPOLOGY HELPERS
// -----------------------------------------------------------------------------

// Face ring of a quad with a triangle stored as a repeated last corner.
int ring_corners(const vec4i& ring) { return ring.z == ring.w ? 3 : 4; }

struct mesh_topology {
  int faces         = 0;
  int vertices      = 0;
  int boundary_edges = 0;
  int boundary_loops = 0;
  int face_components = 0;
  int non_manifold   = 0;
  int inconsistent   = 0;
  int multi_fan_vertices = 0;
};

// Inspect the combinatorial topology of a quad mesh, treating triangles as
// repeated last corners and ignoring geometry (identical positions never merge
// here since indices are the only identity).
mesh_topology inspect_topology(const vector<vec4i>& quads) {
  auto topo = mesh_topology{};
  topo.faces = (int)quads.size();
  auto edge_faces =
      std::map<std::pair<int, int>, std::vector<std::pair<int, int>>>{};
  auto used        = std::set<int>{};
  auto face_adj    = std::vector<std::vector<int>>(quads.size());
  for (auto fid : range(quads.size())) {
    auto ring = quads[fid];
    auto n    = ring_corners(ring);
    for (auto c : range(n)) {
      auto a = (&ring.x)[c];
      auto b = (&ring.x)[(c + 1) % n];
      if (a == b) { topo.inconsistent++; continue; }
      used.insert(a);
      used.insert(b);
      edge_faces[{std::min(a, b), std::max(a, b)}].push_back({(int)fid, a});
    }
  }
  topo.vertices = (int)used.size();
  for (auto& entry : edge_faces) {
    auto& occurrences = entry.second;
    if (occurrences.size() == 1) {
      topo.boundary_edges++;
    } else if (occurrences.size() == 2) {
      if (occurrences[0].second == occurrences[1].second) topo.inconsistent++;
      face_adj[occurrences[0].first].push_back(occurrences[1].first);
      face_adj[occurrences[1].first].push_back(occurrences[0].first);
    } else {
      topo.non_manifold++;
    }
  }
  auto seen = std::vector<bool>(quads.size());
  for (auto fid : range(quads.size())) {
    if (seen[fid]) continue;
    topo.face_components++;
    auto stack = std::vector<int>{(int)fid};
    seen[fid]  = true;
    while (!stack.empty()) {
      auto current = stack.back();
      stack.pop_back();
      for (auto next : face_adj[current]) {
        if (!seen[next]) {
          seen[next] = true;
          stack.push_back(next);
        }
      }
    }
  }
  auto vertex_faces = std::map<int, std::vector<int>>{};
  for (auto fid : range(quads.size())) {
    auto ring = quads[fid];
    auto n    = ring_corners(ring);
    for (auto c : range(n)) vertex_faces[(&ring.x)[c]].push_back((int)fid);
  }
  for (auto& entry : vertex_faces) {
    auto faces       = std::set<int>{};
    for (auto face : entry.second) faces.insert(face);
    auto seen_faces  = std::set<int>{};
    auto fan_count   = 0;
    for (auto face : faces) {
      if (seen_faces.count(face)) continue;
      fan_count++;
      auto stack = std::vector<int>{face};
      seen_faces.insert(face);
      while (!stack.empty()) {
        auto current = stack.back();
        stack.pop_back();
        for (auto next : face_adj[current]) {
          if (faces.count(next) && !seen_faces.count(next)) {
            seen_faces.insert(next);
            stack.push_back(next);
          }
        }
      }
    }
    if (fan_count != 1) topo.multi_fan_vertices++;
  }
  // count boundary loops by following the unique outgoing boundary edge
  auto boundary_next = std::map<int, std::vector<int>>{};
  auto boundary_edges_list = std::vector<std::pair<int, int>>{};
  for (auto& entry : edge_faces) {
    if (entry.second.size() != 1) continue;
    auto fid  = entry.second[0].first;
    auto ring = quads[fid];
    auto n    = ring_corners(ring);
    for (auto c : range(n)) {
      auto a = (&ring.x)[c];
      auto b = (&ring.x)[(c + 1) % n];
      if (std::min(a, b) == entry.first.first &&
          std::max(a, b) == entry.first.second) {
        boundary_edges_list.push_back({a, b});
        boundary_next[a].push_back(b);
      }
    }
  }
  auto visited = std::set<std::pair<int, int>>{};
  for (auto& edge : boundary_edges_list) {
    if (visited.count(edge)) continue;
    topo.boundary_loops++;
    auto current = edge;
    auto guard   = 0;
    while (!visited.count(current) && guard++ < 100000) {
      visited.insert(current);
      auto it = boundary_next.find(current.second);
      if (it == boundary_next.end() || it->second.empty()) break;
      current = {current.second, it->second.front()};
    }
  }
  return topo;
}

// -----------------------------------------------------------------------------
// CAGE BUILDERS
// -----------------------------------------------------------------------------

// An open band of quads. `twisted` makes the last ring swap, which turns the
// band into a Moebius strip.
fvshape_data make_band(int count, bool twisted) {
  auto shape = fvshape_data{};
  for (auto idx : range(count)) {
    auto angle = 2 * pif * idx / count;
    for (auto side : {-1, 1}) {
      auto offset = 0.25f * side;
      shape.positions.push_back(
          {(2 + offset * std::cos(twisted ? angle / 2 : 0)) *
              std::cos(angle),
           (2 + offset * std::cos(twisted ? angle / 2 : 0)) *
              std::sin(angle),
           twisted ? offset * std::sin(angle / 2) : offset});
    }
  }
  for (auto idx : range(count)) {
    auto a = 2 * idx;
    auto b = a + 1;
    auto c = 2 * ((idx + 1) % count);
    auto d = c + 1;
    if (twisted && idx == count - 1) std::swap(c, d);
    shape.quadspos.push_back({a, b, d, c});
  }
  return shape;
}

// A strip that mixes triangles and quads, with a uv seam and an asymmetric
// out-of-plane shape so that the two sides are easy to tell apart.
fvshape_data make_mixed_asymmetric() {
  auto shape      = fvshape_data{};
  shape.positions = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0}, {0, 1, 0.1f},
      {1, 1, 0.3f}, {2, 1, 0.5f}, {3, 1, 0.7f}};
  // two quads and two triangles sharing their edges
  shape.quadspos      = {{0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 7}, {2, 7, 6, 6}};
  // texture topology: the middle edge is a uv seam (different uv indices)
  shape.quadstexcoord = {{0, 1, 2, 3}, {4, 5, 6, 7}, {8, 9, 10, 10},
      {11, 12, 13, 13}};
  shape.texcoords     = {{0.0f, 0.0f}, {0.25f, 0.0f}, {0.25f, 0.4f},
      {0.0f, 0.4f}, {0.3f, 0.0f}, {0.55f, 0.0f}, {0.55f, 0.4f}, {0.3f, 0.4f},
      {0.6f, 0.0f}, {0.8f, 0.0f}, {0.8f, 0.4f}, {0.6f, 0.0f}, {0.9f, 0.0f},
      {0.9f, 0.4f}};
  return shape;
}

// A single open quad.
fvshape_data make_disk() {
  auto shape      = fvshape_data{};
  shape.positions = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
  shape.quadspos  = {{0, 1, 2, 3}};
  return shape;
}

// -----------------------------------------------------------------------------
// FIXTURES
// -----------------------------------------------------------------------------

std::string scratch_dir(const std::string& name) {
  auto dir = std::filesystem::path(".task-build") / "check-cover" / name;
  std::filesystem::create_directories(dir);
  return (dir / "scene.json").string();
}

scene_data make_covered_scene(const fvshape_data& cage, bool catmullclark,
    int subdivisions, bool smooth) {
  auto scene = scene_data{};
  scene.shapes.resize(1);
  scene.shape_names = {"surface"};
  auto subdiv       = subdiv_data{};
  subdiv.shape      = 0;
  subdiv.quadspos   = cage.quadspos;
  subdiv.positions  = cage.positions;
  subdiv.orientation_cover = true;
  subdiv.catmullclark      = catmullclark;
  subdiv.subdivisions      = subdivisions;
  subdiv.smooth            = smooth;
  scene.subdivs            = {subdiv};
  scene.subdiv_names       = {"cage"};
  return scene;
}

// -----------------------------------------------------------------------------
// CASE: TOPOLOGY
// -----------------------------------------------------------------------------

int check_cover_topology() {
  // a plain band lifts to two shells: each source position has two lifts and
  // the shells are disconnected
  for (auto count : {5, 7}) {
    auto source = make_band(count, false);
    auto cover  = make_orientation_cover(source);
    auto topo   = inspect_topology(cover.quadspos);
    TASK_CHECK(topo.faces == 2 * (int)source.quadspos.size());
    TASK_CHECK(topo.non_manifold == 0);
    TASK_CHECK(topo.inconsistent == 0);
    TASK_CHECK(topo.multi_fan_vertices == 0);
    TASK_CHECK(topo.face_components == 2);
    TASK_CHECK(topo.boundary_edges == 4 * count);
    TASK_CHECK(topo.boundary_loops == 4);
    TASK_CHECK((int)cover.positions.size() == 2 * (int)source.positions.size());
    // every lifted edge belongs to exactly one or two faces: no boundary was
    // created inside the cover beyond the original border, and each source
    // position is lifted exactly twice
    auto per_index = std::map<std::array<int, 3>, int>{};
    for (auto& position : cover.positions) {
      auto key = std::array<int, 3>{(int)std::round(position.x * 1000),
          (int)std::round(position.y * 1000), (int)std::round(position.z * 1000)};
      per_index[key]++;
    }
    for (auto& entry : per_index) TASK_CHECK(entry.second == 2);
  }

  // a Moebius band lifts to a single connected band
  for (auto count : {5, 7}) {
    auto source = make_band(count, true);
    auto cover  = make_orientation_cover(source);
    auto topo   = inspect_topology(cover.quadspos);
    TASK_CHECK(topo.faces == 2 * (int)source.quadspos.size());
    TASK_CHECK(topo.non_manifold == 0);
    TASK_CHECK(topo.inconsistent == 0);
    TASK_CHECK(topo.multi_fan_vertices == 0);
    TASK_CHECK(topo.face_components == 1);
    TASK_CHECK(topo.boundary_edges == 4 * count);
    TASK_CHECK(topo.boundary_loops == 2);
    TASK_CHECK((int)cover.positions.size() == 2 * (int)source.positions.size());
  }

  // triangles and quads mixed, with local winding changes, still lift correctly
  {
    auto source = make_mixed_asymmetric();
    auto cover  = make_orientation_cover(source);
    auto topo   = inspect_topology(cover.quadspos);
    TASK_CHECK(topo.non_manifold == 0);
    TASK_CHECK(topo.inconsistent == 0);
    TASK_CHECK(topo.multi_fan_vertices == 0);
    TASK_CHECK(topo.face_components == 2);
    TASK_CHECK(topo.faces == 2 * (int)source.quadspos.size());
    // each source face produces a forward and a reversed copy, in order, and
    // triangles keep their repeated last corner on both sides
    for (auto fid : range(source.quadspos.size())) {
      auto src        = source.quadspos[fid];
      auto forward    = cover.quadspos[2 * fid];
      auto reversed   = cover.quadspos[2 * fid + 1];
      auto is_triangle = src.z == src.w;
      TASK_CHECK((forward.z == forward.w) == is_triangle);
      TASK_CHECK((reversed.z == reversed.w) == is_triangle);
      // the forward copy preserves the source winding, the reversed one is its
      // mirror; check the winding sign flips for the real corners
      auto src_normal = cross(source.positions[src.y] - source.positions[src.x],
          source.positions[src.z] - source.positions[src.x]);
      auto fwd_normal = cross(
          cover.positions[forward.y] - cover.positions[forward.x],
          cover.positions[forward.z] - cover.positions[forward.x]);
      auto rev_normal = cross(
          cover.positions[reversed.y] - cover.positions[reversed.x],
          cover.positions[reversed.z] - cover.positions[reversed.x]);
      TASK_CHECK(dot(src_normal, fwd_normal) > 0);
      TASK_CHECK(dot(src_normal, rev_normal) < 0);
      TASK_CHECK(dot(fwd_normal, rev_normal) < 0);
    }
    // two lifts per index, and coincident positions do not merge
    TASK_CHECK((int)cover.positions.size() == 2 * (int)source.positions.size());
  }

  // a closed orientable mesh lifts to two closed shells with no boundary
  {
    // tetrahedron with triangle faces
    auto source      = fvshape_data{};
    source.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    source.quadspos  = {{0, 2, 1, 1}, {0, 1, 3, 3}, {0, 3, 2, 2},
        {1, 2, 3, 3}};
    auto cover       = make_orientation_cover(source);
    auto topo        = inspect_topology(cover.quadspos);
    TASK_CHECK(topo.non_manifold == 0);
    TASK_CHECK(topo.inconsistent == 0);
    TASK_CHECK(topo.multi_fan_vertices == 0);
    TASK_CHECK(topo.face_components == 2);
    TASK_CHECK(topo.boundary_edges == 0);
    TASK_CHECK(topo.boundary_loops == 0);
    TASK_CHECK(topo.faces == 8);
    TASK_CHECK((int)cover.positions.size() == 8);
  }

  // mixed triangles and quads forming a closed prism also close up
  {
    auto source      = fvshape_data{};
    source.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1},
        {1, 0, 1}, {0, 1, 1}};
    source.quadspos  = {{0, 1, 2, 2}, {5, 4, 3, 3}, {0, 3, 4, 1},
        {1, 4, 5, 2}, {2, 5, 3, 0}};
    auto cover       = make_orientation_cover(source);
    auto topo        = inspect_topology(cover.quadspos);
    TASK_CHECK(topo.non_manifold == 0);
    TASK_CHECK(topo.inconsistent == 0);
    TASK_CHECK(topo.multi_fan_vertices == 0);
    TASK_CHECK(topo.face_components == 2);
    TASK_CHECK(topo.boundary_edges == 0);
    TASK_CHECK(topo.faces == 10);
    TASK_CHECK((int)cover.positions.size() == 12);
  }

  // multiple components with locally different winding lift independently:
  // an orientable part becomes two shells and a Moebius part one band
  {
    auto source = make_band(6, false);
    auto mobius = make_band(5, true);
    auto offset = (int)source.positions.size();
    for (auto& position : mobius.positions)
      source.positions.push_back(position + vec3f{10, 0, 0});
    for (auto& ring : mobius.quadspos) {
      source.quadspos.push_back({ring.x + offset, ring.y + offset,
          ring.z + offset, ring.w + offset});
    }
    // flip one face of the orientable part to test a local winding change
    source.quadspos[1] = {source.quadspos[1].x, source.quadspos[1].w,
        source.quadspos[1].z, source.quadspos[1].y};
    auto cover = make_orientation_cover(source);
    auto topo  = inspect_topology(cover.quadspos);
    TASK_CHECK(topo.non_manifold == 0);
    TASK_CHECK(topo.inconsistent == 0);
    TASK_CHECK(topo.multi_fan_vertices == 0);
    TASK_CHECK(topo.face_components == 3);
    TASK_CHECK((int)cover.positions.size() == 2 * (int)source.positions.size());
  }

  // winding swaps on an open band do not change the number of lifts
  {
    auto source = make_band(6, false);
    for (auto flip : range(source.quadspos.size())) {
      auto flipped = source;
      auto ring    = flipped.quadspos[flip];
      flipped.quadspos[flip] = {ring.x, ring.w, ring.z, ring.y};
      auto cover = make_orientation_cover(flipped);
      auto topo  = inspect_topology(cover.quadspos);
      TASK_CHECK(topo.face_components == 2);
      TASK_CHECK(topo.inconsistent == 0);
      TASK_CHECK(topo.multi_fan_vertices == 0);
      TASK_CHECK((int)cover.positions.size() == 2 * (int)source.positions.size());
    }
  }

  // unreferenced source positions are omitted from the cover
  {
    auto source      = make_disk();
    source.positions.push_back({5, 5, 5});  // never referenced
    auto cover       = make_orientation_cover(source);
    auto topo        = inspect_topology(cover.quadspos);
    TASK_CHECK(topo.vertices == 8);
    TASK_CHECK((int)cover.positions.size() == 8);
  }

  // a single disconnected quad still has two lifts per position
  {
    auto source = make_disk();
    auto cover  = make_orientation_cover(source);
    auto topo   = inspect_topology(cover.quadspos);
    TASK_CHECK(topo.faces == 2);
    TASK_CHECK(topo.face_components == 2);
    TASK_CHECK(topo.vertices == 8);
    TASK_CHECK(topo.boundary_edges == 8);
    TASK_CHECK(topo.boundary_loops == 2);
    TASK_CHECK(topo.multi_fan_vertices == 0);
  }

  // coincident but distinct source positions do not weld
  {
    auto source              = fvshape_data{};
    source.positions         = {{0, 0, 0}, {0, 0, 0}, {0, 0, 1}, {0, 0, 1}};
    source.quadspos          = {{0, 1, 3, 2}};
    // the quad repeats two positions by index, which is still a valid ring
    // only if they are distinct indices: here 0/1 and 2/3 coincide pairwise
    auto cover = make_orientation_cover(source);
    TASK_CHECK((int)cover.positions.size() == 8);
    TASK_CHECK(cover.quadspos.size() == 2);
  }

  // geometrically degenerate but combinatorially valid input is accepted: no
  // area threshold rejects collinear or coincident-position faces
  {
    auto source      = fvshape_data{};
    source.positions = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0}};
    source.quadspos  = {{0, 1, 2, 3}};  // all collinear, zero area
    auto cover       = make_orientation_cover(source);
    TASK_CHECK(cover.quadspos.size() == 2);
    TASK_CHECK(cover.positions.size() == 8);
    // positions may coincide as long as indices differ
    auto coincident      = fvshape_data{};
    coincident.positions = {{0, 0, 0}, {0, 0, 0}, {1, 0, 0}, {1, 0, 0}};
    coincident.quadspos  = {{0, 1, 3, 2}};
    auto coincident_cover = make_orientation_cover(coincident);
    TASK_CHECK(coincident_cover.quadspos.size() == 2);
    TASK_CHECK(coincident_cover.positions.size() == 8);
  }

  // empty cage yields an empty cover with no texture data
  {
    auto cover = make_orientation_cover(fvshape_data{});
    TASK_CHECK(cover.quadspos.empty());
    TASK_CHECK(cover.positions.empty());
    TASK_CHECK(cover.quadstexcoord.empty());
    TASK_CHECK(cover.texcoords.empty());
    TASK_CHECK(cover.normals.empty());
    TASK_CHECK(cover.quadsnorm.empty());
  }

  // the cover ignores input normals and returns empty normals
  {
    auto source       = make_disk();
    source.normals    = vector<vec3f>(source.positions.size(), {0, 0, 1});
    source.quadsnorm  = source.quadspos;
    auto cover        = make_orientation_cover(source);
    TASK_CHECK(cover.normals.empty());
    TASK_CHECK(cover.quadsnorm.empty());
  }

  // uv topologies are separate: matching indices on a shared edge connect,
  // matching values with different indices do not
  {
    auto source              = fvshape_data{};
    source.positions         = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {0, 1, 0},
        {1, 1, 0}, {2, 1, 0}};
    source.quadspos          = {{0, 1, 4, 3}, {1, 2, 5, 4}};
    auto base                = source;
    base.quadstexcoord       = {{0, 1, 2, 3}, {4, 5, 6, 7}};
    base.texcoords           = {{0, 0}, {0.5f, 0}, {0.5f, 1}, {0, 1}, {0.5f, 0},
        {1, 0}, {1, 1}, {0.5f, 1}};
    auto seamed              = make_orientation_cover(base);
    TASK_CHECK(!seamed.quadstexcoord.empty());
    TASK_CHECK(!seamed.texcoords.empty());
    TASK_CHECK((int)seamed.quadstexcoord.size() == 4);
    // a shared uv index on the edge connects the texture topology
    auto joined                = base;
    // the shared geometric edge is (1, 4); give both corners the same uv
    // index on each side: face0 corners (1,4) -> uv (1,2), face1 -> uv (1,2)
    joined.quadstexcoord       = {{0, 1, 2, 3}, {1, 5, 6, 2}};
    auto joined_cover          = make_orientation_cover(joined);
    TASK_CHECK((int)joined_cover.texcoords.size() <
               (int)seamed.texcoords.size());
    // equal uvs with distinct indices do not connect
    auto valued                = base;
    valued.texcoords[4]        = valued.texcoords[1];
    valued.texcoords[7]        = valued.texcoords[2];
    auto valued_cover          = make_orientation_cover(valued);
    TASK_CHECK((int)valued_cover.texcoords.size() ==
               (int)seamed.texcoords.size());
    // the geometric sides are not merged by the texture topology
    auto seam_topo = inspect_topology(seamed.quadspos);
    TASK_CHECK(seam_topo.face_components == 2);
    TASK_CHECK(seam_topo.non_manifold == 0);
    TASK_CHECK(seam_topo.inconsistent == 0);
  }

  // invalid inputs are rejected with invalid_argument
  {
    auto base = make_disk();
    auto expect_invalid = [](auto mutate) {
      try {
        auto source = make_disk();
        mutate(source);
        auto cover = make_orientation_cover(source);
        (void)cover;
        return false;
      } catch (const std::invalid_argument&) {
        return true;
      } catch (...) {
        return false;
      }
    };
    TASK_CHECK(expect_invalid([](fvshape_data& s) { s.quadspos[0].x = 99; }));
    TASK_CHECK(expect_invalid([](fvshape_data& s) { s.quadspos[0].x = -1; }));
    TASK_CHECK(expect_invalid([](fvshape_data& s) { s.quadspos[0].y = 0; }));
    TASK_CHECK(expect_invalid([](fvshape_data& s) { s.quadspos.push_back(s.quadspos[0]); }));
    TASK_CHECK(expect_invalid([](fvshape_data& s) {
      s.positions[0].x = std::numeric_limits<float>::quiet_NaN();
    }));
    TASK_CHECK(expect_invalid([](fvshape_data& s) {
      s.positions[0].x = std::numeric_limits<float>::infinity();
    }));
    TASK_CHECK(expect_invalid([](fvshape_data& s) {
      s.quadstexcoord       = {{0, 1, 2, 3}};
      s.texcoords           = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
      s.quadstexcoord[0].x  = 9;
    }));
    TASK_CHECK(expect_invalid([](fvshape_data& s) {
      s.quadstexcoord = {{0, 1, 2, 3}};
      s.texcoords     = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
      s.texcoords[0].x = std::numeric_limits<float>::quiet_NaN();
    }));
    TASK_CHECK(expect_invalid([](fvshape_data& s) {
      s.quadstexcoord = {{0, 1, 2, 3}};
      s.texcoords     = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
      s.quadstexcoord.resize(2);
    }));
    // three faces on one edge is non manifold
    TASK_CHECK(expect_invalid([](fvshape_data& s) {
      s.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}};
      s.quadspos  = {{0, 1, 2, 2}, {1, 0, 3, 3}, {0, 1, 4, 4}};
    }));
    // a triangle with an out of range repeated corner
    TASK_CHECK(expect_invalid([](fvshape_data& s) {
      s.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
      s.quadspos  = {{0, 1, 2, 2}};
      s.positions.pop_back();
    }));
    // a bowtie vertex with two disjoint face fans at one position
    TASK_CHECK(expect_invalid([](fvshape_data& s) {
      s.positions = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {-1, 0, 0}, {-1, -1, 0}};
      s.quadspos  = {{0, 1, 2, 2}, {0, 4, 3, 3}};
    }));
    // a texture face with the wrong number of corners
    TASK_CHECK(expect_invalid([](fvshape_data& s) {
      s.quadstexcoord = {{0, 1, 2, 3}};
      s.texcoords     = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
      s.quadspos      = {{0, 1, 2, 2}};
    }));
  }

  return 0;
}

// -----------------------------------------------------------------------------
// CASE: PIPELINE
// -----------------------------------------------------------------------------

// Count unique normals in a shape split by geometry classes. Used to verify
// that both sides got opposite oriented normals.
int count_distinct_normals(const vector<vec3f>& normals) {
  auto keys = std::set<std::array<int, 3>>{};
  for (auto& normal : normals) {
    keys.insert({(int)std::round(normal.x * 1000), (int)std::round(normal.y * 1000),
        (int)std::round(normal.z * 1000)});
  }
  return (int)keys.size();
}

int check_cover_pipeline() {
  auto cage = make_mixed_asymmetric();

  // zero level: both sides present, normals opposite, faces doubled
  {
    auto scene = make_covered_scene(cage, true, 0, true);
    tesselate_subdivs(scene);
    auto& shape = scene.shapes[0];
    TASK_CHECK(shape.quads.size() == 2 * cage.quadspos.size());
    TASK_CHECK(!shape.positions.empty());
    auto topo = inspect_topology(shape.quads);
    TASK_CHECK(topo.non_manifold == 0);
    TASK_CHECK(topo.inconsistent == 0);
    TASK_CHECK(topo.multi_fan_vertices == 0);
    TASK_CHECK(topo.face_components == 2);
    // smooth=true output has vertex normals and both sides face oppositely
    TASK_CHECK(!shape.normals.empty());
    auto sum = vec3f{0, 0, 0};
    for (auto& normal : shape.normals) sum += normal;
    TASK_CHECK(length(sum) < 1e-4f);
    TASK_CHECK(count_distinct_normals(shape.normals) > 2);
    // the source cage is unchanged
    TASK_CHECK(scene.subdivs[0].positions == cage.positions);
    TASK_CHECK(scene.subdivs[0].quadspos == cage.quadspos);
  }

  // follow with two subdivisions, linear and Catmull-Clark, both smooth
  for (auto catmullclark : {false, true}) {
    auto scene = make_covered_scene(cage, catmullclark, 2, true);
    tesselate_subdivs(scene);
    // the cover doubles the control faces, and subdivision is per face, so the
    // lifted result is exactly twice the plain subdivision of the same cage
    auto plain = make_covered_scene(cage, catmullclark, 2, true);
    plain.subdivs[0].orientation_cover = false;
    tesselate_subdivs(plain);
    TASK_CHECK(scene.shapes[0].quads.size() == 2 * plain.shapes[0].quads.size());
    auto& shape = scene.shapes[0];
    TASK_CHECK(!shape.normals.empty());
    auto topo = inspect_topology(shape.quads);
    TASK_CHECK(topo.non_manifold == 0);
    TASK_CHECK(topo.inconsistent == 0);
    TASK_CHECK(topo.multi_fan_vertices == 0);
    TASK_CHECK(topo.face_components == 2);
    // smooth normals point in opposite directions on both sides
    auto sum = vec3f{0, 0, 0};
    for (auto& normal : shape.normals) sum += normal;
    TASK_CHECK(length(sum) < 1e-3f);
    // repeat generation from the original cage is identical
    auto positions = shape.positions;
    auto normals   = shape.normals;
    tesselate_subdivs(scene);
    TASK_CHECK(scene.shapes[0].positions == positions);
    TASK_CHECK(scene.shapes[0].normals == normals);
  }

  // smooth=false does not emit vertex normals
  for (auto subdivisions : {0, 2}) {
    auto scene = make_covered_scene(cage, true, subdivisions, false);
    tesselate_subdivs(scene);
    TASK_CHECK(scene.shapes[0].normals.empty());
    TASK_CHECK(scene.shapes[0].positions.size() > 0);
    TASK_CHECK(scene.shapes[0].quads.size() > 0);
  }

  // displacement follows each side's own normal, so both sides move outwards
  // and the surface separates rather than staying coincident
  {
    auto scene     = make_covered_scene(cage, true, 0, true);
    auto& subdiv   = scene.subdivs[0];
    subdiv.quadstexcoord = cage.quadstexcoord;
    subdiv.texcoords     = cage.texcoords;
    subdiv.displacement  = 0.5f;
    subdiv.displacement_tex = 0;
    texture_data texture;
    texture.width   = 2;
    texture.height  = 2;
    texture.linear  = true;
    texture.pixelsf = {{0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
    scene.textures      = {texture};
    scene.texture_names = {"height"};
    // the reference shares the same uv topology but has no displacement, so
    // the split vertex sets match and only positions differ
    auto reference                    = scene;
    reference.subdivs[0].displacement = 0;
    tesselate_subdivs(scene);
    tesselate_subdivs(reference);
    auto& shape = scene.shapes[0];
    TASK_CHECK(!shape.positions.empty());
    TASK_CHECK(shape.positions.size() == reference.shapes[0].positions.size());
    auto moved    = 0;
    auto along    = 0;
    auto opposite = 0;
    for (auto idx : range(shape.positions.size())) {
      auto delta = shape.positions[idx] - reference.shapes[0].positions[idx];
      if (length(delta) <= 1e-5f) continue;
      moved++;
      // each vertex moves along its own normal
      auto alignment = dot(normalize(delta), reference.shapes[0].normals[idx]);
      if (alignment > 0.95f) along++;
    }
    TASK_CHECK(moved > 0);
    TASK_CHECK(along == moved);
    // the two sides at a coincident undisplaced position move in opposite
    // directions, which is what makes a double-sided sheet open up
    for (auto first : range(reference.shapes[0].positions.size())) {
      auto& origin = reference.shapes[0].positions[first];
      auto  delta0 = shape.positions[first] - origin;
      if (length(delta0) <= 1e-5f) continue;
      for (auto second : range(first + 1,
               reference.shapes[0].positions.size())) {
        if (length(reference.shapes[0].positions[second] - origin) > 1e-5f)
          continue;
        auto delta1 = shape.positions[second] - origin;
        if (length(delta1) <= 1e-5f) continue;
        if (dot(normalize(delta0), normalize(delta1)) < -0.95f) opposite++;
      }
    }
    TASK_CHECK(opposite > 0);
    // uv seams do not tear: the split shape's geometry-only classes are
    // welded along the shared source edge even when the uv differs
    auto topo = inspect_topology(shape.quads);
    TASK_CHECK(topo.non_manifold == 0);
  }

  // displacement with two subdivisions, both subdivision modes, matches the
  // zero level behaviour
  for (auto catmullclark : {false, true}) {
    auto scene                  = make_covered_scene(cage, catmullclark, 2, true);
    auto& subdiv                = scene.subdivs[0];
    subdiv.quadstexcoord        = cage.quadstexcoord;
    subdiv.texcoords            = cage.texcoords;
    subdiv.displacement         = 0.4f;
    subdiv.displacement_tex     = 0;
    texture_data texture;
    texture.width   = 2;
    texture.height  = 2;
    texture.linear  = true;
    texture.pixelsf = {{0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
    scene.textures      = {texture};
    scene.texture_names = {"height"};
    auto reference                    = scene;
    reference.subdivs[0].displacement = 0;
    tesselate_subdivs(scene);
    tesselate_subdivs(reference);
    TASK_CHECK(scene.shapes[0].positions.size() ==
               reference.shapes[0].positions.size());
    auto max_delta = 0.0f;
    auto along     = 0;
    for (auto idx : range(scene.shapes[0].positions.size())) {
      auto delta = scene.shapes[0].positions[idx] -
                   reference.shapes[0].positions[idx];
      max_delta = std::max(max_delta, length(delta));
      if (length(delta) <= 1e-5f) continue;
      auto alignment = dot(normalize(delta), reference.shapes[0].normals[idx]);
      if (alignment > 0.95f) along++;
    }
    TASK_CHECK(max_delta > 1e-4f);
    TASK_CHECK(along > 0);
    // displacement happens before the uv split, so the split preserves the
    // number of geometry classes even with a texture seam
    TASK_CHECK(scene.shapes[0].normals.size() ==
               scene.shapes[0].positions.size());
    auto topo = inspect_topology(scene.shapes[0].quads);
    TASK_CHECK(topo.non_manifold == 0);
    TASK_CHECK(topo.inconsistent == 0);
    // the uv seam splits the shape into separate vertices, but the geometry
    // must not crack: coincident split vertices share the same position
    auto split_positions = scene.shapes[0].positions;
    auto welded_positions = vector<vec3f>{};
    for (auto& position : split_positions) welded_positions.push_back(position);
    std::sort(welded_positions.begin(), welded_positions.end(),
        [](const vec3f& a, const vec3f& b) {
          return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
        });
    auto duplicates = 0;
    for (auto idx = (size_t)1; idx < welded_positions.size(); idx++) {
      if (length(welded_positions[idx] - welded_positions[idx - 1]) < 1e-6f)
        duplicates++;
    }
    TASK_CHECK(duplicates > 0);
  }

  // flipping the sign of the displacement mirrors the motion, which proves
  // the two sides use their own normals rather than a shared one
  {
    auto build = [&](float displacement) {
      auto scene              = make_covered_scene(cage, true, 1, true);
      auto& subdiv            = scene.subdivs[0];
      subdiv.quadstexcoord    = cage.quadstexcoord;
      subdiv.texcoords        = cage.texcoords;
      subdiv.displacement     = displacement;
      subdiv.displacement_tex = 0;
      texture_data texture;
      texture.width   = 2;
      texture.height  = 2;
      texture.linear  = true;
      texture.pixelsf = {{0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f},
          {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
      scene.textures      = {texture};
      scene.texture_names = {"height"};
      tesselate_subdivs(scene);
      return scene.shapes[0].positions;
    };
    auto reference = build(0.0f);
    auto positive  = build(0.4f);
    auto negative  = build(-0.4f);
    TASK_CHECK(positive.size() == reference.size());
    TASK_CHECK(negative.size() == reference.size());
    auto moved_positive = 0;
    auto moved_negative = 0;
    auto mirrored       = 0;
    for (auto idx : range(reference.size())) {
      auto delta_positive = positive[idx] - reference[idx];
      auto delta_negative = negative[idx] - reference[idx];
      if (length(delta_positive) > 1e-5f) moved_positive++;
      if (length(delta_negative) > 1e-5f) moved_negative++;
      if (length(delta_positive) > 1e-5f && length(delta_negative) > 1e-5f) {
        if (length(delta_positive + delta_negative) <
            1e-4f * std::max(1.0f, length(delta_positive)))
          mirrored++;
      }
    }
    TASK_CHECK(moved_positive > 0 && moved_negative > 0);
    // every displaced vertex that moves in both builds must move oppositely
    TASK_CHECK(mirrored > 0);
    TASK_CHECK(mirrored == moved_positive);
  }

  // displacement with smooth=false still moves along oriented normals but
  // leaves the final shape without vertex normals
  {
    auto scene         = make_covered_scene(cage, true, 1, false);
    auto& subdiv       = scene.subdivs[0];
    subdiv.quadstexcoord    = cage.quadstexcoord;
    subdiv.texcoords        = cage.texcoords;
    subdiv.displacement     = 0.5f;
    subdiv.displacement_tex = 0;
    texture_data texture;
    texture.width   = 2;
    texture.height  = 2;
    texture.linear  = true;
    texture.pixelsf = {{0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
    scene.textures      = {texture};
    scene.texture_names = {"height"};
    auto reference                    = scene;
    reference.subdivs[0].displacement = 0;
    tesselate_subdivs(scene);
    tesselate_subdivs(reference);
    TASK_CHECK(scene.shapes[0].normals.empty());
    TASK_CHECK(scene.shapes[0].positions.size() ==
               reference.shapes[0].positions.size());
    auto moved = 0;
    for (auto idx : range(scene.shapes[0].positions.size())) {
      if (length(scene.shapes[0].positions[idx] -
                 reference.shapes[0].positions[idx]) > 1e-5f)
        moved++;
    }
    TASK_CHECK(moved > 0);
  }

  // modifying the source cage and regenerating produces the expected new surface
  {
    auto scene = make_covered_scene(cage, true, 0, true);
    tesselate_subdivs(scene);
    auto first = scene.shapes[0].positions;
    scene.subdivs[0].positions[1] += vec3f{0, 0, 0.5f};
    tesselate_subdivs(scene);
    auto changed = 0;
    for (auto idx : range(first.size())) {
      if (length(scene.shapes[0].positions[idx] - first[idx]) > 1e-5f) changed++;
    }
    TASK_CHECK(changed > 0);
  }

  // no displacement texture leaves the source data untouched and the surface
  // unchanged when the flag is off
  {
    auto scene       = make_covered_scene(cage, true, 0, true);
    auto scene_copy  = scene;
    scene.subdivs[0].orientation_cover = false;
    tesselate_subdivs(scene);
    TASK_CHECK(scene.shapes[0].quads.size() == cage.quadspos.size());
    TASK_CHECK(scene.subdivs[0].positions == scene_copy.subdivs[0].positions);
  }

  // destination shape bounds and duplicate destinations are rejected, and
  // a failing cage leaves the whole scene (shapes and subdivs) untouched
  {
    auto scene                  = scene_data{};
    scene.shapes.resize(2);
    scene.shape_names           = {"one", "two"};
    auto subdiv0                = subdiv_data{};
    subdiv0.shape               = 0;
    subdiv0.positions           = cage.positions;
    subdiv0.quadspos            = cage.quadspos;
    subdiv0.orientation_cover   = true;
    auto subdiv1                = subdiv0;
    subdiv1.shape               = 1;
    subdiv1.quadspos[0].x       = 99;
    scene.subdivs               = {subdiv0, subdiv1};
    scene.subdiv_names          = {"good", "bad"};
    auto before                 = scene;
    auto threw                  = false;
    try {
      tesselate_subdivs(scene);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(scene.shapes[0].positions.empty());
    TASK_CHECK(scene.shapes[1].positions.empty());
    TASK_CHECK(scene.subdivs[0].positions == before.subdivs[0].positions);
    TASK_CHECK(scene.subdivs[1].quadspos == before.subdivs[1].quadspos);
  }

  // a texture failure in a later subdiv also leaves the scene untouched
  {
    auto scene      = scene_data{};
    scene.shapes.resize(2);
    scene.shape_names = {"one", "two"};
    auto good       = subdiv_data{};
    good.shape      = 0;
    good.positions  = cage.positions;
    good.quadspos   = cage.quadspos;
    good.orientation_cover = true;
    auto bad        = subdiv_data{};
    bad.shape       = 1;
    bad.positions   = cage.positions;
    bad.quadspos    = cage.quadspos;
    bad.orientation_cover = true;
    bad.displacement      = 1;
    bad.displacement_tex  = 0;  // texture exists but texcoords are missing
    texture_data texture;
    texture.width  = 1;
    texture.height = 1;
    texture.linear = true;
    texture.pixelsf = {{1, 1, 1, 1}};
    scene.textures      = {texture};
    scene.texture_names = {"height"};
    scene.subdivs       = {good, bad};
    scene.subdiv_names  = {"good", "bad"};
    auto threw = false;
    try {
      tesselate_subdivs(scene);
    } catch (const std::exception&) {
      threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(scene.shapes[0].positions.empty());
    TASK_CHECK(scene.shapes[1].positions.empty());
    TASK_CHECK(scene.subdivs[0].positions == cage.positions);
    TASK_CHECK(scene.subdivs[1].positions == cage.positions);
  }

  // duplicate destination shapes are rejected
  {
    auto scene      = scene_data{};
    scene.shapes.resize(1);
    auto subdiv     = subdiv_data{};
    subdiv.shape    = 0;
    subdiv.positions = cage.positions;
    subdiv.quadspos  = cage.quadspos;
    subdiv.orientation_cover = true;
    scene.subdivs = {subdiv, subdiv};
    auto threw = false;
    try {
      tesselate_subdivs(scene);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(scene.shapes[0].positions.empty());
  }

  // destination out of range is rejected
  {
    auto scene       = scene_data{};
    scene.shapes.resize(1);
    auto subdiv      = subdiv_data{};
    subdiv.shape     = 3;
    subdiv.positions = cage.positions;
    subdiv.quadspos  = cage.quadspos;
    subdiv.orientation_cover = true;
    scene.subdivs = {subdiv};
    auto threw = false;
    try {
      tesselate_subdivs(scene);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(scene.shapes[0].positions.empty());
  }

  // instance references and shape indices survive generation
  {
    auto scene = make_covered_scene(cage, true, 1, true);
    scene.shapes.resize(2);
    scene.instances = {{identity3x4f, 0, invalidid},
        {translation_frame(vec3f{2, 0, 0}), 0, invalidid}};
    tesselate_subdivs(scene);
    TASK_CHECK(scene.instances[0].shape == 0);
    TASK_CHECK(scene.instances[1].shape == 0);
    TASK_CHECK(scene.shapes.size() == 2);
    TASK_CHECK(scene.shapes[1].positions.empty());
  }

  return 0;
}

// -----------------------------------------------------------------------------
// CASE: ASSETS
// -----------------------------------------------------------------------------

// Compare generated shapes by HDR precision, since the roundtrip saves textures
// as float images.
bool shapes_close(const shape_data& first, const shape_data& second) {
  if (first.quads.size() != second.quads.size()) return false;
  if (first.positions.size() != second.positions.size()) return false;
  for (auto idx : range(first.quads.size())) {
    if (!(first.quads[idx] == second.quads[idx])) return false;
  }
  for (auto idx : range(first.positions.size())) {
    if (length(first.positions[idx] - second.positions[idx]) > 1e-4f)
      return false;
  }
  return true;
}

bool normals_close(const shape_data& first, const shape_data& second) {
  if (first.normals.size() != second.normals.size()) return false;
  for (auto idx : range(first.normals.size())) {
    if (length(first.normals[idx] - second.normals[idx]) > 1e-4f) return false;
  }
  return true;
}

int check_cover_assets() {
  auto cage = make_mixed_asymmetric();

  // build a scene with two instances, a texture and the cover option
  auto scene                  = make_covered_scene(cage, true, 2, true);
  auto& subdiv                = scene.subdivs[0];
  subdiv.smooth               = true;
  subdiv.quadstexcoord        = cage.quadstexcoord;
  subdiv.texcoords            = cage.texcoords;
  subdiv.displacement         = 0.25f;
  subdiv.displacement_tex     = 0;
  texture_data texture;
  texture.width   = 3;
  texture.height  = 3;
  texture.linear  = true;
  texture.pixelsf = {{0.1f, 0.2f, 0.3f, 1.0f}, {0.4f, 0.5f, 0.6f, 1.0f},
      {0.7f, 0.8f, 0.9f, 1.0f}, {0.2f, 0.3f, 0.4f, 1.0f}, {0.5f, 0.6f, 0.7f, 1.0f},
      {0.8f, 0.9f, 0.1f, 1.0f}, {0.3f, 0.4f, 0.5f, 1.0f}, {0.6f, 0.7f, 0.8f, 1.0f},
      {0.9f, 0.1f, 0.2f, 1.0f}};
  scene.textures      = {texture};
  scene.texture_names = {"height"};
  scene.materials.resize(1);
  scene.material_names = {"material"};
  scene.instances = {{identity3x4f, 0, 0},
      {translation_frame(vec3f{4, 0, 0}), 0, 0}};
  scene.instance_names = {"left", "right"};

  // generate first so the destination shape exists and is saved alongside the
  // control cage, mirroring the real pipeline
  tesselate_subdivs(scene);
  auto original_surface = scene.shapes[0];

  // save and reload through the JSON path
  auto filename = scratch_dir("assets");
  auto error    = std::string{};
  TASK_CHECK(make_scene_directories(filename, scene, error));
  TASK_CHECK(save_scene(filename, scene, error, true));
  auto loaded = scene_data{};
  TASK_CHECK(load_scene(filename, loaded, error, true));
  TASK_CHECK(loaded.subdivs.size() == 1);
  TASK_CHECK(loaded.instances.size() == 2);
  TASK_CHECK(loaded.instances[0].shape == loaded.instances[1].shape);
  TASK_CHECK(loaded.subdivs[0].orientation_cover);
  TASK_CHECK(loaded.subdivs[0].displacement_tex == 0);
  TASK_CHECK(loaded.subdivs[0].positions == scene.subdivs[0].positions);
  TASK_CHECK(loaded.subdivs[0].quadspos == scene.subdivs[0].quadspos);
  // the OBJ face-varying roundtrip may renumber uv indices without changing
  // the uv values, so compare values rather than indices
  TASK_CHECK(loaded.subdivs[0].texcoords.size() ==
             scene.subdivs[0].texcoords.size());
  for (auto idx : range(scene.subdivs[0].quadstexcoord.size())) {
    auto& first  = scene.subdivs[0].quadstexcoord[idx];
    auto& second = loaded.subdivs[0].quadstexcoord[idx];
    for (auto c : range(4)) {
      auto& uv0 = scene.subdivs[0].texcoords[(&first.x)[c]];
      auto& uv1 = loaded.subdivs[0].texcoords[(&second.x)[c]];
      TASK_CHECK(length(uv0 - uv1) < 1e-6f);
    }
  }
  TASK_CHECK(loaded.textures.size() == 1 &&
             loaded.textures[0].pixelsf.size() ==
                 scene.textures[0].pixelsf.size());
  // texture values survive the HDR roundtrip at HDR precision
  auto texture_error = 0.0f;
  for (auto idx : range(scene.textures[0].pixelsf.size())) {
    texture_error = std::max(texture_error,
        length(vec3f{loaded.textures[0].pixelsf[idx].x -
                         scene.textures[0].pixelsf[idx].x,
            loaded.textures[0].pixelsf[idx].y -
                scene.textures[0].pixelsf[idx].y,
            loaded.textures[0].pixelsf[idx].z -
                scene.textures[0].pixelsf[idx].z}));
  }
  TASK_CHECK(texture_error < 1e-2f);

  // regenerate on both sides. The texture goes through the HDR file format, so
  // the reference generation uses the reloaded texture values to compare only
  // the regenerated surface, not the texture encoding error.
  auto reference = scene;
  reference.textures[0] = loaded.textures[0];
  tesselate_subdivs(reference);
  tesselate_subdivs(loaded);
  TASK_CHECK(shapes_close(reference.shapes[0], loaded.shapes[0]));
  TASK_CHECK(normals_close(reference.shapes[0], loaded.shapes[0]));
  TASK_CHECK(loaded.shapes[0].quads.size() > 0);

  // the bvh and eval_normal read the generated result
  auto bvh = make_scene_bvh(loaded, false, true);
  auto eval_ok = 0;
  auto hits    = 0;
  for (auto idx : range(loaded.shapes[0].positions.size())) {
    auto origin = loaded.shapes[0].positions[idx] + vec3f{0, 0, 0.05f};
    auto ray    = ray3f{origin, {0, 0, -1}};
    auto hit    = intersect_scene_bvh(bvh, loaded, ray, true);
    if (hit.hit) {
      hits++;
      auto normal = eval_normal(loaded, loaded.instances[0], hit.element, hit.uv);
      eval_ok += isfinite(normal) ? 1 : 0;
    }
  }
  TASK_CHECK(hits > 0);
  TASK_CHECK(eval_ok == hits);

  // cast rays straight down through the generated sheet and check that
  // eval_normal returns the interpolated vertex normal of the hit face
  auto checked  = 0;
  auto& shape   = loaded.shapes[0];
  auto instance = loaded.instances[0];
  for (auto fid : range(shape.quads.size())) {
    auto uv        = vec2f{0.5f, 0.5f};
    auto position  = eval_position(loaded, instance, fid, uv);
    auto origin    = position + vec3f{0, 0, 1};
    auto ray       = ray3f{origin, {0, 0, -1}};
    auto hit       = intersect_scene_bvh(bvh, loaded, ray, true);
    if (!hit.hit || hit.instance != 0) continue;
    auto hit_normal = eval_normal(loaded, instance, hit.element, hit.uv);
    auto q          = shape.quads[hit.element];
    // interpolate the stored vertex normals the same way eval_normal does
    auto expected = normalize(interpolate_quad(shape.normals[q.x],
        shape.normals[q.y], shape.normals[q.z], shape.normals[q.w], hit.uv));
    TASK_CHECK(isfinite(hit_normal));
    TASK_CHECK(dot(hit_normal, expected) > 0.9f);
    checked++;
  }
  TASK_CHECK(checked > 0);

  // unknown texture names in a 4.2 asset are rejected
  {
    auto bad_filename = scratch_dir("bad-tex");
    auto bad_error    = std::string{};
    TASK_CHECK(make_scene_directories(bad_filename, scene, bad_error));
    TASK_CHECK(save_scene(bad_filename, scene, bad_error, true));
    // rewrite the json to use a stale texture name
    auto json_path  = bad_filename;
    auto text       = std::string{};
    TASK_CHECK(load_text(json_path, text, bad_error));
    auto stale_text = text;
    auto key        = std::string{"\"displacement_tex\": 0"};
    auto pos        = stale_text.find(key);
    TASK_CHECK(pos != std::string::npos);
    stale_text.replace(pos, key.size(), "\"displacement_tex\": \"missing\"");
    TASK_CHECK(save_text(json_path, stale_text, bad_error));
    auto bad_scene = scene_data{};
    TASK_CHECK(!load_scene(json_path, bad_scene, bad_error, true));
    TASK_CHECK(!bad_error.empty());
  }

  // old writers that used texture names still load
  {
    auto old_filename = scratch_dir("old-name");
    auto old_error    = std::string{};
    TASK_CHECK(make_scene_directories(old_filename, scene, old_error));
    TASK_CHECK(save_scene(old_filename, scene, old_error, true));
    auto json_path = old_filename;
    auto text      = std::string{};
    TASK_CHECK(load_text(json_path, text, old_error));
    auto legacy = text;
    // store the texture reference by name, as a 4.0/4.1 writer would
    auto key = std::string{"\"displacement_tex\": 0"};
    auto pos = legacy.find(key);
    TASK_CHECK(pos != std::string::npos);
    legacy.replace(pos, key.size(), "\"displacement_tex\": \"height\"");
    TASK_CHECK(save_text(json_path, legacy, old_error));
    auto old_scene = scene_data{};
    TASK_CHECK(load_scene(json_path, old_scene, old_error, true));
    TASK_CHECK(old_scene.subdivs[0].displacement_tex == 0);
    // use the file-loaded texture values so the comparison isolates the name
    // to index resolution rather than the HDR encoding
    auto legacy_reference        = old_scene;
    legacy_reference.textures[0] = old_scene.textures[0];
    tesselate_subdivs(legacy_reference);
    TASK_CHECK(shapes_close(reference.shapes[0], legacy_reference.shapes[0]));
  }

  // the option defaults to false and an old scene without it behaves as before
  {
    auto plain_filename = scratch_dir("plain");
    auto plain_error    = std::string{};
    auto plain_scene    = scene;
    plain_scene.subdivs[0].orientation_cover = false;
    TASK_CHECK(make_scene_directories(plain_filename, plain_scene, plain_error));
    TASK_CHECK(save_scene(plain_filename, plain_scene, plain_error, true));
    auto plain_text = std::string{};
    TASK_CHECK(load_text(plain_filename, plain_text, plain_error));
    TASK_CHECK(plain_text.find("orientation_cover") == std::string::npos);
    auto reloaded    = scene_data{};
    TASK_CHECK(load_scene(plain_filename, reloaded, plain_error, true));
    TASK_CHECK(!reloaded.subdivs[0].orientation_cover);
    tesselate_subdivs(reloaded);
    // without the cover the output matches the plain cage subdivided with the
    // same options, since triangles become quads during subdivision
    auto plain_reference = plain_scene;
    tesselate_subdivs(plain_reference);
    TASK_CHECK(reloaded.shapes[0].quads.size() ==
               plain_reference.shapes[0].quads.size());
    TASK_CHECK(reloaded.shapes[0].positions.size() ==
               plain_reference.shapes[0].positions.size());
  }

  // OBJ cage roundtrip preserves the control cage, then regenerates the same
  // surface as the original scene
  {
    auto obj_filename = scratch_dir("obj") + ".obj";
    auto obj_error    = std::string{};
    auto cage_fv      = fvshape_data{};
    cage_fv.quadspos      = cage.quadspos;
    cage_fv.positions     = cage.positions;
    cage_fv.quadstexcoord = cage.quadstexcoord;
    cage_fv.texcoords     = cage.texcoords;
    TASK_CHECK(save_fvshape(obj_filename, cage_fv, obj_error, true));
    auto loaded_fv = fvshape_data{};
    TASK_CHECK(load_fvshape(obj_filename, loaded_fv, obj_error, true));
    TASK_CHECK(loaded_fv.quadspos == cage_fv.quadspos);
    TASK_CHECK(loaded_fv.quadstexcoord == cage_fv.quadstexcoord);
    // regenerate from the two control cages and compare the generated
    // surfaces: one built from the original in-memory cage, one from the cage
    // loaded back through OBJ
    auto from_original = make_covered_scene(cage, true, 2, true);
    auto from_obj      = make_covered_scene(cage, true, 2, true);
    from_original.textures      = scene.textures;
    from_original.texture_names = scene.texture_names;
    from_original.subdivs[0].displacement     = 0.25f;
    from_original.subdivs[0].displacement_tex  = 0;
    from_original.subdivs[0].quadstexcoord     = cage_fv.quadstexcoord;
    from_original.subdivs[0].texcoords         = cage_fv.texcoords;
    from_obj.textures      = scene.textures;
    from_obj.texture_names = scene.texture_names;
    from_obj.subdivs[0].displacement     = 0.25f;
    from_obj.subdivs[0].displacement_tex  = 0;
    from_obj.subdivs[0].quadstexcoord     = loaded_fv.quadstexcoord;
    from_obj.subdivs[0].texcoords         = loaded_fv.texcoords;
    tesselate_subdivs(from_original);
    tesselate_subdivs(from_obj);
    TASK_CHECK(shapes_close(from_original.shapes[0], from_obj.shapes[0]));
    TASK_CHECK(normals_close(from_original.shapes[0], from_obj.shapes[0]));
  }

  // second generation after changing the cage reflects the change
  {
    auto changed = scene;
    tesselate_subdivs(changed);
    auto first = changed.shapes[0].positions;
    changed.subdivs[0].positions[2] += vec3f{0, 0, 0.4f};
    tesselate_subdivs(changed);
    auto moved = 0;
    for (auto idx : range(first.size())) {
      if (length(changed.shapes[0].positions[idx] - first[idx]) > 1e-5f) moved++;
    }
    TASK_CHECK(moved > 0);
  }

  return 0;
}

}  // namespace

// -----------------------------------------------------------------------------
// CASE REGISTRATION
// -----------------------------------------------------------------------------

extern const task_case task_cases[] = {
    {"topology", "cover topology", check_cover_topology},
    {"pipeline", "cover pipeline", check_cover_pipeline},
    {"assets", "cover assets", check_cover_assets},
};
extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
