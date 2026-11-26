#include "task_support/check.h"

#include "libs/yocto/yocto_bvh.h"
#include "libs/yocto/yocto_scene.h"
#include "libs/yocto/yocto_sceneio.h"
#include "libs/yocto/yocto_shape.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace yocto;

namespace {

struct TestDsu {
  std::vector<int> parent;

  explicit TestDsu(int n) : parent(n) {
    for (auto i = 0; i < n; ++i) parent[i] = i;
  }

  int find(int x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x         = parent[x];
    }
    return x;
  }

  void unite(int a, int b) {
    a = find(a);
    b = find(b);
    if (a != b) parent[b] = a;
  }
};

struct EdgeRecord {
  int face;
  int corner;
  int start;
  int end;
};

struct TopologyInfo {
  bool valid              = false;
  int  vertex_count       = 0;
  int  face_count         = 0;
  int  edge_count         = 0;
  int  boundary_edges     = 0;
  int  components         = 0;
  bool one_fan_per_vertex = false;
};

TopologyInfo inspect_cover(const std::vector<vec4i>& quads, int vertex_count) {
  auto info         = TopologyInfo{};
  info.vertex_count = vertex_count;
  info.face_count   = (int)quads.size();

  auto edges = std::map<std::pair<int, int>, std::vector<EdgeRecord>>{};
  auto arity = std::vector<int>(quads.size(), 4);
  for (auto f = 0; f < (int)quads.size(); ++f) {
    auto& q = quads[f];
    if (q.x < 0 || q.x >= vertex_count || q.y < 0 || q.y >= vertex_count ||
        q.z < 0 || q.z >= vertex_count || q.w < 0 || q.w >= vertex_count)
      return info;
    auto n = q.z == q.w ? 3 : 4;
    arity[f] = n;
    for (auto c = 0; c < n; ++c) {
      auto a = (&q.x)[c];
      auto b = (&q.x)[(c + 1) % n];
      if (a == b) return info;
      auto key = std::minmax(a, b);
      edges[{key.first, key.second}].push_back({f, c, a, b});
    }
  }

  info.edge_count = (int)edges.size();
  for (auto& [key, records] : edges) {
    if (records.size() > 2) return info;
    if (records.size() == 1) ++info.boundary_edges;
  }

  auto dsu = TestDsu((int)quads.size() * 4);
  for (auto f = 0; f < (int)quads.size(); ++f) {
    if (arity[f] == 3) dsu.unite(f * 4 + 2, f * 4 + 3);
  }

  auto corner_of = [&](const EdgeRecord& record, int vertex, int n) {
    return record.start == vertex ? record.corner
                                  : (record.corner + 1) % n;
  };
  for (auto& [key, records] : edges) {
    if (records.size() != 2) continue;
    auto& r0 = records[0];
    auto& r1 = records[1];
    for (auto vertex : {key.first, key.second}) {
      auto c0 = corner_of(r0, vertex, arity[r0.face]);
      auto c1 = corner_of(r1, vertex, arity[r1.face]);
      dsu.unite(r0.face * 4 + c0, r1.face * 4 + c1);
    }
  }

  info.one_fan_per_vertex = true;
  auto fan_root           = std::vector<int>(vertex_count, -1);
  for (auto f = 0; f < (int)quads.size(); ++f) {
    for (auto c = 0; c < arity[f]; ++c) {
      auto v    = (&quads[f].x)[c];
      auto root = dsu.find(f * 4 + c);
      if (fan_root[v] == -1) {
        fan_root[v] = root;
      } else if (fan_root[v] != root) {
        info.one_fan_per_vertex = false;
      }
    }
  }

  auto adjacency = std::vector<std::vector<int>>(quads.size());
  for (auto& [key, records] : edges) {
    if (records.size() != 2) continue;
    auto a = records[0].face;
    auto b = records[1].face;
    adjacency[a].push_back(b);
    adjacency[b].push_back(a);
  }
  auto seen = std::vector<char>(quads.size(), false);
  for (auto f = 0; f < (int)quads.size(); ++f) {
    if (seen[f]) continue;
    ++info.components;
    auto queue = std::vector<int>{f};
    seen[f]    = true;
    for (auto i = (size_t)0; i < queue.size(); ++i) {
      auto current = queue[i];
      for (auto next : adjacency[current]) {
        if (!seen[next]) {
          seen[next] = true;
          queue.push_back(next);
        }
      }
    }
  }

  info.valid = info.one_fan_per_vertex;
  return info;
}

fvshape_data make_band(int segments, bool twisted, bool mixed) {
  auto cage      = fvshape_data{};
  cage.positions = {};
  for (auto i = 0; i < segments; ++i) {
    auto angle = 2.0f * 3.14159265358979323846f * (float)i / (float)segments;
    for (auto side : {-1.0f, 1.0f}) {
      auto width = 0.25f * side;
      auto twist = twisted ? angle / 2.0f : 0.0f;
      auto radius = 2.0f + width * std::cos(twist);
      cage.positions.push_back({radius * std::cos(angle),
          radius * std::sin(angle),
          twisted ? width * std::sin(twist) : width});
    }
  }

  for (auto i = 0; i < segments; ++i) {
    auto a = 2 * i;
    auto b = a + 1;
    auto j = (i + 1) % segments;
    auto c = 2 * j;
    auto d = c + 1;
    if (twisted && i == segments - 1) std::swap(c, d);

    if (mixed && i % 2 == 0) {
      cage.quadspos.push_back({a, c, d, b});
    } else {
      cage.quadspos.push_back({a, c, d, d});
      cage.quadspos.push_back({a, d, b, b});
    }
  }
  return cage;
}

bool throws_invalid_argument(const std::function<void()>& callback) {
  try {
    callback();
  } catch (const std::invalid_argument&) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

bool same_position_occurrences(
    const fvshape_data& source, const fvshape_data& cover) {
  for (auto& p : source.positions) {
    auto count = 0;
    for (auto& q : cover.positions) {
      if (p == q) ++count;
    }
    if (count != 2) return false;
  }
  return true;
}

scene_data make_displacement_scene(int levels, bool catmullclark,
    float displacement, bool orientation_cover, bool smooth = true) {
  auto scene = scene_data{};

  auto texture        = texture_data{};
  texture.width       = 2;
  texture.height      = 1;
  texture.linear      = true;
  texture.pixelsf     = {
      {0.2f, 0.2f, 0.2f, 0.2f}, {0.6f, 0.6f, 0.6f, 0.6f}};
  scene.textures      = {texture};
  scene.texture_names = {"height"};

  scene.shapes.resize(1);
  scene.shape_names = {"surface"};

  auto cage             = subdiv_data{};
  cage.shape            = 0;
  cage.smooth           = smooth;
  cage.orientation_cover = orientation_cover;
  cage.subdivisions     = levels;
  cage.catmullclark     = catmullclark;
  cage.positions        = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {0, 1, 0},
      {1, 1, 0}, {2, 1, 0}};
  cage.quadspos         = {{0, 1, 4, 3}, {1, 2, 5, 4}};
  cage.quadsnorm        = cage.quadspos;
  cage.normals          = std::vector<vec3f>(6, {0, 0, 1});
  cage.quadstexcoord    = {{0, 1, 2, 3}, {4, 5, 6, 7}};
  cage.texcoords        = {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0.5f, 0},
      {0.5f, 0}, {0.5f, 0}, {0.5f, 0}};
  cage.displacement     = displacement;
  cage.displacement_tex = 0;
  scene.subdivs         = {cage};
  scene.subdiv_names    = {"cage"};

  scene.materials.resize(1);
  scene.material_names = {"material"};
  scene.instances       = {
      {identity3x4f, 0, 0}, {translation_frame({3, 0, 0}), 0, 0}};
  scene.instance_names = {"left", "right"};
  return scene;
}

bool near_value(float a, float b, float eps = 1e-5f) {
  return std::abs(a - b) <= eps;
}

}  // namespace

static int topology_bands() {
  constexpr int n = 7;

  auto annulus = make_band(n, false, false);
  auto annulus_cover = make_orientation_cover(annulus);
  auto annulus_info = inspect_cover(
      annulus_cover.quadspos, (int)annulus_cover.positions.size());
  TASK_CHECK(annulus_info.valid);
  TASK_CHECK(annulus_info.components == 2);
  TASK_CHECK(annulus_info.boundary_edges == 4 * n);
  TASK_CHECK(annulus_info.edge_count == 8 * n);
  TASK_CHECK(annulus_info.one_fan_per_vertex);
  TASK_CHECK((int)annulus_cover.positions.size() == 2 * (int)annulus.positions.size());
  TASK_CHECK(same_position_occurrences(annulus, annulus_cover));
  TASK_CHECK(annulus_cover.normals.empty());
  TASK_CHECK(annulus_cover.quadsnorm.empty());

  auto mobius = make_band(n, true, false);
  auto mobius_cover = make_orientation_cover(mobius);
  auto mobius_info = inspect_cover(
      mobius_cover.quadspos, (int)mobius_cover.positions.size());
  TASK_CHECK(mobius_info.valid);
  TASK_CHECK(mobius_info.components == 1);
  TASK_CHECK(mobius_info.boundary_edges == 4 * n);
  TASK_CHECK(mobius_info.edge_count == 8 * n);
  TASK_CHECK(mobius_info.one_fan_per_vertex);
  TASK_CHECK((int)mobius_cover.positions.size() == 2 * (int)mobius.positions.size());
  TASK_CHECK(same_position_occurrences(mobius, mobius_cover));

  // Numbered source positions with identical coordinates are not welded.
  auto duplicated = fvshape_data{};
  duplicated.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 0},
      {1, 0, 0}, {0, 1, 0}};
  duplicated.quadspos = {{0, 1, 2, 2}, {3, 4, 5, 5}};
  auto duplicated_cover = make_orientation_cover(duplicated);
  auto duplicated_info  = inspect_cover(
      duplicated_cover.quadspos, (int)duplicated_cover.positions.size());
  TASK_CHECK(duplicated_info.valid);
  TASK_CHECK(duplicated_info.vertex_count == 12);
  TASK_CHECK(duplicated_info.components == 4);
  TASK_CHECK(duplicated_info.one_fan_per_vertex);
  auto zero_occurrences = 0;
  for (auto& p : duplicated_cover.positions)
    if (p == vec3f{0, 0, 0}) ++zero_occurrences;
  TASK_CHECK(zero_occurrences == 4);
  return 0;
}

static int topology_mixed() {
  constexpr int n = 8;
  for (auto twisted : {false, true}) {
    auto cage  = make_band(n, twisted, true);
    auto cover = make_orientation_cover(cage);
    auto info  = inspect_cover(cover.quadspos, (int)cover.positions.size());
    TASK_CHECK(info.valid);
    TASK_CHECK(info.one_fan_per_vertex);
    TASK_CHECK(info.boundary_edges == 2 * 2 * n);
    TASK_CHECK(info.components == (twisted ? 1 : 2));
    TASK_CHECK((int)cover.quadspos.size() ==
               2 * (int)cage.quadspos.size());
    TASK_CHECK((int)cover.positions.size() == 2 * (int)cage.positions.size());

    // Every generated triangle still uses z == w.
    for (auto& q : cover.quadspos) {
      if (q.z == q.w) {
        TASK_CHECK(q.x != q.y && q.y != q.z && q.x != q.z);
      } else {
        TASK_CHECK(q.x != q.y && q.x != q.z && q.x != q.w &&
                   q.y != q.z && q.y != q.w && q.z != q.w);
      }
    }
  }

  // One face can have locally reversed winding; edge direction pairing still
  // has to produce two orientable shells.
  auto flipped = fvshape_data{};
  flipped.positions = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {0, 1, 0},
      {1, 1, 0}, {2, 1, 0}};
  flipped.quadspos = {{0, 1, 4, 3}, {1, 4, 5, 2}};
  auto flipped_cover = make_orientation_cover(flipped);
  auto flipped_info = inspect_cover(
      flipped_cover.quadspos, (int)flipped_cover.positions.size());
  TASK_CHECK(flipped_info.valid);
  TASK_CHECK(flipped_info.components == 2);
  TASK_CHECK(flipped_info.boundary_edges == 12);
  TASK_CHECK(flipped_info.one_fan_per_vertex);
  return 0;
}

static int topology_invalid() {
  TASK_CHECK(throws_invalid_argument([] {
    fvshape_data cage;
    cage.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    cage.quadspos  = {{0, 1, 9, 9}};
    (void)make_orientation_cover(cage);
  }));

  TASK_CHECK(throws_invalid_argument([] {
    fvshape_data cage;
    cage.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    cage.quadspos  = {{0, 1, 1, 2}};
    (void)make_orientation_cover(cage);
  }));

  TASK_CHECK(throws_invalid_argument([] {
    fvshape_data cage;
    cage.positions = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    cage.quadspos  = {{0, 1, 2, 3}, {0, 1, 2, 3}};
    (void)make_orientation_cover(cage);
  }));

  TASK_CHECK(throws_invalid_argument([] {
    fvshape_data cage;
    cage.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {2, 0, 0}};
    cage.quadspos  = {{0, 1, 2, 2}, {0, 3, 1, 1}, {0, 1, 4, 4}};
    (void)make_orientation_cover(cage);
  }));

  TASK_CHECK(throws_invalid_argument([] {
    fvshape_data cage;
    cage.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {2, 1, 0}, {2, 2, 0}};
    cage.quadspos  = {{0, 1, 2, 2}, {0, 3, 4, 4}};
    (void)make_orientation_cover(cage);
  }));

  TASK_CHECK(throws_invalid_argument([] {
    fvshape_data cage;
    cage.positions = {{std::nanf(""), 0, 0}, {1, 0, 0}, {0, 1, 0}};
    cage.quadspos  = {{0, 1, 2, 2}};
    (void)make_orientation_cover(cage);
  }));

  TASK_CHECK(throws_invalid_argument([] {
    fvshape_data cage;
    cage.positions     = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    cage.quadspos      = {{0, 1, 2, 2}};
    cage.quadstexcoord = {{0, 1, 2, 2}};
    cage.texcoords     = {{0, 0}, {1, 0}};
    (void)make_orientation_cover(cage);
  }));

  TASK_CHECK(throws_invalid_argument([] {
    fvshape_data cage;
    cage.positions     = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    cage.quadspos      = {{0, 1, 2, 2}};
    cage.quadstexcoord = {{0, 1, 2, 3}, {0, 0, 0, 0}};
    cage.texcoords     = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    (void)make_orientation_cover(cage);
  }));

  TASK_CHECK(throws_invalid_argument([] {
    fvshape_data cage;
    cage.positions     = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    cage.quadspos      = {{0, 1, 2, 2}};
    cage.quadstexcoord = {{0, 1, 2, 3}};
    cage.texcoords     = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    (void)make_orientation_cover(cage);
  }));

  // Degenerate geometry is combinatorially legal and must not be area-rejected.
  fvshape_data degenerate;
  degenerate.positions = {{0, 0, 0}, {0, 0, 0}, {1, 0, 0}, {1, 1, 0}};
  degenerate.quadspos  = {{0, 1, 2, 3}};
  auto degenerate_cover = make_orientation_cover(degenerate);
  TASK_CHECK(degenerate_cover.quadspos.size() == 2);
  TASK_CHECK(degenerate_cover.positions.size() == 8);
  return 0;
}

static int topology_uv() {
  auto cage = fvshape_data{};
  cage.positions = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {0, 1, 0}, {1, 1, 0},
      {2, 1, 0}};
  cage.quadspos = {{0, 1, 4, 3}, {1, 2, 5, 4}};

  // Shared geometric edge 1--4 has matching UV indices at both endpoints.
  cage.texcoords = {{0, 0}, {1, 0}, {1, 1}, {0, 1}, {2, 0}, {2, 1}};
  cage.quadstexcoord = {{0, 1, 2, 3}, {1, 4, 5, 2}};
  auto matching = make_orientation_cover(cage);
  TASK_CHECK(matching.quadstexcoord.size() == 4);
  TASK_CHECK(matching.quadstexcoord[0].y == matching.quadstexcoord[2].x);
  TASK_CHECK(matching.quadstexcoord[0].z == matching.quadstexcoord[2].w);
  TASK_CHECK(matching.quadstexcoord[1].z == matching.quadstexcoord[3].y);
  TASK_CHECK(matching.quadstexcoord[1].w == matching.quadstexcoord[3].x);

  // Different UV indices stay split even when their numeric values coincide.
  auto seam = cage;
  seam.texcoords = {{0, 0}, {0.4f, 0}, {0.4f, 1}, {0, 1}, {0.4f, 0},
      {1, 0}, {1, 1}, {0.4f, 1}};
  seam.quadstexcoord = {{0, 1, 2, 3}, {4, 5, 6, 7}};
  auto seam_cover = make_orientation_cover(seam);
  TASK_CHECK(seam_cover.quadstexcoord[0].y != seam_cover.quadstexcoord[2].x);
  TASK_CHECK(seam_cover.quadstexcoord[0].z != seam_cover.quadstexcoord[2].w);
  TASK_CHECK(seam_cover.texcoords.size() == 16);

  // A matching UV index at one endpoint connects that corner independently.
  auto partial = cage;
  partial.texcoords = {{0, 0}, {1, 0}, {1, 1}, {0, 1}, {2, 0}, {2, 1},
      {0.7f, 1}};
  partial.quadstexcoord = {{0, 1, 2, 3}, {1, 4, 5, 6}};
  auto partial_cover = make_orientation_cover(partial);
  TASK_CHECK(partial_cover.quadstexcoord[0].y ==
             partial_cover.quadstexcoord[2].x);
  TASK_CHECK(partial_cover.quadstexcoord[0].z !=
             partial_cover.quadstexcoord[2].w);
  return 0;
}

static int pipeline_displacement_matrix() {
  for (auto levels : {0, 2}) {
    for (auto catmullclark : {false, true}) {
      for (auto sign : {1.0f, -1.0f}) {
        auto scene = make_displacement_scene(
            levels, catmullclark, sign, true, true);
        tesselate_subdivs(scene);
        auto& shape = scene.shapes[0];

        auto expected_quads = 4;  // two source quads times two cover shells
        for (auto i = 0; i < levels; ++i) expected_quads *= 4;
        TASK_CHECK((int)shape.quads.size() == expected_quads);
        TASK_CHECK(shape.normals.size() == shape.positions.size());
        TASK_CHECK(!shape.texcoords.empty());

        auto positive_normal_z = 0.0f;
        auto negative_normal_z = 0.0f;
        auto positive_count    = 0;
        auto negative_count    = 0;
        auto positive_min      = 1e30f;
        auto positive_max      = -1e30f;
        auto negative_min      = 1e30f;
        auto negative_max      = -1e30f;
        for (auto i = 0; i < (int)shape.positions.size(); ++i) {
          auto& p = shape.positions[i];
          if (!near_value(p.x, 1.0f, 1e-4f) ||
              !near_value(p.y, 0.0f, 1e-4f))
            continue;
          TASK_CHECK(i < (int)shape.normals.size());
          auto nz = shape.normals[i].z;
          if (nz > 0.2f) {
            positive_normal_z += p.z;
            ++positive_count;
            positive_min = std::min(positive_min, p.z);
            positive_max = std::max(positive_max, p.z);
          } else if (nz < -0.2f) {
            negative_normal_z += p.z;
            ++negative_count;
            negative_min = std::min(negative_min, p.z);
            negative_max = std::max(negative_max, p.z);
          }
        }
        TASK_CHECK(positive_count >= 2);  // UV seam duplicates
        TASK_CHECK(negative_count >= 2);
        positive_normal_z /= (float)positive_count;
        negative_normal_z /= (float)negative_count;
        TASK_CHECK(near_value(positive_normal_z, 0.4f * sign, 1e-5f));
        TASK_CHECK(near_value(negative_normal_z, -0.4f * sign, 1e-5f));
        TASK_CHECK(positive_max - positive_min < 1e-5f);
        TASK_CHECK(negative_max - negative_min < 1e-5f);

        for (auto& n : shape.normals)
          TASK_CHECK(std::isfinite(n.x) && std::isfinite(n.y) &&
                     std::isfinite(n.z));
      }
    }
  }

  // smooth=false produces no final vertex normals, even with displacement.
  auto flat = make_displacement_scene(0, true, 1.0f, true, false);
  tesselate_subdivs(flat);
  TASK_CHECK(flat.shapes[0].normals.empty());
  return 0;
}

static int pipeline_bvh_normals() {
  auto scene = make_displacement_scene(0, true, 1.0f, true, true);
  tesselate_subdivs(scene);
  TASK_CHECK(scene.instances[0].shape == 0);
  TASK_CHECK(scene.instances[1].shape == 0);

  auto bvh = make_scene_bvh(scene, false, true);
  auto top_hit = intersect_scene_bvh(bvh, scene,
      ray3f{{0.5f, 0.5f, 2.0f}, {0.0f, 0.0f, -1.0f}});
  TASK_CHECK(top_hit.hit);
  TASK_CHECK(top_hit.instance == 0);
  auto top_normal = eval_normal(
      scene, scene.instances[top_hit.instance], top_hit.element, top_hit.uv);
  TASK_CHECK(top_normal.z > 0.9f);

  auto bottom_hit = intersect_scene_bvh(bvh, scene,
      ray3f{{0.5f, 0.5f, -2.0f}, {0.0f, 0.0f, 1.0f}});
  TASK_CHECK(bottom_hit.hit);
  TASK_CHECK(bottom_hit.instance == 0);
  auto bottom_normal = eval_normal(scene,
      scene.instances[bottom_hit.instance], bottom_hit.element, bottom_hit.uv);
  TASK_CHECK(bottom_normal.z < -0.9f);

  auto translated_hit = intersect_scene_bvh(bvh, scene,
      ray3f{{3.5f, 0.5f, 2.0f}, {0.0f, 0.0f, -1.0f}});
  TASK_CHECK(translated_hit.hit);
  TASK_CHECK(translated_hit.instance == 1);
  return 0;
}

static int pipeline_repeat_and_atomic() {
  auto scene = make_displacement_scene(0, true, 1.0f, true, true);
  tesselate_subdivs(scene);
  auto first_positions = scene.shapes[0].positions;
  auto first_quads     = scene.shapes[0].quads;
  tesselate_subdivs(scene);
  TASK_CHECK(scene.shapes[0].positions == first_positions);
  TASK_CHECK(scene.shapes[0].quads == first_quads);

  scene.subdivs[0].positions[0].z += 0.3f;
  tesselate_subdivs(scene);
  TASK_CHECK(!(scene.shapes[0].positions == first_positions));

  // A later invalid cage must not publish an earlier generated destination.
  auto atomic = scene_data{};
  atomic.shapes.resize(2);
  atomic.shapes[0].positions = {{9, 9, 9}};
  atomic.shapes[0].points    = vector<int>{0};
  atomic.shapes[1].positions = {{-9, -9, -9}};
  atomic.shapes[1].points    = vector<int>{0};
  atomic.subdivs.resize(2);
  atomic.subdivs[0].shape = 0;
  atomic.subdivs[0].orientation_cover = true;
  atomic.subdivs[0].positions = {
      {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
  atomic.subdivs[0].quadspos = {{0, 1, 2, 3}};
  atomic.subdivs[1].shape = 1;
  atomic.subdivs[1].orientation_cover = true;
  atomic.subdivs[1].positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  atomic.subdivs[1].quadspos  = {{0, 1, 7, 7}};
  auto shapes_before          = atomic.shapes;
  auto subdivs_before         = atomic.subdivs;
  TASK_CHECK(throws_invalid_argument([&] { tesselate_subdivs(atomic); }));
  TASK_CHECK(atomic.shapes[0].positions == shapes_before[0].positions);
  TASK_CHECK(atomic.shapes[0].points == shapes_before[0].points);
  TASK_CHECK(atomic.shapes[1].positions == shapes_before[1].positions);
  TASK_CHECK(atomic.subdivs[0].positions == subdivs_before[0].positions);
  TASK_CHECK(atomic.subdivs[1].quadspos == subdivs_before[1].quadspos);

  auto out_of_bounds = scene_data{};
  out_of_bounds.shapes.resize(1);
  out_of_bounds.subdivs.resize(1);
  out_of_bounds.subdivs[0].shape = 8;
  TASK_CHECK(throws_invalid_argument([&] { tesselate_subdivs(out_of_bounds); }));

  auto duplicate = scene_data{};
  duplicate.shapes.resize(1);
  duplicate.subdivs.resize(2);
  duplicate.subdivs[0].shape = 0;
  duplicate.subdivs[1].shape = 0;
  duplicate.subdivs[0].positions = {
      {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
  duplicate.subdivs[0].quadspos = {{0, 1, 2, 3}};
  duplicate.subdivs[1].positions = duplicate.subdivs[0].positions;
  duplicate.subdivs[1].quadspos  = duplicate.subdivs[0].quadspos;
  TASK_CHECK(throws_invalid_argument([&] { tesselate_subdivs(duplicate); }));
  return 0;
}

static int assets_roundtrip() {
  auto scene = make_displacement_scene(0, true, 1.0f, true, true);
  // HDR stores RGB only and reloads alpha as one. Match that before generating
  // the reference so the roundtrip comparison exercises HDR precision only.
  for (auto& pixel : scene.textures[0].pixelsf) pixel.w = 1.0f;
  tesselate_subdivs(scene);
  auto original_positions = scene.shapes[0].positions;

  auto directory = std::filesystem::path(".task-build/test-cover-roundtrip");
  std::filesystem::create_directories(directory);
  auto filename = (directory / "scene.json").string();
  auto error    = std::string{};
  TASK_CHECK(make_scene_directories(filename, scene, error));
  TASK_CHECK(save_scene(filename, scene, error, true));

  auto json = nlohmann::json::parse(std::ifstream(filename));
  TASK_CHECK(json["subdivs"][0]["orientation_cover"].get<bool>());
  TASK_CHECK(json["subdivs"][0]["displacement_tex"].is_number_integer());
  TASK_CHECK(json["subdivs"][0]["displacement_tex"].get<int>() == 0);

  auto loaded = scene_data{};
  TASK_CHECK(load_scene(filename, loaded, error, true));
  TASK_CHECK(loaded.subdivs.size() == 1);
  TASK_CHECK(loaded.subdivs[0].orientation_cover);
  TASK_CHECK(loaded.subdivs[0].displacement_tex == 0);
  TASK_CHECK(loaded.instances.size() == 2);
  TASK_CHECK(loaded.instances[0].shape == loaded.instances[1].shape);
  TASK_CHECK(loaded.subdivs[0].positions == scene.subdivs[0].positions);
  TASK_CHECK(loaded.subdivs[0].quadspos == scene.subdivs[0].quadspos);

  TASK_CHECK(loaded.textures.size() == 1);
  TASK_CHECK(!loaded.textures[0].pixelsf.empty());
  TASK_CHECK(std::abs(loaded.textures[0].pixelsf[0][0] - 0.2f) < 0.003f);
  TASK_CHECK(std::abs(loaded.textures[0].pixelsf[1][0] - 0.6f) < 0.003f);

  tesselate_subdivs(loaded);
  TASK_CHECK(loaded.shapes[0].positions.size() == original_positions.size());
  auto sort_positions = [](std::vector<vec3f> positions) {
    std::sort(positions.begin(), positions.end(),
        [](const vec3f& a, const vec3f& b) {
          if (a.x != b.x) return a.x < b.x;
          if (a.y != b.y) return a.y < b.y;
          return a.z < b.z;
        });
    return positions;
  };
  auto sorted_original = sort_positions(original_positions);
  auto sorted_loaded   = sort_positions(loaded.shapes[0].positions);
  auto max_difference = 0.0f;
  for (auto i = 0; i < (int)sorted_original.size(); ++i) {
    max_difference = std::max(max_difference,
        length(sorted_loaded[i] - sorted_original[i]));
  }
  TASK_CHECK(max_difference < 0.01f);

  // Old 4.2 writers referred to textures by name.
  json["subdivs"][0]["displacement_tex"] = "height";
  auto named_filename = (directory / "named-reference.json").string();
  {
    auto output = std::ofstream(named_filename);
    output << json.dump(2);
  }
  auto named = scene_data{};
  TASK_CHECK(load_scene(named_filename, named, error, true));
  TASK_CHECK(named.subdivs[0].displacement_tex == 0);

  json["subdivs"][0]["displacement_tex"] = "unknown-height";
  auto unknown_filename = (directory / "unknown-reference.json").string();
  {
    auto output = std::ofstream(unknown_filename);
    output << json.dump(2);
  }
  auto unknown = scene_data{};
  TASK_CHECK(!load_scene(unknown_filename, unknown, error, true));
  return 0;
}

static int assets_legacy_defaults() {
  // Absence of the new key in old inputs must leave the option disabled.
  auto directory = std::filesystem::path(".task-build/test-cover-legacy");
  std::filesystem::create_directories(directory / "subdivs");
  std::filesystem::create_directories(directory / "shapes");
  auto write_quad_obj = [](const std::filesystem::path& path) {
    auto output = std::ofstream(path);
    output << "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n";
    output << "f 1 2 3 4\n";
  };
  write_quad_obj(directory / "subdivs" / "legacy.obj");
  write_quad_obj(directory / "shapes" / "legacy.obj");
  write_quad_obj(directory / "shapes" / "surface.obj");

  auto json = nlohmann::json{};
  json["asset"]["version"]   = "4.1";
  json["subdivs"]["legacy"]  = {
      {"shape", "surface"}, {"datafile", "legacy.obj"}};
  json["shapes"]["surface"] = {{"datafile", "legacy.obj"}};
  auto filename = (directory / "scene-41.json").string();
  {
    auto output = std::ofstream(filename);
    output << json.dump(2);
  }
  auto scene = scene_data{};
  auto error = std::string{};
  TASK_CHECK(load_scene(filename, scene, error, true));
  TASK_CHECK(scene.subdivs.size() == 1);
  TASK_CHECK(!scene.subdivs[0].orientation_cover);
  TASK_CHECK(scene.subdivs[0].quadspos.size() == 1);
  tesselate_subdivs(scene);
  TASK_CHECK(scene.shapes.size() == 1);
  TASK_CHECK(scene.shapes[0].quads.size() == 1);

  auto json40 = nlohmann::json{};
  json40["asset"]["version"]  = "4.0";
  json40["subdivs"]["legacy"] = {{"shape", "surface"}};
  auto filename40 = (directory / "scene-40.json").string();
  {
    auto output = std::ofstream(filename40);
    output << json40.dump(2);
  }
  auto scene40 = scene_data{};
  TASK_CHECK(load_scene(filename40, scene40, error, true));
  TASK_CHECK(scene40.subdivs.size() == 1);
  TASK_CHECK(!scene40.subdivs[0].orientation_cover);
  tesselate_subdivs(scene40);
  TASK_CHECK(scene40.shapes.size() == 1);
  TASK_CHECK(scene40.shapes[0].quads.size() == 1);
  return 0;
}

extern const task_case task_cases[] = {
    {"topology", "bands", topology_bands},
    {"topology", "mixed-tri-quad", topology_mixed},
    {"topology", "invalid-inputs", topology_invalid},
    {"topology", "uv-topology", topology_uv},
    {"pipeline", "displacement-matrix", pipeline_displacement_matrix},
    {"pipeline", "bvh-normals", pipeline_bvh_normals},
    {"pipeline", "repeat-mutate-atomic", pipeline_repeat_and_atomic},
    {"assets", "json-obj-roundtrip", assets_roundtrip},
    {"assets", "legacy-defaults", assets_legacy_defaults},
};

extern const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
