// Headless demonstration of a deformable tetrahedral smoke volume rendered
// together with the original surface scene.  Builds its own cage and scene,
// writes the TGA path given on the command line, and prints the linear color
// and pose counters for two independent poses of the same resource.

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

#include "RayTracer.h"
#include "Ray.h"
#include "Color.h"
#include "TetVolume.hpp"
#include "TetTransport.hpp"

using namespace tetvol;

namespace {

TetCell makeCell(int a, int b, int c, int d,
                 const std::vector<Vector>& verts, const RGB& sigma,
                 const RGB& source) {
   TetCell cell;
   cell.v = {a, b, c, d};
   Vector e1 = verts[b] - verts[a];
   Vector e2 = verts[c] - verts[a];
   Vector e3 = verts[d] - verts[a];
   if (e1.dot(e2.cross(e3)) <= 0.0) {
      cell.v = {a, c, b, d};
   }
   cell.sigma = sigma;
   cell.source = source;
   return cell;
}

// A chain of triangular prisms following a curved centerline; each prism is
// split into three positively oriented tetrahedra.  Reports the number of
// prism sections so callers can address a middle cross-section.
std::shared_ptr<TetVolume> buildSmokeCage(int& sectionCount) {
   const int sections = 5;
   sectionCount = sections;
   std::vector<Vector> verts;
   for (int i = 0; i <= sections; ++i) {
      double z = static_cast<double>(i);
      Vector center(0.2 * std::sin(z), 0.0, z + 0.5);
      verts.push_back(center + Vector(0, -0.9, 0));
      verts.push_back(center + Vector(0.7794, 0.45, 0));
      verts.push_back(center + Vector(-0.7794, 0.45, 0));
   }
   std::vector<TetCell> cells;
   const RGB sigma(0.35, 0.25, 0.15);
   const RGB source(0.6, 0.75, 0.9);
   for (int i = 0; i < sections; ++i) {
      int a = 3 * i, b = a + 1, c = a + 2;
      int A = a + 3, B = a + 4, C = a + 5;
      cells.push_back(makeCell(a, b, c, A, verts, sigma, source));
      cells.push_back(makeCell(b, c, A, B, verts, sigma, source));
      cells.push_back(makeCell(c, A, B, C, verts, sigma, source));
   }
   std::vector<double> density(verts.size(), 1.0);
   return std::make_shared<TetVolume>(verts, cells, density);
}

void printCounters(const char* tag, const TetPose& pose) {
   std::cout << tag << " generation=" << pose.generation()
             << " totalBuilds=" << pose.totalBuilds()
             << " inverseRecomputations=" << pose.inverseRecomputations()
             << " boundsRefits=" << pose.boundsRefits() << '\n';
}

} // namespace

int main(int argc, char** argv) {
   if (argc != 2) {
      std::cerr << "usage: " << argv[0] << " <output.tga>\n";
      return 1;
   }

   int sections = 0;
   std::shared_ptr<TetVolume> volume = buildSmokeCage(sections);
   std::shared_ptr<TetPose> poseOne = volume->createPose();
   std::shared_ptr<TetPose> poseTwo = volume->createPose();

   RayTracer tracer(16, 12, 2, 1, 1);
   std::istringstream scene(
      "cameraPosition 0 0 6 cameraLookAt 0 0 0 cameraScreenWidth 6 "
      "sphere 0 0 0 1 FlatColor 0.8 0.4 0.2 null "
      "light 0 3 5 0.9");
   tracer.readScene(scene);

   const Vector viewOrigin(0, 0, 6);
   const Vector viewDir(0, 0, -1);

   // First pose: rest geometry, glowing volume between camera and sphere.
   tracer.setVolume(poseOne);
   Ray rayOne(viewOrigin, viewDir, 2, tracer.startingMaterial);
   Color colorOne = tracer.sampleRay(rayOne);
   std::cout << "pose1 rest linear color=(" << colorOne.r << ", "
             << colorOne.g << ", " << colorOne.b << ")\n";
   printCounters("pose1", *poseOne);

   // Second pose: bend the cage by translating its middle cross-section.
   // Only the six incident tets get new inverse maps; the BVH is refit, not
   // rebuilt, and pose one is completely untouched.
   const int middleSection = sections / 2;
   std::vector<int> ids;
   std::vector<Vector> positions;
   const Vector bend(0.6, 0.0, 0.0);
   for (int k = 0; k < 3; ++k) {
      int id = 3 * middleSection + k;
      ids.push_back(id);
      positions.push_back(poseTwo->position(id) + bend);
   }
   poseTwo->update(ids, positions);

   tracer.setVolume(poseTwo);
   Ray rayTwo(viewOrigin, viewDir, 2, tracer.startingMaterial);
   Color colorTwo = tracer.sampleRay(rayTwo);
   std::cout << "pose2 bent linear color=(" << colorTwo.r << ", "
             << colorTwo.g << ", " << colorTwo.b << ")\n";
   printCounters("pose2", *poseTwo);
   printCounters("pose1", *poseOne); // first pose keeps its own generation

   // Render the full image through the bent pose to the requested TGA.
   tracer.traceRays(argv[1]);
   return 0;
}
