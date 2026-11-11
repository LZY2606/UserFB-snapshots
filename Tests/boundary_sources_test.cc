/*
** SGI FREE SOFTWARE LICENSE B (Version 2.0, Sept. 18, 2008)
** Copyright (C) [dates of first publication] Silicon Graphics, Inc.
** All Rights Reserved.
**
** Permission is hereby granted, free of charge, to any person obtaining a copy
** of this software and associated documentation files (the "Software"), to deal
** in the Software without restriction, including without limitation the rights
** to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
** of the Software, and to permit persons to whom the Software is furnished to do so,
** subject to the following conditions:
**
** The above copyright including the dates of first publication and either this
** permission notice or a reference to http://oss.sgi.com/projects/FreeB/ shall be
** included in all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
** INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
** PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL SILICON GRAPHICS, INC.
** BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
** TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
** OR OTHER DEALINGS IN THE SOFTWARE.
*/

// Tests for the optional TESS_BOUNDARY_SOURCES feature.  Every check works
// through the public API: directed output boundary edges are compared as an
// unordered set, and each edge is matched against the exact set of original
// contour edges covering it, including the t0/t1 parameter reconstruction.

#include <math.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "tesselator.h"

namespace {

// All test coordinates have magnitude at most 10, so an absolute tolerance
// of 1e-5 on coordinates (relative ~1e-6) and on parameters is comfortably
// inside single-float rounding for these scales; no extended precision is
// required by the library contract.
constexpr float kEps = 1e-5f;

struct Pt {
  float x, y;
};

struct SourceRec {
  int contour;
  int edge;
  float t0, t1;
};

struct BoundaryEdge {
  Pt a, b;  // directed, a -> b
  std::vector<SourceRec> sources;
};

bool Near(float x, float y) { return fabsf(x - y) <= kEps; }

bool SamePt(const Pt& u, const Pt& v) {
  return Near(u.x, v.x) && Near(u.y, v.y);
}

// Pulls every directed boundary edge and its CSR source range out of the
// tesselator.
std::vector<BoundaryEdge> Collect(TESStesselator* tess) {
  std::vector<BoundaryEdge> out;
  const TESSreal* verts = tessGetVertices(tess);
  const TESSindex* elems = tessGetElements(tess);
  const TESSindex* offsets = tessGetBoundarySourceOffsets(tess);
  const TESSboundarySource* recs = tessGetBoundarySources(tess);
  const int ncontours = tessGetElementCount(tess);
  const int nverts = tessGetVertexCount(tess);

  if (offsets != nullptr) {
    EXPECT_EQ(offsets[nverts], tessGetBoundarySourceCount(tess));
  }

  for (int c = 0; c < ncontours; ++c) {
    const int base = elems[2 * c];
    const int count = elems[2 * c + 1];
    for (int j = 0; j < count; ++j) {
      const int k = base + j;
      const int kn = base + (j + 1) % count;
      BoundaryEdge be;
      be.a = {verts[2 * k], verts[2 * k + 1]};
      be.b = {verts[2 * kn], verts[2 * kn + 1]};
      if (offsets != nullptr && recs != nullptr) {
        for (TESSindex s = offsets[k]; s < offsets[k + 1]; ++s) {
          be.sources.push_back(
              {recs[s].contour, recs[s].edge, recs[s].t0, recs[s].t1});
        }
      }
      out.push_back(be);
    }
  }
  return out;
}

// Finds the unique directed edge a -> b in the collected set.
const BoundaryEdge& FindEdge(const std::vector<BoundaryEdge>& edges, Pt a,
                             Pt b) {
  static BoundaryEdge empty;
  const BoundaryEdge* found = nullptr;
  for (const auto& e : edges) {
    if (SamePt(e.a, a) && SamePt(e.b, b)) {
      EXPECT_EQ(found, nullptr) << "duplicate edge in output";
      found = &e;
    }
  }
  EXPECT_NE(found, nullptr) << "missing edge (" << a.x << "," << a.y << ")->("
                            << b.x << "," << b.y << ")";
  return found != nullptr ? *found : empty;
}

// A source identity without parameter expectations.
struct SourceId {
  int contour, edge;
};

bool HasSource(const BoundaryEdge& e, int contour, int edge) {
  for (const auto& s : e.sources) {
    if (s.contour == contour && s.edge == edge) return true;
  }
  return false;
}

// All boundary edges that carry a given source identity, regardless of the
// output traversal direction.
std::vector<const BoundaryEdge*> EdgesWithSource(
    const std::vector<BoundaryEdge>& edges, int contour, int edge) {
  std::vector<const BoundaryEdge*> out;
  for (const auto& e : edges) {
    if (HasSource(e, contour, edge)) out.push_back(&e);
  }
  return out;
}

bool UsesEndpoints(const BoundaryEdge& e, Pt p, Pt q) {
  return (SamePt(e.a, p) && SamePt(e.b, q)) ||
         (SamePt(e.a, q) && SamePt(e.b, p));
}

void ExpectSourceSet(const BoundaryEdge& e,
                     const std::vector<SourceId>& expected) {
  ASSERT_EQ(e.sources.size(), expected.size());
  // Records must be sorted by (contour, edge) and contain no duplicates.
  for (size_t i = 1; i < e.sources.size(); ++i) {
    const SourceRec& prev = e.sources[i - 1];
    const SourceRec& cur = e.sources[i];
    EXPECT_TRUE(prev.contour < cur.contour ||
                (prev.contour == cur.contour && prev.edge < cur.edge));
  }
  for (const auto& id : expected) {
    EXPECT_TRUE(HasSource(e, id.contour, id.edge));
  }
}

const SourceRec* GetSource(const BoundaryEdge& e, int contour, int edge) {
  for (const auto& s : e.sources) {
    if (s.contour == contour && s.edge == edge) return &s;
  }
  return nullptr;
}

// Reconstructs P(t) on contour c, edge j.
Pt EvalSource(const std::vector<std::vector<Pt>>& contours, int c, int j,
              float t) {
  const std::vector<Pt>& contour = contours[c];
  const Pt& p0 = contour[j];
  const Pt& p1 = contour[(j + 1) % static_cast<int>(contour.size())];
  return {p0.x + t * (p1.x - p0.x), p0.y + t * (p1.y - p0.y)};
}

// Verifies that every source record reconstructs the edge endpoints through
// its t0/t1 parameters, and that both parameters lie in [0,1].
void ExpectReconstruction(const BoundaryEdge& e,
                          const std::vector<std::vector<Pt>>& contours) {
  for (const auto& s : e.sources) {
    EXPECT_GE(s.t0, -kEps);
    EXPECT_LE(s.t0, 1.0f + kEps);
    EXPECT_GE(s.t1, -kEps);
    EXPECT_LE(s.t1, 1.0f + kEps);
    Pt p0 = EvalSource(contours, s.contour, s.edge, s.t0);
    Pt p1 = EvalSource(contours, s.contour, s.edge, s.t1);
    EXPECT_TRUE(SamePt(p0, e.a))
        << "t0 mismatch on contour " << s.contour << " edge " << s.edge;
    EXPECT_TRUE(SamePt(p1, e.b))
        << "t1 mismatch on contour " << s.contour << " edge " << s.edge;
  }
}

void AddContour(TESStesselator* tess, const std::vector<Pt>& pts) {
  tessAddContour(tess, 2, pts.data(), 2 * sizeof(TESSreal),
                 static_cast<int>(pts.size()));
}

constexpr float kNormal[3] = {0, 0, 1};

bool RunBoundary(TESStesselator* tess, int rule) {
  return tessTesselate(tess, rule, TESS_BOUNDARY_CONTOURS, 3, 2, kNormal) != 0;
}

// A simple square, CCW in input order.
std::vector<Pt> Square(float x0, float y0, float x1, float y1) {
  return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
}

TEST(BoundarySources, RegularAndCrossing) {
  // Plain square: each boundary edge has exactly one full-range source.
  const std::vector<Pt> square = Square(0, 0, 4, 4);
  TESStesselator* tess = tessNewTess(nullptr);
  ASSERT_NE(tess, nullptr);
  tessSetOption(tess, TESS_BOUNDARY_SOURCES, 1);
  AddContour(tess, square);
  ASSERT_TRUE(RunBoundary(tess, TESS_WINDING_NONZERO));

  std::vector<std::vector<Pt>> inputs = {square};
  std::vector<BoundaryEdge> edges = Collect(tess);
  ASSERT_EQ(edges.size(), 4u);
  // Check the four labeled edges explicitly.
  ExpectSourceSet(FindEdge(edges, {0, 0}, {4, 0}), {{0, 0}});
  ExpectSourceSet(FindEdge(edges, {4, 0}, {4, 4}), {{0, 1}});
  ExpectSourceSet(FindEdge(edges, {4, 4}, {0, 4}), {{0, 2}});
  ExpectSourceSet(FindEdge(edges, {0, 4}, {0, 0}), {{0, 3}});
  for (const auto& e : edges) ExpectReconstruction(e, inputs);
  tessDeleteTess(tess);

  // Bowtie self-intersection under the odd rule produces two triangles.
  // The crossing splits both participating edges at t = 0.5, and the output
  // boundary keeps both orientations of the split source edges.
  const std::vector<Pt> bow = {{0, 0}, {4, 4}, {0, 4}, {4, 0}};
  tess = tessNewTess(nullptr);
  ASSERT_NE(tess, nullptr);
  tessSetOption(tess, TESS_BOUNDARY_SOURCES, 1);
  AddContour(tess, bow);
  ASSERT_TRUE(RunBoundary(tess, TESS_WINDING_ODD));
  std::vector<std::vector<Pt>> bowInputs = {bow};
  edges = Collect(tess);
  ASSERT_EQ(tessGetElementCount(tess), 2);
  ASSERT_EQ(edges.size(), 6u);

  // Source edge 0 (0,0)->(4,4) is split at the crossing (2,2); both pieces
  // reach the boundary, in whatever traversal direction the library chooses.
  const std::vector<const BoundaryEdge*> e0pieces =
      EdgesWithSource(edges, 0, 0);
  ASSERT_EQ(e0pieces.size(), 2u);
  bool e0low = false, e0high = false;
  for (const BoundaryEdge* e : e0pieces) {
    ASSERT_EQ(e->sources.size(), 1u);
    const SourceRec* r = GetSource(*e, 0, 0);
    ASSERT_NE(r, nullptr);
    if (UsesEndpoints(*e, {0, 0}, {2, 2})) {
      e0low = true;
      EXPECT_NEAR(fminf(r->t0, r->t1), 0.0f, kEps);
      EXPECT_NEAR(fmaxf(r->t0, r->t1), 0.5f, kEps);
    } else if (UsesEndpoints(*e, {2, 2}, {4, 4})) {
      e0high = true;
      EXPECT_NEAR(fminf(r->t0, r->t1), 0.5f, kEps);
      EXPECT_NEAR(fmaxf(r->t0, r->t1), 1.0f, kEps);
    }
  }
  EXPECT_TRUE(e0low && e0high);

  // Source edge 2 (0,4)->(4,0) crosses the same point and is likewise split;
  // its pieces may be oriented either way, including reversed parameters.
  const std::vector<const BoundaryEdge*> e2pieces =
      EdgesWithSource(edges, 0, 2);
  ASSERT_EQ(e2pieces.size(), 2u);
  bool e2left = false, e2right = false;
  for (const BoundaryEdge* e : e2pieces) {
    ASSERT_EQ(e->sources.size(), 1u);
    const SourceRec* r = GetSource(*e, 0, 2);
    ASSERT_NE(r, nullptr);
    if (UsesEndpoints(*e, {0, 4}, {2, 2})) {
      e2left = true;
      EXPECT_NEAR(fminf(r->t0, r->t1), 0.0f, kEps);
      EXPECT_NEAR(fmaxf(r->t0, r->t1), 0.5f, kEps);
    } else if (UsesEndpoints(*e, {4, 0}, {2, 2})) {
      e2right = true;
      EXPECT_NEAR(fminf(r->t0, r->t1), 0.5f, kEps);
      EXPECT_NEAR(fmaxf(r->t0, r->t1), 1.0f, kEps);
    }
  }
  EXPECT_TRUE(e2left && e2right);
  // The crossing contour is closed, so at least one boundary piece runs
  // opposite to its source edge and carries a reversed t0 > t1.
  bool anyReversed = false;
  for (const auto& e : edges) {
    for (const auto& s : e.sources) {
      if (s.t0 > s.t1) anyReversed = true;
    }
  }
  EXPECT_TRUE(anyReversed);
  for (const auto& e : edges) ExpectReconstruction(e, bowInputs);
  tessDeleteTess(tess);
}

TEST(BoundarySources, CoincidentOwnership) {
  const std::vector<Pt> ccw = Square(0, 0, 4, 4);
  // Same square, clockwise.
  const std::vector<Pt> cw = {{0, 0}, {0, 4}, {4, 4}, {4, 0}};

  // Two forward copies and one reversed copy.  Under the nonzero rule the
  // boundary survives and every segment keeps all three identities, one of
  // them parameterized backwards.
  TESStesselator* tess = tessNewTess(nullptr);
  ASSERT_NE(tess, nullptr);
  tessSetOption(tess, TESS_BOUNDARY_SOURCES, 1);
  AddContour(tess, ccw);
  AddContour(tess, ccw);
  AddContour(tess, cw);
  ASSERT_TRUE(RunBoundary(tess, TESS_WINDING_NONZERO));
  std::vector<std::vector<Pt>> inputs = {ccw, ccw, cw};
  std::vector<BoundaryEdge> edges = Collect(tess);
  ASSERT_EQ(edges.size(), 4u);
  const BoundaryEdge& bottom = FindEdge(edges, {0, 0}, {4, 0});
  ExpectSourceSet(bottom, {{0, 0}, {1, 0}, {2, 3}});
  const SourceRec* reversed = GetSource(bottom, 2, 3);
  ASSERT_NE(reversed, nullptr);
  EXPECT_GT(reversed->t0, reversed->t1);  // opposite direction
  for (const auto& e : edges) ExpectReconstruction(e, inputs);
  tessDeleteTess(tess);

  // Two identical copies under the odd rule fill on neither side of the
  // coincident lines, so no boundary edge and no sources are produced.
  tess = tessNewTess(nullptr);
  ASSERT_NE(tess, nullptr);
  tessSetOption(tess, TESS_BOUNDARY_SOURCES, 1);
  AddContour(tess, ccw);
  AddContour(tess, ccw);
  ASSERT_TRUE(RunBoundary(tess, TESS_WINDING_ODD));
  EXPECT_EQ(tessGetElementCount(tess), 0);
  EXPECT_EQ(tessGetVertexCount(tess), 0);
  EXPECT_EQ(tessGetBoundarySourceCount(tess), 0);
  tessDeleteTess(tess);

  // Three contours sharing one segment: two squares share edge x = 2 and a
  // third copy doubles the first square.  The shared internal segment is not
  // output; the doubled outer edges carry two identities.
  const std::vector<Pt> left = Square(0, 0, 2, 2);
  const std::vector<Pt> right = Square(2, 0, 4, 2);
  tess = tessNewTess(nullptr);
  ASSERT_NE(tess, nullptr);
  tessSetOption(tess, TESS_BOUNDARY_SOURCES, 1);
  AddContour(tess, left);
  AddContour(tess, right);
  AddContour(tess, left);
  ASSERT_TRUE(RunBoundary(tess, TESS_WINDING_NONZERO));
  std::vector<std::vector<Pt>> threeInputs = {left, right, left};
  edges = Collect(tess);
  ExpectSourceSet(FindEdge(edges, {0, 0}, {2, 0}), {{0, 0}, {2, 0}});
  ExpectSourceSet(FindEdge(edges, {2, 0}, {4, 0}), {{1, 0}});
  // No output edge may run along the internal shared segment x = 2.
  for (const auto& e : edges) {
    EXPECT_FALSE(SamePt(e.a, {2, 0}) && SamePt(e.b, {2, 2}));
    EXPECT_FALSE(SamePt(e.a, {2, 2}) && SamePt(e.b, {2, 0}));
    ExpectReconstruction(e, threeInputs);
  }
  tessDeleteTess(tess);
}

TEST(BoundarySources, PartialOverlap) {
  // Two unit-height rectangles overlapping on x in [2,4].
  const std::vector<Pt> a = Square(0, 0, 4, 4);
  const std::vector<Pt> b = Square(2, 0, 6, 4);
  TESStesselator* tess = tessNewTess(nullptr);
  ASSERT_NE(tess, nullptr);
  tessSetOption(tess, TESS_BOUNDARY_SOURCES, 1);
  AddContour(tess, a);
  AddContour(tess, b);
  ASSERT_TRUE(RunBoundary(tess, TESS_WINDING_NONZERO));
  std::vector<std::vector<Pt>> inputs = {a, b};
  std::vector<BoundaryEdge> edges = Collect(tess);
  ASSERT_EQ(tessGetElementCount(tess), 1);

  // Bottom edge is broken exactly where the source ownership changes.
  const BoundaryEdge& leftFlank = FindEdge(edges, {0, 0}, {2, 0});
  const BoundaryEdge& shared = FindEdge(edges, {2, 0}, {4, 0});
  const BoundaryEdge& rightFlank = FindEdge(edges, {4, 0}, {6, 0});
  ExpectSourceSet(leftFlank, {{0, 0}});
  ExpectSourceSet(shared, {{0, 0}, {1, 0}});
  ExpectSourceSet(rightFlank, {{1, 0}});

  // Parameter ranges match the physical sub-segments.
  const SourceRec* s0 = GetSource(shared, 0, 0);
  const SourceRec* s1 = GetSource(shared, 1, 0);
  ASSERT_NE(s0, nullptr);
  ASSERT_NE(s1, nullptr);
  EXPECT_NEAR(s0->t0, 0.5f, kEps);
  EXPECT_NEAR(s0->t1, 1.0f, kEps);
  EXPECT_NEAR(s1->t0, 0.0f, kEps);
  EXPECT_NEAR(s1->t1, 0.5f, kEps);
  EXPECT_NEAR(GetSource(leftFlank, 0, 0)->t1, 0.5f, kEps);
  EXPECT_NEAR(GetSource(rightFlank, 1, 0)->t0, 0.5f, kEps);

  // The top overlap is covered in the opposite output direction.
  ExpectSourceSet(FindEdge(edges, {4, 4}, {2, 4}), {{0, 2}, {1, 2}});

  // A long output edge must never carry a source that does not cover it:
  // verify every record reconstructs both endpoints, which fails for any
  // over-long edge paired with a short source record.
  for (const auto& e : edges) ExpectReconstruction(e, inputs);
  tessDeleteTess(tess);
}

TEST(BoundarySources, WindingAndReverse) {
  const std::vector<Pt> ccw = Square(0, 0, 4, 4);
  const std::vector<Pt> cw = {{0, 0}, {0, 4}, {4, 4}, {4, 0}};

  // reverseContours changes only the winding interpretation; the recorded
  // contour/edge identities and parameter directions follow the actual input
  // edge orientation and must not be silently flipped.
  TESStesselator* tess = tessNewTess(nullptr);
  ASSERT_NE(tess, nullptr);
  tessSetOption(tess, TESS_BOUNDARY_SOURCES, 1);
  tessSetOption(tess, TESS_REVERSE_CONTOURS, 1);
  AddContour(tess, cw);  // treated as CCW for winding purposes
  ASSERT_TRUE(RunBoundary(tess, TESS_WINDING_POSITIVE));
  std::vector<std::vector<Pt>> cwInputs = {cw};
  std::vector<BoundaryEdge> edges = Collect(tess);
  ASSERT_EQ(edges.size(), 4u);
  // The directed output bottom edge (0,0)->(4,0) is sourced by input edge 3,
  // which itself runs (4,0)->(0,0), so the parameters must be reversed.
  const BoundaryEdge& bottom = FindEdge(edges, {0, 0}, {4, 0});
  ExpectSourceSet(bottom, {{0, 3}});
  const SourceRec* rec = GetSource(bottom, 0, 3);
  ASSERT_NE(rec, nullptr);
  EXPECT_GT(rec->t0, rec->t1);
  for (const auto& e : edges) ExpectReconstruction(e, cwInputs);
  tessDeleteTess(tess);

  // Without reversal the clockwise contour has negative winding and is
  // excluded by the positive rule, but included by the negative rule.
  tess = tessNewTess(nullptr);
  ASSERT_NE(tess, nullptr);
  tessSetOption(tess, TESS_BOUNDARY_SOURCES, 1);
  AddContour(tess, cw);
  ASSERT_TRUE(RunBoundary(tess, TESS_WINDING_POSITIVE));
  EXPECT_EQ(tessGetElementCount(tess), 0);
  EXPECT_EQ(tessGetBoundarySourceCount(tess), 0);
  tessDeleteTess(tess);

  tess = tessNewTess(nullptr);
  ASSERT_NE(tess, nullptr);
  tessSetOption(tess, TESS_BOUNDARY_SOURCES, 1);
  AddContour(tess, cw);
  ASSERT_TRUE(RunBoundary(tess, TESS_WINDING_NEGATIVE));
  std::vector<std::vector<Pt>> cwInputs2 = {cw};
  edges = Collect(tess);
  ASSERT_EQ(edges.size(), 4u);
  for (const auto& e : edges) ExpectReconstruction(e, cwInputs2);
  tessDeleteTess(tess);

  // Every winding rule either emits fully reconstructable sources or no
  // boundary at all.
  const int rules[] = {TESS_WINDING_ODD,        TESS_WINDING_NONZERO,
                       TESS_WINDING_POSITIVE,   TESS_WINDING_NEGATIVE,
                       TESS_WINDING_ABS_GEQ_TWO};
  for (int rule : rules) {
    tess = tessNewTess(nullptr);
    ASSERT_NE(tess, nullptr);
    tessSetOption(tess, TESS_BOUNDARY_SOURCES, 1);
    AddContour(tess, ccw);
    AddContour(tess, cw);
    ASSERT_TRUE(RunBoundary(tess, rule));
    std::vector<std::vector<Pt>> mixed = {ccw, cw};
    for (const auto& e : Collect(tess)) ExpectReconstruction(e, mixed);
    tessDeleteTess(tess);
  }
}

TEST(BoundarySources, ReuseAndLegacy) {
  const std::vector<Pt> square = Square(0, 0, 4, 4);

  // Legacy behavior with the option off: ordinary indices and elements keep
  // working and the source queries stay empty.
  TESStesselator* tess = tessNewTess(nullptr);
  ASSERT_NE(tess, nullptr);
  AddContour(tess, square);
  ASSERT_TRUE(RunBoundary(tess, TESS_WINDING_NONZERO));
  EXPECT_EQ(tessGetVertexCount(tess), 4);
  EXPECT_NE(tessGetVertices(tess), nullptr);
  EXPECT_NE(tessGetVertexIndices(tess), nullptr);
  const TESSindex* legacy = tessGetVertexIndices(tess);
  for (int i = 0; i < 4; ++i) EXPECT_NE(legacy[i], TESS_UNDEF);
  EXPECT_EQ(tessGetBoundarySourceCount(tess), 0);
  EXPECT_EQ(tessGetBoundarySources(tess), nullptr);
  EXPECT_EQ(tessGetBoundarySourceOffsets(tess), nullptr);

  // Enabling the option still allows polygon triangulation, which has no
  // boundary source output.
  tessSetOption(tess, TESS_BOUNDARY_SOURCES, 1);
  AddContour(tess, square);
  ASSERT_NE(tessTesselate(tess, TESS_WINDING_NONZERO, TESS_POLYGONS, 3, 2,
                          kNormal),
            0);
  EXPECT_EQ(tessGetElementCount(tess), 2);
  EXPECT_EQ(tessGetBoundarySourceCount(tess), 0);
  EXPECT_EQ(tessGetBoundarySources(tess), nullptr);
  tessDeleteTess(tess);

  // Reuse: sources from a successful run, then a second run on the same
  // object whose contour numbering restarts at zero.  Also overwrite the
  // caller's input buffer immediately after tessAddContour.
  tess = tessNewTess(nullptr);
  ASSERT_NE(tess, nullptr);
  tessSetOption(tess, TESS_BOUNDARY_SOURCES, 1);
  std::vector<float> buffer(square.size() * 2);
  for (size_t i = 0; i < square.size(); ++i) {
    buffer[2 * i] = square[i].x;
    buffer[2 * i + 1] = square[i].y;
  }
  tessAddContour(tess, 2, buffer.data(), 2 * sizeof(float), 4);
  std::fill(buffer.begin(), buffer.end(), -12345.0f);  // caller reuses memory
  ASSERT_TRUE(RunBoundary(tess, TESS_WINDING_NONZERO));
  ASSERT_EQ(tessGetBoundarySourceCount(tess), 4);
  std::vector<std::vector<Pt>> inputs = {square};
  for (const auto& e : Collect(tess)) ExpectReconstruction(e, inputs);

  const std::vector<Pt> triangle = {{0, 0}, {2, 0}, {0, 2}};
  AddContour(tess, triangle);
  ASSERT_TRUE(RunBoundary(tess, TESS_WINDING_NONZERO));
  const TESSboundarySource* recs = tessGetBoundarySources(tess);
  ASSERT_NE(recs, nullptr);
  for (int i = 0; i < tessGetBoundarySourceCount(tess); ++i) {
    EXPECT_EQ(recs[i].contour, 0);  // numbering restarted
  }
  std::vector<std::vector<Pt>> triInputs = {triangle};
  for (const auto& e : Collect(tess)) ExpectReconstruction(e, triInputs);
  tessDeleteTess(tess);

  // Queries on a fresh, never-run object are empty.
  tess = tessNewTess(nullptr);
  ASSERT_NE(tess, nullptr);
  tessSetOption(tess, TESS_BOUNDARY_SOURCES, 1);
  EXPECT_EQ(tessGetBoundarySourceCount(tess), 0);
  EXPECT_EQ(tessGetBoundarySources(tess), nullptr);
  EXPECT_EQ(tessGetBoundarySourceOffsets(tess), nullptr);
  tessDeleteTess(tess);
}

// Counting allocator used to drive allocation failures and verify that every
// successful allocation is released exactly once.
struct CountingAlloc {
  int failAfter = -1;  // -1 = never fail
  int allocCount = 0;
  int freeCount = 0;
  int activeBytes = 0;
  TESSalloc alloc;
};

void* CountingMalloc(void* userData, unsigned int size) {
  CountingAlloc* c = static_cast<CountingAlloc*>(userData);
  if (c->failAfter >= 0 && c->allocCount >= c->failAfter) return nullptr;
  void* p = malloc(size);
  if (p != nullptr) {
    ++c->allocCount;
    c->activeBytes += static_cast<int>(size);
  }
  return p;
}

void* CountingRealloc(void* userData, void* ptr, unsigned int size) {
  CountingAlloc* c = static_cast<CountingAlloc*>(userData);
  if (ptr == nullptr) return CountingMalloc(userData, size);
  if (c->failAfter >= 0 && c->allocCount >= c->failAfter) return nullptr;
  void* p = realloc(ptr, size);
  if (p != nullptr) ++c->allocCount;
  return p;
}

void CountingFree(void* userData, void* ptr) {
  CountingAlloc* c = static_cast<CountingAlloc*>(userData);
  if (ptr != nullptr) {
    ++c->freeCount;
    free(ptr);
  }
}

TEST(BoundarySources, AllocationFailure) {
  const std::vector<Pt> a = Square(0, 0, 4, 4);
  const std::vector<Pt> b = Square(2, 0, 6, 4);

  // Sweep the failure point across every allocation.  Each failed run must
  // report failure and OUT_OF_MEMORY, and deleting the object must release
  // every allocation exactly once with no double free.
  bool sawFailure = false;
  for (int threshold = 0; threshold < 60; ++threshold) {
    CountingAlloc ca;
    ca.failAfter = threshold;
    ca.alloc = {CountingMalloc, CountingRealloc, CountingFree, &ca,
                512, 512, 256, 512, 256, 0};
    TESStesselator* tess = tessNewTess(&ca.alloc);
    if (tess == nullptr) {
      // Even constructing the object may fail; nothing else to clean up.
      sawFailure = true;
      continue;
    }
    tessSetOption(tess, TESS_BOUNDARY_SOURCES, 1);
    AddContour(tess, a);
    AddContour(tess, b);
    int rc = tessTesselate(tess, TESS_WINDING_NONZERO,
                           TESS_BOUNDARY_CONTOURS, 3, 2, kNormal);
    if (rc == 0) {
      sawFailure = true;
      EXPECT_EQ(tessGetStatus(tess), TESS_STATUS_OUT_OF_MEMORY);
      EXPECT_EQ(tessGetBoundarySourceCount(tess), 0);
      EXPECT_EQ(tessGetBoundarySources(tess), nullptr);
    }
    tessDeleteTess(tess);
    EXPECT_EQ(ca.allocCount, ca.freeCount)
        << "unbalanced allocations at threshold " << threshold;
  }
  EXPECT_TRUE(sawFailure);

  // The same allocator supports a fully successful run and releases all
  // memory afterwards.
  CountingAlloc ok;
  ok.alloc = {CountingMalloc, CountingRealloc, CountingFree, &ok,
              512, 512, 256, 512, 256, 0};
  TESStesselator* tess = tessNewTess(&ok.alloc);
  ASSERT_NE(tess, nullptr);
  tessSetOption(tess, TESS_BOUNDARY_SOURCES, 1);
  AddContour(tess, a);
  AddContour(tess, b);
  ASSERT_TRUE(RunBoundary(tess, TESS_WINDING_NONZERO));
  EXPECT_GT(tessGetBoundarySourceCount(tess), 0);
  tessDeleteTess(tess);
  EXPECT_EQ(ok.allocCount, ok.freeCount);
}

}  // namespace
