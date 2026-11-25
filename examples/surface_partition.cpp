// Example: build a common SurfacePartition from three transverse solids and
// write every occupancy region as a named OBJ group.  All groups reference the
// same global vertex table; adjacent regions repeat the same three indices in
// opposite winding order.
//
// Usage:
//   surface_partition_example [output.obj]
// The task runner invokes it with .task-build/partition.obj.

#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "manifold/manifold.h"
#include "manifold/surface_partition.h"

using namespace manifold;

namespace {

std::vector<Manifold> ExampleSolids() {
  return {
      Manifold::Cube({2.0, 2.0, 2.0}, true),
      Manifold::Cube({1.55, 1.85, 1.65}, true)
          .Rotate(13.0, 29.0, 37.0)
          .Translate({0.16, 0.06, 0.11}),
      Manifold::Cube({1.75, 1.45, 1.95}, true)
          .Rotate(-17.0, 23.0, -11.0)
          .Translate({-0.11, 0.13, 0.07}),
  };
}

}  // namespace

int main(int argc, char** argv) {
  const std::string outPath =
      argc >= 2 ? argv[1] : "surface_partition_regions.obj";

  std::vector<Manifold> solids = ExampleSolids();
  SurfacePartition partition(solids);
  if (!partition.Valid()) {
    std::cerr << "SurfacePartition construction failed: "
              << static_cast<int>(partition.GetStatus()) << "\n";
    return 1;
  }

  std::ofstream out(outPath);
  if (!out) {
    std::cerr << "Could not open output OBJ: " << outPath << "\n";
    return 2;
  }

  out << "# SurfacePartition example\n";
  out << "# masks use bit i for being inside operand i; mask 0 is exterior\n";
  out << "# " << partition.Vertices().size() << " shared geometric vertices, "
      << partition.Faces().size() << " interface triangles\n";
  out << std::setprecision(17);

  for (const vec3& v : partition.Vertices()) {
    out << "v " << v.x << ' ' << v.y << ' ' << v.z << '\n';
  }

  for (uint32_t mask = 1; mask < 8; ++mask) {
    Manifold region;
    if (!partition.TryExtract(mask, region)) {
      std::cerr << "invalid mask " << mask << "\n";
      return 3;
    }
    out << "o region_mask_" << mask << '\n';
    out << "g mask_" << mask << "\n";
    out << "usemtl occupancy_" << mask << '\n';

    size_t written = 0;
    for (const SurfacePartition::Face& f : partition.Faces()) {
      if (mask == f.minusMask) {
        out << "f " << f.vertices[0] + 1 << ' ' << f.vertices[1] + 1 << ' '
            << f.vertices[2] + 1 << '\n';
        ++written;
      } else if (mask == f.plusMask) {
        // The plus side sees the same stored triangle from its other side.
        out << "f " << f.vertices[0] + 1 << ' ' << f.vertices[2] + 1 << ' '
            << f.vertices[1] + 1 << '\n';
        ++written;
      }
    }
    out << "# mask " << mask << " triangles=" << written
        << " manifold_triangles=" << region.GetMeshGL64().NumTri()
        << " volume=" << region.Volume() << "\n";
  }

  if (!out) {
    std::cerr << "failed while writing " << outPath << "\n";
    return 4;
  }
  std::cout << "wrote " << outPath << "\n";
  return 0;
}
