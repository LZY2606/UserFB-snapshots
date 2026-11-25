// Example: build one common partition for three transverse solids and write
// every occupancy region as an OBJ group.  All groups use one shared vertex
// pool, so adjacent groups reference the same interface vertices and faces.
//
// Usage:
//   surface_partition_example [output.obj]

#include <fstream>
#include <iostream>
#include <string>

#include "manifold/manifold.h"
#include "manifold/surface_partition.h"

using namespace manifold;

int main(int argc, char** argv) {
  const std::string outPath =
      argc > 1 ? argv[1] : "surface_partition_regions.obj";

  Manifold a = Manifold::Cube({6.0, 5.0, 5.0}, true).Translate({-1, 0, 0});
  Manifold b = Manifold::Cube({5.0, 6.0, 5.5}, true).Translate({0, -1, 0});
  Manifold c = Manifold::Cube({4.5, 5.5, 6.0}, true).Translate({0, 0, -1});

  SurfacePartition partition({a, b, c});
  if (!partition.ok()) {
    std::cerr << "SurfacePartition construction failed: "
              << partition.errorMessage() << "\n";
    return 1;
  }

  std::ofstream out(outPath);
  if (!out) {
    std::cerr << "Could not open output OBJ: " << outPath << "\n";
    return 2;
  }

  out << "# SurfacePartition example\n";
  out << "o common_partition\n";
  out << std::fixed;
  out.precision(12);
  for (const vec3& v : partition.Vertices())
    out << "v " << v.x << ' ' << v.y << ' ' << v.z << '\n';

  size_t faceCount = 0;
  for (uint32_t mask = 1; mask < 8; ++mask) {
    out << "g region_mask_" << mask << "\n";
    for (const SurfacePartition::Triangle& t : partition.Triangles()) {
      if (t.minusMask != mask && t.plusMask != mask) continue;
      std::array<uint64_t, 3> id = t.vertices;
      // Stored winding is source-outward.  The positive side sees the
      // opposite outward normal when its occupancy mask is selected.
      if (t.plusMask == mask) std::swap(id[1], id[2]);
      out << "f " << (id[0] + 1) << ' ' << (id[1] + 1) << ' '
          << (id[2] + 1) << '\n';
      ++faceCount;
    }
  }

  std::cout << "Wrote " << partition.Vertices().size() << " shared vertices, "
            << partition.Triangles().size() << " unique interface triangles, "
            << faceCount << " grouped region faces to " << outPath << "\n";
  return 0;
}
