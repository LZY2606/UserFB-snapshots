#include "task_support/check.h"

#include "RayTracer.h"
#include "Ray.h"
#include "BSP.h"
#include "Intersection.h"
#include "TetVolume.hpp"
#include "TetTransport.hpp"

#include <cmath>
#include <limits>
#include <memory>
#include <sstream>
#include <vector>

using namespace tetvol;

namespace {

constexpr double EPS = 1e-8;

bool close(double a, double b, double tol = EPS) {
   return std::fabs(a - b) <= tol;
}

// Positively-oriented cell from four vertex ids; flips the middle pair if the
// supplied ordering would be inverted.
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

// Independent reference half-space clip using an explicit barycentric solve,
// mirroring clipCell but written from scratch for the tests.
bool referenceClip(const TetPose& pose, std::size_t cell,
                   const Vector& origin, const Vector& dir,
                   double t0, double t1, double& tEnter, double& tExit) {
   const TetPose::InvMap& m = pose.inverseMap(cell);
   Vector rel0 = origin - m.p0;
   double b[4], s[4];
   b[1] = m.row1.dot(rel0);
   b[2] = m.row2.dot(rel0);
   b[3] = m.row3.dot(rel0);
   b[0] = 1.0 - b[1] - b[2] - b[3];
   s[1] = m.row1.dot(dir);
   s[2] = m.row2.dot(dir);
   s[3] = m.row3.dot(dir);
   s[0] = -(s[1] + s[2] + s[3]);

   double lo = t0, hi = t1;
   for (int k = 0; k < 4; ++k) {
      if (s[k] == 0.0) {
         if (b[k] < 0.0) return false;
         continue;
      }
      double t = -b[k] / s[k];
      if (s[k] > 0.0) lo = std::max(lo, t);
      else hi = std::min(hi, t);
      if (lo >= hi) return false;
   }
   if (!(hi > lo)) return false;
   tEnter = lo;
   tExit = hi;
   return true;
}

// Independent endpoint/trapezoid density integral using barycentric values
// evaluated directly at both interval ends.
double referenceQ(const TetPose& pose, std::size_t cell,
                  const Vector& origin, const Vector& dir,
                  double tEnter, double tExit) {
   const TetVolume& vol = pose.volume();
   const TetCell& c = vol.cell(cell);
   const TetPose::InvMap& m = pose.inverseMap(cell);
   auto densityAt = [&](double t) {
      Vector rel = (origin + dir * t) - m.p0;
      double lam[4];
      lam[1] = m.row1.dot(rel);
      lam[2] = m.row2.dot(rel);
      lam[3] = m.row3.dot(rel);
      lam[0] = 1.0 - lam[1] - lam[2] - lam[3];
      double rho = 0.0;
      for (int k = 0; k < 4; ++k) {
         rho += vol.vertexDensity(c.v[k]) * lam[k];
      }
      return rho;
   };
   double rhoA = densityAt(tEnter);
   double rhoB = densityAt(tExit);
   double arc = std::sqrt(dir.dot(dir));
   return 0.5 * (rhoA + rhoB) * (tExit - tEnter) * arc;
}

// Shared single-tet fixture: unit tetrahedron, varying vertex densities.
struct SingleTet {
   std::shared_ptr<TetVolume> volume;
   std::shared_ptr<TetPose> pose;
   SingleTet() {
      std::vector<Vector> verts = {
         Vector(0, 0, 0), Vector(1, 0, 0),
         Vector(0, 1, 0), Vector(0, 0, 1)};
      TetCell cell = makeCell(0, 1, 2, 3, verts, RGB(0.5, 0.25, 0.75),
                              RGB(1.0, 2.0, 3.0));
      std::vector<double> density = {1.0, 2.0, 3.0, 4.0};
      volume = std::make_shared<TetVolume>(verts,
                                           std::vector<TetCell>{cell}, density);
      pose = volume->createPose();
   }
};

// Axis-aligned box triangulated into six tets sharing the 0-6 diagonal.
struct BoxMesh {
   std::vector<Vector> verts;
   std::vector<TetCell> cells;
   std::vector<double> density;
   BoxMesh(const Vector& lo, const Vector& hi, const RGB& sigma,
           const RGB& source, double rho) {
      verts = {
         lo,
         Vector(hi.x, lo.y, lo.z),
         Vector(hi.x, hi.y, lo.z),
         Vector(lo.x, hi.y, lo.z),
         Vector(lo.x, lo.y, hi.z),
         Vector(hi.x, lo.y, hi.z),
         hi,
         Vector(lo.x, hi.y, hi.z)};
      static const int idx[6][4] = {
         {0, 1, 2, 6}, {0, 2, 3, 6}, {0, 3, 7, 6},
         {0, 7, 4, 6}, {0, 4, 5, 6}, {0, 5, 1, 6}};
      for (const auto& q : idx) {
         cells.push_back(makeCell(q[0], q[1], q[2], q[3], verts, sigma,
                                  source));
      }
      density.assign(verts.size(), rho);
   }
};

} // namespace

// ----------------------------- geometry group -----------------------------

static int test_single_cell_oblique() {
   SingleTet fixture;
   const TetPose& pose = *fixture.pose;
   Vector origin(0.1, 0.1, -1.0);
   Vector dir(0.05, 0.05, 1.0); // deliberately not normalized
   const double t0 = -10.0, t1 = 10.0;

   auto cursor = std::make_shared<TetCursor>(fixture.pose, origin, dir, t0, t1);
   TetInterval interval;
   TASK_CHECK(cursor->next(interval));
   TASK_CHECK(interval.cell == 0);
   TASK_CHECK(interval.tExit > interval.tEnter);

   double refEnter, refExit;
   TASK_CHECK(referenceClip(pose, 0, origin, dir, t0, t1, refEnter, refExit));
   TASK_CHECK(close(interval.tEnter, refEnter));
   TASK_CHECK(close(interval.tExit, refExit));

   TetInterval empty;
   TASK_CHECK(!cursor->next(empty));

   TetSegment seg = integrateSegment(pose, interval.cell, origin, dir,
                                     interval.tEnter, interval.tExit);
   double qRef = referenceQ(pose, 0, origin, dir, interval.tEnter,
                            interval.tExit);
   TASK_CHECK(close(seg.q, qRef));

   // Linear RGB transport values at unit scale.
   const TetCell& cell = pose.volume().cell(0);
   for (int ch = 0; ch < 3; ++ch) {
      double sigma = (&cell.sigma.r)[ch];
      double source = (&cell.source.r)[ch];
      double T = (&seg.transmittance.r)[ch];
      double C = (&seg.emission.r)[ch];
      TASK_CHECK(close(T, std::exp(-sigma * seg.q)));
      TASK_CHECK(close(C, source * (1.0 - T)));
   }
   return 0;
}

static int test_inside_start_axis_window() {
   SingleTet fixture;
   const TetPose& pose = *fixture.pose;
   // Start inside the tet, travel along a single axis, stop inside the tet.
   Vector origin(0.1, 0.1, 0.1);
   Vector dir(0.0, 0.0, 1.0);
   TetCursor cursor(fixture.pose, origin, dir, 0.0, 0.5);
   TetInterval interval;
   TASK_CHECK(cursor.next(interval));
   TASK_CHECK(close(interval.tEnter, 0.0)); // interval begins at the ray origin
   TASK_CHECK(close(interval.tExit, 0.5));  // finite window clips it inside

   double refEnter, refExit;
   TASK_CHECK(referenceClip(pose, 0, origin, dir, 0.0, 0.5, refEnter, refExit));
   TASK_CHECK(close(interval.tEnter, refEnter));
   TASK_CHECK(close(interval.tExit, refExit));
   TetSegment seg = integrateSegment(pose, 0, origin, dir, interval.tEnter,
                                     interval.tExit);
   TASK_CHECK(close(seg.q, referenceQ(pose, 0, origin, dir, interval.tEnter,
                                     interval.tExit)));
   return 0;
}

static int test_shared_face_and_owner() {
   // Two tets sharing face {0,1,2}; cell 0 apex +z, cell 1 apex -z.
   std::vector<Vector> verts = {
      Vector(0, 0, 0), Vector(1, 0, 0), Vector(0, 1, 0),
      Vector(0, 0, 1), Vector(0, 0, -1)};
   TetCell c0 = makeCell(0, 1, 2, 3, verts, RGB(0.1, 0.2, 0.3),
                         RGB(1, 2, 3));
   TetCell c1 = makeCell(0, 2, 1, 4, verts, RGB(0.4, 0.5, 0.6),
                         RGB(4, 5, 6));
   std::vector<double> density(5, 1.0);
   auto volume = std::make_shared<TetVolume>(
      verts, std::vector<TetCell>{c0, c1}, density);
   auto pose = volume->createPose();
   TASK_CHECK(volume->faceOwner(0, 1, 2) == 0); // smallest cell owns the face

   // Straight crossing through both cells, shared face at t = 1.
   TetCursor cursor(pose, Vector(0.1, 0.1, -1), Vector(0, 0, 1), 0.0, 10.0);
   TetInterval a, b;
   TASK_CHECK(cursor.next(a));
   TASK_CHECK(cursor.next(b));
   TASK_CHECK(a.cell == 1 && b.cell == 0); // near to far
   TASK_CHECK(close(a.tExit, b.tEnter));    // continuous, no recomputed gap
   TASK_CHECK(close(a.tEnter, 0.2));
   TASK_CHECK(close(b.tExit, 1.8));
   TetInterval none;
   TASK_CHECK(!cursor.next(none));

   // A ray propagating exactly inside the shared face plane is attributed to
   // exactly one cell: the smallest-index owner.
   TetCursor faceCursor(pose, Vector(-0.1, 0.1, 0), Vector(1, 0, 0),
                        0.0, 10.0);
   TetInterval face;
   TASK_CHECK(faceCursor.next(face));
   TASK_CHECK(face.cell == 0);
   TASK_CHECK(face.tExit > face.tEnter);
   TASK_CHECK(!faceCursor.next(none));

   // A ray running exactly along the shared edge {0,1} is likewise owned once.
   TASK_CHECK(volume->edgeOwner(0, 1) == 0);
   TetCursor edgeCursor(pose, Vector(-0.1, 0.0, 0.0), Vector(1, 0, 0),
                        0.0, 10.0);
   TetInterval edge;
   TASK_CHECK(edgeCursor.next(edge));
   TASK_CHECK(edge.cell == 0);
   TASK_CHECK(close(edge.tEnter, 0.1));
   TASK_CHECK(close(edge.tExit, 1.1));
   TASK_CHECK(!edgeCursor.next(none));
   return 0;
}

static int test_source_ordering() {
   std::vector<Vector> verts = {
      Vector(0, 0, 0), Vector(1, 0, 0), Vector(0, 1, 0),
      Vector(0, 0, 1), Vector(0, 0, -1)};
   auto build = [&](const RGB& s0, const RGB& s1) {
      TetCell c0 = makeCell(0, 1, 2, 3, verts, RGB(0.2, 0.2, 0.2), s0);
      TetCell c1 = makeCell(0, 2, 1, 4, verts, RGB(0.2, 0.2, 0.2), s1);
      std::vector<double> density(5, 1.0);
      auto volume = std::make_shared<TetVolume>(
         verts, std::vector<TetCell>{c0, c1}, density);
      return volume->createPose();
   };

   Vector origin(0.1, 0.1, -1), dir(0, 0, 1);
   auto poseAB = build(RGB(1.0, 0.0, 0.0), RGB(0.0, 0.0, 1.0));
   TetRayTransport ab = integrateRay(poseAB, origin, dir, 0.0, 10.0);
   auto poseBA = build(RGB(0.0, 0.0, 1.0), RGB(1.0, 0.0, 0.0));
   TetRayTransport ba = integrateRay(poseBA, origin, dir, 0.0, 10.0);

   // The nearer cell (cell 1, entered first) contributes its color before the
   // farther cell's transmittance, so swapping the per-cell sources must
   // change the accumulated color in a direction-dependent way.
   TASK_CHECK(ab.emission.b > ab.emission.r);
   TASK_CHECK(ba.emission.r > ba.emission.b);
   // Total transmittance is identical because sigma did not change.
   TASK_CHECK(close(ab.transmittance.r, ba.transmittance.r));
   TASK_CHECK(close(ab.transmittance.b, ba.transmittance.b));
   return 0;
}

static int test_bent_chain_not_global_affine() {
   // A chain of triangular prisms following a curved centerline.  Each prism
   // is three tets, so the bent volume cannot be represented by one global
   // affine map.
   const int N = 6;
   std::vector<Vector> verts;
   for (int i = 0; i <= N; ++i) {
      double z = static_cast<double>(i);
      Vector center(0.25 * std::sin(z), 0.0, z);
      verts.push_back(center + Vector(0, -1.2, 0));
      verts.push_back(center + Vector(1.0392, 0.6, 0));
      verts.push_back(center + Vector(-1.0392, 0.6, 0));
   }
   std::vector<TetCell> cells;
   for (int i = 0; i < N; ++i) {
      int a = 3 * i, b = a + 1, c = a + 2;
      int A = a + 3, B = a + 4, C = a + 5;
      cells.push_back(makeCell(a, b, c, A, verts, RGB(0.3, 0.2, 0.1),
                               RGB(0.4, 0.5, 0.6)));
      cells.push_back(makeCell(b, c, A, B, verts, RGB(0.3, 0.2, 0.1),
                               RGB(0.4, 0.5, 0.6)));
      cells.push_back(makeCell(c, A, B, C, verts, RGB(0.3, 0.2, 0.1),
                               RGB(0.4, 0.5, 0.6)));
   }
   std::vector<double> density(verts.size(), 1.0);
   auto volume = std::make_shared<TetVolume>(verts, cells, density);
   auto pose = volume->createPose();

   Vector origin(0, 0, -0.5), dir(0, 0, 1);
   auto cursor = std::make_shared<TetCursor>(pose, origin, dir, 0.0, 20.0);
   TetInterval interval;
   int count = 0;
   double previousExit = -1.0;
   double qTotal = 0.0;
   while (cursor->next(interval)) {
      if (count > 0) {
         TASK_CHECK(interval.tEnter >= previousExit - 1e-10);
      }
      previousExit = interval.tExit;
      TetSegment seg = integrateSegment(*pose, interval.cell, origin, dir,
                                        interval.tEnter, interval.tExit);
      qTotal += seg.q;
      ++count;
   }
   TASK_CHECK(count == 3 * N);
   // Unit density over a chain spanning length N along z.
   TASK_CHECK(close(qTotal, static_cast<double>(N), 1e-7));

   // A single global affine map (cell 0's inverse) evaluated at a point deep
   // in the chain yields out-of-range barycentric coordinates, whereas the
   // local inverse map of the owning cell keeps them in [0,1].
   const TetPose::InvMap& global = pose->inverseMap(0);
   int farCell = 3 * (N - 1);
   const TetPose::InvMap& local = pose->inverseMap(
      static_cast<std::size_t>(farCell));
   Vector farPoint = origin + dir * (static_cast<double>(N) - 0.25);

   Vector gRel = farPoint - global.p0;
   double gLam[4];
   gLam[1] = global.row1.dot(gRel);
   gLam[2] = global.row2.dot(gRel);
   gLam[3] = global.row3.dot(gRel);
   gLam[0] = 1.0 - gLam[1] - gLam[2] - gLam[3];
   bool globalInside = true;
   for (double l : gLam) {
      if (l < -1e-9 || l > 1.0 + 1e-9) globalInside = false;
   }
   TASK_CHECK(!globalInside); // one affine matrix cannot cover the bend

   Vector lRel = farPoint - local.p0;
   double lLam[4];
   lLam[1] = local.row1.dot(lRel);
   lLam[2] = local.row2.dot(lRel);
   lLam[3] = local.row3.dot(lRel);
   lLam[0] = 1.0 - lLam[1] - lLam[2] - lLam[3];
   for (double l : lLam) {
      TASK_CHECK(l >= -1e-9 && l <= 1.0 + 1e-9);
   }
   return 0;
}

// ------------------------------ updates group ------------------------------

static int test_local_update_counts() {
   SingleTet fixture;
   auto pose = fixture.pose;

   auto genBefore = pose->generation();
   auto invBefore = pose->inverseRecomputations();
   auto refitBefore = pose->boundsRefits();
   auto buildsBefore = pose->totalBuilds();

   // Moving one vertex touches exactly the one incident cell in this fixture.
   pose->update({1}, {Vector(1.0, 0.1, 0.0)});
   TASK_CHECK(pose->generation() == genBefore + 1);
   TASK_CHECK(pose->totalBuilds() == buildsBefore + 1);
   TASK_CHECK(pose->inverseRecomputations() == invBefore + 1);
   TASK_CHECK(pose->boundsRefits() > refitBefore);

   // Geometry really moved: a ray that used to hit now clips differently.
   TetCursor cursor(pose, Vector(0.1, 0.1, -1), Vector(0, 0, 1), 0.0, 10.0);
   TetInterval interval;
   TASK_CHECK(cursor.next(interval));
   TASK_CHECK(close(interval.tEnter, 1.0));

   // An empty update changes nothing, including the generation.
   auto genAt = pose->generation();
   pose->update({}, {});
   TASK_CHECK(pose->generation() == genAt);
   TASK_CHECK(pose->totalBuilds() == buildsBefore + 1);
   return 0;
}

static int test_rejected_update_keeps_old_cursor() {
   SingleTet fixture;
   auto pose = fixture.pose;
   auto cursor = std::make_shared<TetCursor>(pose, Vector(0.1, 0.1, -1),
                                             Vector(0, 0, 1), 0.0, 10.0);
   auto generation = pose->generation();

   bool threw = false;
   try {
      // Collapsing vertex 1 onto vertex 0 flips/degenerates the only cell.
      pose->update({1}, {Vector(0, 0, 0)});
   } catch (const std::invalid_argument&) {
      threw = true;
   }
   TASK_CHECK(threw);
   TASK_CHECK(pose->generation() == generation);

   // The old cursor and its captured inverse maps remain fully usable.
   TetInterval interval;
   TASK_CHECK(cursor->next(interval));
   TASK_CHECK(interval.cell == 0);
   TASK_CHECK(close(interval.tEnter, 1.0));
   return 0;
}

static int test_stale_cursor_throws() {
   SingleTet fixture;
   auto pose = fixture.pose;
   auto cursor = std::make_shared<TetCursor>(pose, Vector(0.1, 0.1, -1),
                                             Vector(0, 0, 1), 0.0, 10.0);
   TetInterval first;
   TASK_CHECK(cursor->next(first));

   // A successful (even identity-position) update bumps the generation.
   pose->update({2}, {Vector(0, 1, 0)});
   TASK_CHECK(cursor->generation() != pose->generation());

   bool threw = false;
   try {
      TetInterval stale;
      cursor->next(stale);
   } catch (const std::logic_error&) {
      threw = true;
   }
   TASK_CHECK(threw);

   // A fresh cursor on the new generation works normally.
   TetCursor fresh(pose, Vector(0.1, 0.1, -1), Vector(0, 0, 1), 0.0, 10.0);
   TetInterval interval;
   TASK_CHECK(fresh.next(interval));
   return 0;
}

static int test_shared_source_two_poses() {
   SingleTet fixture;
   auto poseA = fixture.volume->createPose();
   auto poseB = fixture.volume->createPose();

   poseA->update({3}, {Vector(0, 0, 1.5)});
   TASK_CHECK(poseA->generation() == 2);
   TASK_CHECK(poseB->generation() == 1); // independent pose, untouched

   // The second pose keeps its own cache and still traverses the rest shape.
   TetCursor cursorB(poseB, Vector(0.1, 0.1, -1), Vector(0, 0, 1), 0.0, 10.0);
   TetInterval interval;
   TASK_CHECK(cursorB.next(interval));
   TASK_CHECK(close(interval.tExit, 1.8));

   // And pose A reflects its moved apex with a longer z extent.
   TetCursor cursorA(poseA, Vector(0.1, 0.1, -1), Vector(0, 0, 1), 0.0, 10.0);
   TASK_CHECK(cursorA.next(interval));
   TASK_CHECK(interval.tExit > 1.8);

   // Mutating the original constructor inputs after construction cannot
   // change the resource or either pose: inputs are copied on build.
   std::vector<Vector> mutableVerts = {
      Vector(0, 0, 0), Vector(1, 0, 0), Vector(0, 1, 0), Vector(0, 0, 1)};
   std::vector<TetCell> mutableCells = {
      makeCell(0, 1, 2, 3, mutableVerts, RGB(1, 1, 1), RGB(1, 1, 1))};
   std::vector<double> mutableDensity = {1, 1, 1, 1};
   TetVolume guarded(mutableVerts, mutableCells, mutableDensity);
   auto guardedPose = guarded.createPose();
   mutableVerts[3] = Vector(9, 9, 9);
   mutableCells[0].source = RGB(7, 7, 7);
   mutableDensity[0] = 42.0;
   TASK_CHECK(guarded.restVertex(3).x == 0.0);
   TASK_CHECK(guarded.cell(0).source.r == 1.0);
   TASK_CHECK(guarded.vertexDensity(0) == 1.0);
   TetCursor guardedCursor(guardedPose, Vector(0.1, 0.1, -1),
                           Vector(0, 0, 1), 0.0, 10.0);
   TASK_CHECK(guardedCursor.next(interval));
   TASK_CHECK(close(interval.tEnter, 1.0));
   return 0;
}

static int test_bvh_first_interval_scaling() {
   // One near cell plus a large field of far-away cells.
   std::vector<Vector> verts;
   std::vector<TetCell> cells;
   auto addTet = [&](const Vector& a, const Vector& b, const Vector& c,
                     const Vector& d) {
      int base = static_cast<int>(verts.size());
      verts.insert(verts.end(), {a, b, c, d});
      cells.push_back(makeCell(base, base + 1, base + 2, base + 3, verts,
                               RGB(0.1, 0.1, 0.1), RGB(1, 1, 1)));
   };
   addTet(Vector(-.5, -.5, -.5), Vector(.5, -.5, -.5),
          Vector(-.5, .5, -.5), Vector(0, 0, .5));
   for (int i = 0; i < 2000; ++i) {
      double x = 10.0 + (i % 50) * 1.5;
      double y = (i / 50) * 1.5;
      addTet(Vector(x, y, 0), Vector(x + 1, y, 0),
             Vector(x, y + 1, 0), Vector(x, y, 1));
   }
   std::vector<double> density(verts.size(), 1.0);
   auto volume = std::make_shared<TetVolume>(verts, cells, density);
   auto pose = volume->createPose();

   auto cursor = std::make_shared<TetCursor>(pose, Vector(0, 0, -1),
                                             Vector(0, 0, 1), 0.0, 10.0);
   TetInterval first;
   TASK_CHECK(cursor->next(first));
   TASK_CHECK(first.cell == 0);

   // Reaching the first interval must not cost a linear scan of every cell.
   TASK_CHECK(cursor->cellTests() < 64);
   TASK_CHECK(cursor->cellTests() < cells.size() / 10);
   TASK_CHECK(cursor->nodeTests() < 128);
   return 0;
}

static int test_invalid_construction_and_updates() {
   std::vector<Vector> verts = {
      Vector(0, 0, 0), Vector(1, 0, 0), Vector(0, 1, 0), Vector(0, 0, 1)};
   std::vector<double> density = {1, 1, 1, 1};

   auto throwsArg = [](const std::vector<Vector>& v,
                       const std::vector<TetCell>& c,
                       const std::vector<double>& d) {
      try {
         TetVolume volume(v, c, d);
         return false;
      } catch (const std::invalid_argument&) {
         return true;
      }
   };

   TetCell good = makeCell(0, 1, 2, 3, verts, RGB(1, 1, 1), RGB(1, 1, 1));

   // Repeated / out-of-range index.
   TetCell badIndex;
   badIndex.v = {0, 1, 2, 2};
   badIndex.sigma = good.sigma;
   badIndex.source = good.source;
   TASK_CHECK(throwsArg(verts, {badIndex}, density));
   TetCell badRange = good;
   badRange.v = {0, 1, 2, 99};
   TASK_CHECK(throwsArg(verts, {badRange}, density));

   // Negative and non-finite density.
   std::vector<double> negDensity = {1, -1, 1, 1};
   TASK_CHECK(throwsArg(verts, {good}, negDensity));
   std::vector<double> nanDensity = {1, 1,
                                     std::numeric_limits<double>::quiet_NaN(), 1};
   TASK_CHECK(throwsArg(verts, {good}, nanDensity));

   // Flipped rest cell (negative orientation).
   TetCell flipped = good;
   flipped.v = {0, 2, 1, 3};
   TASK_CHECK(throwsArg(verts, {flipped}, density));

   // Negative sigma / source.
   TetCell badSigma = good;
   badSigma.sigma = RGB(-0.1, 1, 1);
   TASK_CHECK(throwsArg(verts, {badSigma}, density));
   TetCell badSource = good;
   badSource.source = RGB(1, -1, 1);
   TASK_CHECK(throwsArg(verts, {badSource}, density));

   // Update argument validation.
   auto volume = std::make_shared<TetVolume>(verts,
                                             std::vector<TetCell>{good},
                                             density);
   auto pose = volume->createPose();
   bool threw = false;
   try { pose->update({1, 1}, {Vector(1, 0, 0), Vector(1, 1, 0)}); }
   catch (const std::invalid_argument&) { threw = true; }
   TASK_CHECK(threw);
   threw = false;
   try { pose->update({7}, {Vector(0, 0, 0)}); }
   catch (const std::invalid_argument&) { threw = true; }
   TASK_CHECK(threw);
   threw = false;
   try {
      pose->update({1}, {Vector(std::numeric_limits<double>::infinity(), 0, 0)});
   } catch (const std::invalid_argument&) { threw = true; }
   TASK_CHECK(threw);
   TASK_CHECK(pose->generation() == 1); // nothing committed
   return 0;
}

// ------------------------------ render group -------------------------------

namespace {

RayTracer& buildSurfaceScene(RayTracer& tracer, bool withLight) {
   std::ostringstream scene;
   scene << "cameraPosition 0 0 6 cameraLookAt 0 0 0 cameraScreenWidth 8 "
         << "sphere 0 0 0 1 FlatColor 0.8 0.4 0.2 null ";
   if (withLight) {
      scene << "light 0 3 5 1.0 ";
   }
   std::istringstream in(scene.str());
   tracer.readScene(in);
   return tracer;
}

} // namespace

static int test_solid_truncation() {
   // Volume box straddling the sphere's near surface: it spans z in
   // [0.5, 3.0], while the unit sphere around the origin is first reached at
   // z = 1, so the surface must cut the volume window short.
   BoxMesh box(Vector(-1.5, -1.5, 0.5), Vector(1.5, 1.5, 3.0),
               RGB(0.4, 0.3, 0.2), RGB(0.5, 0.6, 0.7), 1.0);
   auto volume = std::make_shared<TetVolume>(box.verts, box.cells,
                                             box.density);
   auto pose = volume->createPose();

   Vector origin(0, 0, 6), dir(0, 0, -1);

   // Without a surface, the whole finite window shows full volume emission.
   TetRayTransport full = integrateRay(pose, origin, dir, 0.0, 6.0);
   TASK_CHECK(full.emission.r > 0.0);

   RayTracer tracer(4, 4, 1, 1, 1);
   buildSurfaceScene(tracer, false); // no point light: surface is ambient only
   Ray ray(origin, dir, 1, tracer.startingMaterial);
   Intersection hit = tracer.bsp->getClosestIntersection(ray);
   TASK_CHECK(hit.didIntersect);
   double sphereT = hit.distance;

   // The volume window starts before and ends after the sphere intersection.
   double lo, hi;
   TASK_CHECK(rayAABB(ray.origin, ray.direction, 0.0, 1e30,
                      pose->rootBounds(), lo, hi));
   TASK_CHECK(sphereT > lo && sphereT < hi);

   tracer.setVolume(pose);
   Color color = tracer.sampleRay(ray);

   // Recompose the expected result: truncated volume + T * surface radiance.
   // Use the Ray's offset origin so the arithmetic matches castRay exactly.
   // With no point light the surface term is ambient only and is not light
   // attenuated, so it matches the surface-only tracer bit for bit.
   TetRayTransport truncated = integrateRay(pose, ray.origin, ray.direction,
                                            std::max(lo, 0.0), sphereT);
   RayTracer plain(4, 4, 1, 1, 1);
   buildSurfaceScene(plain, false);
   Ray plainRay(origin, dir, 1, plain.startingMaterial);
   Color surface = plain.sampleRay(plainRay);
   double expectedR = truncated.emission.r +
                      truncated.transmittance.r * surface.r;
   double expectedG = truncated.emission.g +
                      truncated.transmittance.g * surface.g;
   double expectedB = truncated.emission.b +
                      truncated.transmittance.b * surface.b;
   TASK_CHECK(close(color.r, expectedR, 1e-7));
   TASK_CHECK(close(color.g, expectedG, 1e-7));
   TASK_CHECK(close(color.b, expectedB, 1e-7));

   // Truncation removes the part of the volume behind the sphere.
   TASK_CHECK(truncated.emission.r < full.emission.r);
   return 0;
}

static int test_colored_point_light_shadow() {
   // A colored volume between the surface point and the point light tints the
   // light reaching the surface, while solid occlusion stays in force.  Green
   // and blue extinction dominate, so red light survives preferentially.
   BoxMesh box(Vector(-0.8, 0.6, 1.0), Vector(0.8, 2.2, 3.0),
               RGB(0.15, 0.9, 0.9), RGB(0.0, 0.0, 0.0), 1.0);
   auto volume = std::make_shared<TetVolume>(box.verts, box.cells,
                                             box.density);
   auto pose = volume->createPose();

   RayTracer plain(4, 4, 1, 1, 1);
   buildSurfaceScene(plain, true);
   RayTracer tinted(4, 4, 1, 1, 1);
   buildSurfaceScene(tinted, true);
   tinted.setVolume(pose);

   Vector origin(0, 0, 6), dir(0, 0, -1);
   Ray rayA(origin, dir, 1, plain.startingMaterial);
   Ray rayB(origin, dir, 1, tinted.startingMaterial);
   Color base = plain.sampleRay(rayA);
   Color withVolume = tinted.sampleRay(rayB);

   // The shadow segment crosses the volume: red survives, green/blue fade.
   TASK_CHECK(base.g > 0.0);
   TASK_CHECK(withVolume.r < base.r + 1e-12);
   TASK_CHECK(withVolume.g < base.g);
   TASK_CHECK(withVolume.b < base.b);
   TASK_CHECK(withVolume.g / (base.g + 1e-300) <
              withVolume.r / (base.r + 1e-300));

   // Shadow rays still only carry transmittance: no volume glow is added to
   // the surface, so the result is never brighter than the baseline.
   TASK_CHECK(withVolume.r <= base.r + 1e-12);
   TASK_CHECK(withVolume.g <= base.g + 1e-12);
   TASK_CHECK(withVolume.b <= base.b + 1e-12);
   return 0;
}

static int test_recursive_reflection_segment() {
   // A mirror sphere reflects a ray down onto a floor; a volume placed on that
   // reflected sub-ray attenuates only the recursive segment.
   const std::string scene =
      "cameraPosition 0 0 6 cameraLookAt 0 0 0 cameraScreenWidth 8 "
      "sphere 0 0 0 2 ShinyColor 0 0 0 100 1.0 null "
      "triangle -50 -6.02 -20  50 -6.02 -20  50 -6.02 50 "
      "FlatColor 0.8 0.7 0.6 null "
      "light 5 5 5 1.0";

   RayTracer plain(8, 8, 2, 1, 1);
   {
      std::istringstream in(scene);
      plain.readScene(in);
   }
   RayTracer tinted(8, 8, 2, 1, 1);
   {
      std::istringstream in(scene);
      tinted.readScene(in);
   }

   // Reflected segment runs from about (0,-1.36,1.47) toward negative y.
   BoxMesh box(Vector(-1.0, -4.6, 0.2), Vector(1.0, -2.4, 1.8),
               RGB(0.5, 0.5, 0.5), RGB(0.0, 0.0, 0.0), 1.0);
   auto volume = std::make_shared<TetVolume>(box.verts, box.cells,
                                             box.density);
   auto pose = volume->createPose();
   tinted.setVolume(pose);

   Ray ray(Vector(0, 0, 6), Vector(0, -0.3, -1), 2,
           plain.startingMaterial);
   Ray rayTinted(Vector(0, 0, 6), Vector(0, -0.3, -1), 2,
                 tinted.startingMaterial);
   Color base = plain.sampleRay(ray);
   Color withVolume = tinted.sampleRay(rayTinted);

   TASK_CHECK(base.r > 0.0); // the reflected floor is visible
   TASK_CHECK(withVolume.r < base.r);
   TASK_CHECK(withVolume.g < base.g);
   TASK_CHECK(withVolume.b < base.b);

   // The direct segment to the sphere does not enter the lower volume, so the
   // change comes from the recursive segment only: verify the box is below the
   // sphere hit and never overlaps the primary ray.
   Intersection primary = plain.bsp->getClosestIntersection(
      Ray(Vector(0, 0, 6), Vector(0, -0.3, -1), 2, plain.startingMaterial));
   TASK_CHECK(primary.intersection.y > box.verts[0].y);
   return 0;
}

static int test_volume_off_unchanged_output() {
   // With no volume attached, output is identical to the surface-only tracer.
   RayTracer a(6, 5, 2, 1, 1);
   buildSurfaceScene(a, true);
   RayTracer b(6, 5, 2, 1, 1);
   buildSurfaceScene(b, true);
   TASK_CHECK(!b.volumePose); // explicitly unset

   const Vector origins[3] = {
      Vector(0.1, 0.15, 6), Vector(-0.7, 0.4, 6), Vector(0.5, -0.6, 6)};
   for (const Vector& o : origins) {
      Ray rayA(o, Vector(0, 0, -1), 2, a.startingMaterial);
      Ray rayB(o, Vector(0, 0, -1), 2, b.startingMaterial);
      Color ca = a.sampleRay(rayA);
      Color cb = b.sampleRay(rayB);
      TASK_CHECK(close(ca.r, cb.r, 0.0));
      TASK_CHECK(close(ca.g, cb.g, 0.0));
      TASK_CHECK(close(ca.b, cb.b, 0.0));
   }

   // A miss still returns black when there is no volume.
   Ray missRay(Vector(5, 5, 6), Vector(0, 0, -1), 1, a.startingMaterial);
   Color miss = a.sampleRay(missRay);
   TASK_CHECK(miss.r == 0.0 && miss.g == 0.0 && miss.b == 0.0);

   // Attaching a volume makes the same miss show volume glow instead.
   BoxMesh box(Vector(-0.5, -0.5, -1.0), Vector(0.5, 0.5, 1.0),
               RGB(0.2, 0.2, 0.2), RGB(0.9, 0.8, 0.7), 1.0);
   auto volume = std::make_shared<TetVolume>(box.verts, box.cells,
                                             box.density);
   b.setVolume(volume->createPose());
   Ray glowRay(Vector(0, 0, 6), Vector(0, 0, -1), 1,
               b.startingMaterial);
   Color glow = b.sampleRay(glowRay);
   TASK_CHECK(glow.r > 0.0 || glow.g > 0.0 || glow.b > 0.0);
   return 0;
}

// ------------------------------ registration -------------------------------

extern const struct task_case task_cases[] = {
   {"geometry", "single_cell_oblique_interval", test_single_cell_oblique},
   {"geometry", "inside_start_axis_window", test_inside_start_axis_window},
   {"geometry", "shared_face_continuity_owner", test_shared_face_and_owner},
   {"geometry", "source_front_back_order", test_source_ordering},
   {"geometry", "bent_chain_not_global_affine",
    test_bent_chain_not_global_affine},

   {"updates", "local_update_counts", test_local_update_counts},
   {"updates", "rejected_update_keeps_cursor",
    test_rejected_update_keeps_old_cursor},
   {"updates", "stale_cursor_logic_error", test_stale_cursor_throws},
   {"updates", "shared_source_two_poses", test_shared_source_two_poses},
   {"updates", "bvh_first_interval_scaling", test_bvh_first_interval_scaling},
   {"updates", "invalid_construction_updates",
    test_invalid_construction_and_updates},

   {"render", "solid_truncation", test_solid_truncation},
   {"render", "colored_point_light_shadow", test_colored_point_light_shadow},
   {"render", "recursive_reflection_segment",
    test_recursive_reflection_segment},
   {"render", "volume_off_unchanged_output", test_volume_off_unchanged_output},
};

extern const size_t task_case_count =
   sizeof(task_cases) / sizeof(task_cases[0]);
