// Tests for al::graphics::RetainedRenderer and TriangleDrawer scissoring.
//
// The retained renderer is always compared against an independent full-frame
// draw that uses the original TriangleDrawer entry points with no scissor.
// The test cases are registered with the task_support/check.h harness, which
// supplies main().
#include "task_support/check.h"

#include "RetainedRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using namespace al::graphics;

namespace {

const u32 BG     = ImageData::makePixel(12, 34, 56, 255);
const u32 BG2    = ImageData::makePixel(70, 80, 90, 255);
const u32 RED    = ImageData::makePixel(220, 30, 20, 255);
const u32 GREEN  = ImageData::makePixel(20, 200, 50, 255);
const u32 BLUE   = ImageData::makePixel(30, 60, 230, 255);
const u32 ORANGE = ImageData::makePixel(240, 140, 20, 255);
const u32 SENTINEL_COLOR = ImageData::makePixel(1, 2, 3, 4);
const double SENTINEL_DEPTH = std::numeric_limits<double>::infinity();

struct Obj
{
  std::uint64_t id;
  std::vector<Triangle> tris;
  RetainedRenderer::Material mat;

  Obj()
  : id(0), tris(), mat()
  {
  }

  Obj(std::uint64_t id_, const std::vector<Triangle>& tris_,
      const RetainedRenderer::Material& mat_)
  : id(id_), tris(tris_), mat(mat_)
  {
  }
};

Vector3f lightVector()
{
  return G3D::normalize(Vector3f(0.3f, 0.4f, 0.5f));
}

ImageData makeBlankTexture()
{
  ImageData texture(1, 1);
  texture.fill(ImageData::makePixel(0, 0, 0, 0));
  return texture;
}

// Distinct, deliberately asymmetric pattern so a transposed or mirrored
// texture lookup cannot accidentally match.
ImageData makeAsymmetricTexture(bool withHole)
{
  ImageData texture(6, 4);
  for (u32 y = 0; y < texture.height(); ++y) {
    for (u32 x = 0; x < texture.width(); ++x) {
      const u8 r = (u8)(20 + x * 35);
      const u8 g = (u8)(15 + y * 55);
      const u8 b = (u8)(40 + (x * 7 + y * 23) * 3);
      u8 a = 255;
      if (withHole && x == 1 && y == 2) {
        a = 0;
      }
      texture.setPixel(x, y, ImageData::makePixel(r, g, b, a));
    }
  }
  return texture;
}

Triangle triABC(float ax, float ay, float bx, float by, float cx, float cy,
                double depth, u32 color)
{
  Triangle t(ax, ay, 0.0f, bx, by, 0.0f, cx, cy, 0.0f, color);
  for (int i = 0; i < 3; ++i) {
    t[i].pos.w() = (float)depth;
    t[i].normal = Vector3f(0.0f, 0.0f, 1.0f);
    t[i].rgb = Vector3f(20.0f + i * 25.0f,
                        15.0f + i * 30.0f,
                        10.0f + i * 20.0f);
  }
  t.normal = Vector3f(0.0f, 0.0f, 1.0f);
  t.setTexUVs(0.02f, 0.02f, 0.02f, 0.98f, 0.98f, 0.02f);
  return t;
}

// Right triangle with the winding used by the original TriangleDrawer
// (negative area in y-down screen coordinates).
Triangle rightTri(float x, float y, float w, float h,
                  double depth, u32 color)
{
  return triABC(x, y, x, y + h, x + w, y, depth, color);
}

// Fills the whole given rectangle with two right triangles.
void addQuad(std::vector<Triangle>& out,
             float x0, float y0, float x1, float y1,
             double depth, u32 color)
{
  out.push_back(triABC(x0, y0, x0, y1, x1, y0, depth, color));
  out.push_back(triABC(x1, y0, x0, y1, x1, y1, depth, color));
}

void drawWithEntry(TriangleDrawer& drawer, int algo,
                   ImageData& image, const Triangle& triangle)
{
  switch (algo) {
    case 0: drawer.scanlineTriangle(image, triangle); break;
    case 1: drawer.barycentricTriangle(image, triangle); break;
    case 2: drawer.barycentricO1(image, triangle); break;
    default: drawer.barycentricO2(image, triangle); break;
  }
}

void drawFull(const std::vector<Obj>& objects, unsigned width, unsigned height,
              int algo, u32 background, ImageData& image, ZBuffer& depth)
{
  (void)width;
  (void)height;
  image.fill(background);
  depth.fill(std::numeric_limits<double>::infinity());

  TriangleDrawer drawer(&depth);
  drawer.algo = (TriangleDrawer::Algo)algo;
  ImageData blank = makeBlankTexture();

  for (std::size_t o = 0; o < objects.size(); ++o) {
    const RetainedRenderer::Material& material = objects[o].mat;
    drawer.drawMode = material.drawMode;
    drawer.lightsStyle = material.lightsStyle;
    drawer.lightDirection = material.lightDirection;
    drawer.textureImageData =
      material.hasTexture() ? (ImageData*)&material.texture : &blank;
    for (std::size_t t = 0; t < objects[o].tris.size(); ++t) {
      drawer.triangle(image, objects[o].tris[t]);
    }
  }
}

bool sameImage(const ImageData& a, const ImageData& b)
{
  if (a.width() != b.width() || a.height() != b.height()) {
    return false;
  }
  for (u32 y = 0; y < a.height(); ++y) {
    for (u32 x = 0; x < a.width(); ++x) {
      if (a.getPixel(x, y) != b.getPixel(x, y)) {
        return false;
      }
    }
  }
  return true;
}

bool closeDepth(double a, double b)
{
  const bool aInf = std::isinf(a);
  const bool bInf = std::isinf(b);
  if (aInf || bInf) {
    return aInf && bInf;
  }
  const double scale = std::max(std::fabs(a), std::fabs(b));
  return std::fabs(a - b) <= 1e-6 * std::max(scale, 1e-12) + 1e-12;
}

bool sameDepth(const ZBuffer& a, const ZBuffer& b)
{
  if (a.width() != b.width() || a.height() != b.height()) {
    return false;
  }
  for (u32 y = 0; y < a.height(); ++y) {
    for (u32 x = 0; x < a.width(); ++x) {
      if (!closeDepth(a.unsafeGet(x, y), b.unsafeGet(x, y))) {
        return false;
      }
    }
  }
  return true;
}

struct SceneResult
{
  bool samePixels;
  bool sameDepths;
  bool same;

  SceneResult() : samePixels(false), sameDepths(false), same(false) {}
};

SceneResult compareScene(const RetainedRenderer& renderer,
                         const std::vector<Obj>& objects,
                         int algo, u32 background)
{
  const unsigned width = renderer.width();
  const unsigned height = renderer.height();
  ImageData image(width, height);
  ZBuffer depth(width, height);
  drawFull(objects, width, height, algo, background, image, depth);

  SceneResult result;
  result.samePixels = sameImage(image, renderer.image());
  result.sameDepths = sameDepth(depth, renderer.depth());
  result.same = result.samePixels && result.sameDepths;
  return result;
}

bool bumped(const ZBuffer& depth, u32 x, u32 y, double expected)
{
  return closeDepth(depth.unsafeGet(x, y), expected);
}

// ---------------------------------------------------------------------------
// damage group
// ---------------------------------------------------------------------------

bool damageMoveReveals(int algo)
{
  const unsigned W = 48, H = 40;
  RetainedRenderer renderer(W, H, (TriangleDrawer::Algo)algo, BG);

  const RetainedRenderer::Material solid(
    TriangleDrawer::DRAW_SOLID, TriangleDrawer::LIGHTS_STYLE_NONE,
    lightVector());

  std::vector<Triangle> behind;
  addQuad(behind, 8, 8, 40, 32, 5.0, GREEN);

  std::vector<Triangle> front;
  front.push_back(rightTri(12, 12, 6, 6, 2.0, RED));

  std::vector<Obj> objects;
  objects.push_back(Obj(1, behind, solid));
  objects.push_back(Obj(2, front, solid));

  renderer.set(1, behind, solid);
  renderer.set(2, front, solid);
  renderer.flush();

  if (!compareScene(renderer, objects, algo, BG).same) {
    return false;
  }
  if (renderer.image().getPixel(13, 13) != RED) {
    return false;
  }
  if (!bumped(renderer.depth(), 13, 13, 2.0)) {
    return false;
  }
  if (renderer.image().getPixel(16, 16) != GREEN) {
    return false;
  }

  // Move the small triangle within the larger object.
  std::vector<Triangle> moved;
  moved.push_back(rightTri(28, 24, 6, 6, 2.0, RED));
  renderer.set(2, moved, solid);
  renderer.flush();

  objects[1].tris = moved;
  if (!compareScene(renderer, objects, algo, BG).same) {
    return false;
  }
  // The old location now reveals the object behind it.
  if (renderer.image().getPixel(13, 13) != GREEN) {
    return false;
  }
  if (!bumped(renderer.depth(), 13, 13, 5.0)) {
    return false;
  }
  // The new location draws in front.
  if (renderer.image().getPixel(30, 26) != RED) {
    return false;
  }
  return bumped(renderer.depth(), 30, 26, 2.0);
}

bool damageEraseReveals(int algo)
{
  const unsigned W = 48, H = 40;
  RetainedRenderer renderer(W, H, (TriangleDrawer::Algo)algo, BG);

  const RetainedRenderer::Material solid(
    TriangleDrawer::DRAW_SOLID, TriangleDrawer::LIGHTS_STYLE_NONE,
    lightVector());

  std::vector<Triangle> behind;
  addQuad(behind, 8, 8, 40, 32, 5.0, GREEN);
  std::vector<Triangle> front;
  front.push_back(rightTri(12, 12, 6, 6, 2.0, RED));

  std::vector<Obj> objects;
  objects.push_back(Obj(1, behind, solid));
  objects.push_back(Obj(2, front, solid));
  renderer.set(1, behind, solid);
  renderer.set(2, front, solid);
  renderer.flush();

  if (!renderer.erase(2)) {
    return false;
  }
  if (renderer.erase(2)) {
    return false; // second erase must report false
  }
  renderer.flush();
  objects.pop_back();

  if (!compareScene(renderer, objects, algo, BG).same) {
    return false;
  }
  if (renderer.image().getPixel(16, 16) != GREEN) {
    return false;
  }
  return bumped(renderer.depth(), 16, 16, 5.0);
}

bool damageEqualDepthReplaceAndReinsert(int algo)
{
  const unsigned W = 44, H = 36;
  RetainedRenderer renderer(W, H, (TriangleDrawer::Algo)algo, BG);

  const RetainedRenderer::Material redMat(
    TriangleDrawer::DRAW_SOLID, TriangleDrawer::LIGHTS_STYLE_NONE,
    lightVector());
  RetainedRenderer::Material blueMat = redMat;
  blueMat.drawMode = TriangleDrawer::DRAW_AFFINE; // no texture: writes nothing
  // Keep replacement in a mode that writes pixels.
  blueMat.drawMode = TriangleDrawer::DRAW_SOLID;

  // Identical geometry and depth for both objects: the first object must win.
  std::vector<Triangle> a;
  a.push_back(rightTri(12, 12, 8, 8, 3.0, RED));
  std::vector<Triangle> b;
  b.push_back(rightTri(12, 12, 8, 8, 3.0, GREEN));

  std::vector<Obj> objects;
  objects.push_back(Obj(1, a, redMat));
  objects.push_back(Obj(2, b, redMat));

  renderer.set(1, a, redMat);
  renderer.set(2, b, redMat);
  renderer.flush();
  if (!compareScene(renderer, objects, algo, BG).same) return false;
  if (renderer.image().getPixel(14, 14) != RED) return false;
  if (!bumped(renderer.depth(), 14, 14, 3.0)) return false;

  // Replacing the first object keeps its draw order, so blue still wins.
  std::vector<Triangle> blue;
  blue.push_back(rightTri(12, 12, 8, 8, 3.0, BLUE));
  renderer.set(1, blue, blueMat);
  renderer.flush();
  objects[0].tris = blue;
  objects[0].mat = blueMat;
  if (!compareScene(renderer, objects, algo, BG).same) return false;
  if (renderer.image().getPixel(14, 14) != BLUE) return false;

  // Deleting the first object reveals the equal-depth second one.
  if (!renderer.erase(1)) return false;
  renderer.flush();
  objects.erase(objects.begin());
  if (!compareScene(renderer, objects, algo, BG).same) return false;
  if (renderer.image().getPixel(14, 14) != GREEN) return false;

  // Re-inserting goes to the end of the draw order. At equal depth the
  // second object (now first) wins, so the image must stay green.
  renderer.set(1, blue, blueMat);
  renderer.flush();
  objects.push_back(Obj(1, blue, blueMat));
  if (!compareScene(renderer, objects, algo, BG).same) return false;
  return renderer.image().getPixel(14, 14) == GREEN;
}

bool damageTextureSnapshot(int algo)
{
  const unsigned W = 44, H = 34;
  RetainedRenderer renderer(W, H, (TriangleDrawer::Algo)algo, BG);

  ImageData original = makeAsymmetricTexture(false);
  RetainedRenderer::Material mat(
    TriangleDrawer::DRAW_AFFINE, TriangleDrawer::LIGHTS_STYLE_NONE,
    lightVector(), original);

  std::vector<Triangle> tris;
  tris.push_back(triABC(6, 6, 6, 26, 36, 6, 1.5, 0));
  std::vector<Obj> objects;
  objects.push_back(Obj(7, tris, mat));

  renderer.set(7, tris, mat);
  renderer.flush();
  if (!compareScene(renderer, objects, algo, BG).same) return false;

  // The renderer must own a snapshot: mutating the caller's texture without
  // publishing it through set() changes nothing.
  ImageData snapshot = renderer.image();
  original.fill(ImageData::makePixel(250, 250, 250, 255));
  renderer.flush();
  if (!sameImage(snapshot, renderer.image())) return false;
  if (renderer.stats().dirty_tiles != 0 ||
      renderer.stats().candidate_visits != 0 ||
      renderer.stats().raster_calls != 0) {
    return false;
  }

  // A re-set() publishes the new texture.
  RetainedRenderer::Material changedMat(
    TriangleDrawer::DRAW_AFFINE, TriangleDrawer::LIGHTS_STYLE_NONE,
    lightVector(), original);
  renderer.set(7, tris, changedMat);
  renderer.flush();
  objects[0].mat = changedMat;
  if (!compareScene(renderer, objects, algo, BG).same) return false;
  return !sameImage(snapshot, renderer.image());
}

bool damageTransparentHoles(int algo)
{
  const unsigned W = 52, H = 40;
  RetainedRenderer renderer(W, H, (TriangleDrawer::Algo)algo, BG);

  const RetainedRenderer::Material solid(
    TriangleDrawer::DRAW_SOLID, TriangleDrawer::LIGHTS_STYLE_NONE,
    lightVector());
  ImageData holeTexture = makeAsymmetricTexture(true);
  RetainedRenderer::Material textured(
    TriangleDrawer::DRAW_PERSPECTIVE, TriangleDrawer::LIGHTS_STYLE_NONE,
    lightVector(), holeTexture);

  std::vector<Triangle> behind;
  addQuad(behind, 6, 6, 46, 34, 5.0, GREEN);
  std::vector<Triangle> front;
  front.push_back(triABC(10, 10, 10, 30, 42, 10, 2.0, 0));

  std::vector<Obj> objects;
  objects.push_back(Obj(1, behind, solid));
  objects.push_back(Obj(2, front, textured));
  renderer.set(1, behind, solid);
  renderer.set(2, front, textured);
  renderer.flush();

  // Compare against the full-frame draw, including depths.
  if (!compareScene(renderer, objects, algo, BG).same) return false;

  // Alpha zero must leave both colour and depth untouched so the object
  // behind stays visible.
  std::size_t holes = 0;
  for (u32 y = 10; y < 30; ++y) {
    for (u32 x = 10; x < 42; ++x) {
      if (renderer.image().getPixel(x, y) == GREEN) {
        ++holes;
        if (!bumped(renderer.depth(), x, y, 5.0)) {
          return false;
        }
      }
    }
  }
  return holes > 0;
}

bool damageCrossTileContinuous(int algo)
{
  const unsigned W = 53, H = 41; // deliberately not a multiple of 8
  RetainedRenderer renderer(W, H, (TriangleDrawer::Algo)algo, BG2);

  const RetainedRenderer::Material solid(
    TriangleDrawer::DRAW_SOLID, TriangleDrawer::LIGHTS_STYLE_NONE,
    lightVector());
  const RetainedRenderer::Material shaded(
    TriangleDrawer::DRAW_RGB_SHADED, TriangleDrawer::LIGHTS_STYLE_SMOOTH,
    lightVector());
  const RetainedRenderer::Material affine(
    TriangleDrawer::DRAW_AFFINE, TriangleDrawer::LIGHTS_STYLE_FLAT,
    lightVector());

  std::vector<Triangle> behind;
  addQuad(behind, 2, 2, 51, 39, 9.0, GREEN);
  std::vector<Obj> objects;
  objects.push_back(Obj(1, behind, solid));
  renderer.set(1, behind, solid);

  ImageData texture = makeAsymmetricTexture(false);
  RetainedRenderer::Material textured = affine;
  textured.texture = texture;

  // A triangle that spans several 8x8 tiles, moved repeatedly. Materials are
  // replaced as well so draw mode and lighting changes are exercised.
  for (int step = 0; step < 6; ++step) {
    const float x = 5.0f + step * 3.0f;
    std::vector<Triangle> moving;
    moving.push_back(triABC(x, 6, x, 30, 45, 6, 2.0, ORANGE));

    const RetainedRenderer::Material* mats[3] = {&solid, &shaded, &textured};
    const RetainedRenderer::Material& mat = *mats[step % 3];

    renderer.set(2, moving, mat);
    renderer.flush();

    if (step == 0) {
      objects.push_back(Obj(2, moving, mat));
    }
    else {
      objects[1].tris = moving;
      objects[1].mat = mat;
    }
    if (!compareScene(renderer, objects, algo, BG2).same) return false;
  }

  // Erase and re-insert so the object moves to the end of the order.
  if (!renderer.erase(2)) return false;
  renderer.flush();
  objects.pop_back();
  if (!compareScene(renderer, objects, algo, BG2).same) return false;

  std::vector<Triangle> last;
  last.push_back(triABC(8, 8, 8, 32, 44, 8, 3.0, BLUE));
  renderer.set(2, last, solid);
  renderer.flush();
  objects.push_back(Obj(2, last, solid));
  return compareScene(renderer, objects, algo, BG2).same;
}

bool damageUpdatesBeforeFlushCoalesce(int algo)
{
  const unsigned W = 46, H = 34;
  RetainedRenderer renderer(W, H, (TriangleDrawer::Algo)algo, BG);

  const RetainedRenderer::Material solid(
    TriangleDrawer::DRAW_SOLID, TriangleDrawer::LIGHTS_STYLE_NONE,
    lightVector());

  // Two updates to the same object between flushes: the second position is
  // what must be drawn, and the tiles of the *first* position must be
  // repaired too (the object may have visited them before moving again).
  std::vector<Triangle> first;
  first.push_back(rightTri(4, 6, 10, 10, 2.0, RED));
  renderer.set(1, first, solid);

  std::vector<Triangle> second;
  second.push_back(rightTri(30, 20, 10, 10, 3.0, GREEN));
  std::vector<Triangle> third;
  third.push_back(rightTri(10, 24, 10, 8, 4.0, BLUE));

  renderer.set(1, second, solid);
  renderer.set(1, third, solid);

  // Erase and re-add a second object without any flush in between.
  std::vector<Triangle> other;
  other.push_back(rightTri(24, 4, 12, 12, 1.5, ORANGE));
  renderer.set(2, other, solid);
  renderer.erase(2);
  renderer.erase(2);
  renderer.set(2, other, solid);
  renderer.flush();

  // Only the last surviving versions exist in the reference.
  std::vector<Obj> objects;
  objects.push_back(Obj(1, third, solid));
  objects.push_back(Obj(2, other, solid));
  if (!compareScene(renderer, objects, algo, BG).same) return false;

  // Nothing from the first position or from the erased copy may remain.
  if (renderer.image().getPixel(6, 8) != BG) return false;
  if (renderer.image().getPixel(12, 26) != BLUE) return false;
  return renderer.image().getPixel(26, 6) == ORANGE;
}

bool damageFirstFlushInitializes(int algo)
{
  const unsigned W = 21, H = 19; // non-multiple of 8, odd size
  RetainedRenderer renderer(W, H, (TriangleDrawer::Algo)algo, BG);

  renderer.flush();
  RetainedRenderer::Stats s = renderer.stats();
  if (s.dirty_tiles != 3 * 3) return false; // ceil(21/8) x ceil(19/8)
  if (s.candidate_visits != 0) return false;
  if (s.raster_calls != 0) return false;

  for (u32 y = 0; y < H; ++y) {
    for (u32 x = 0; x < W; ++x) {
      if (renderer.image().getPixel(x, y) != BG) return false;
      if (!std::isinf(renderer.depth().unsafeGet(x, y))) return false;
    }
  }

  // A second flush without changes does no work at all.
  renderer.flush();
  return renderer.stats().dirty_tiles == 0 &&
         renderer.stats().candidate_visits == 0 &&
         renderer.stats().raster_calls == 0;
}

// ---------------------------------------------------------------------------
// scissor group
// ---------------------------------------------------------------------------

bool scissorInsideMatches(int algo, bool directEntry,
                          TriangleDrawer::DrawMode drawMode)
{
  const unsigned W = 37, H = 29;
  ImageData texture = makeAsymmetricTexture(true);
  Triangle triangle = triABC(2, 2, 2, 27, 35, 2, 1.2, 0);

  ImageData full(W, H);
  ZBuffer fullDepth(W, H);
  full.fill(BG);
  fullDepth.fill(std::numeric_limits<double>::infinity());
  TriangleDrawer fullDrawer(&fullDepth);
  fullDrawer.algo = (TriangleDrawer::Algo)algo;
  fullDrawer.drawMode = drawMode;
  fullDrawer.lightsStyle = TriangleDrawer::LIGHTS_STYLE_NONE;
  fullDrawer.lightDirection = lightVector();
  fullDrawer.textureImageData = &texture;
  if (directEntry) {
    drawWithEntry(fullDrawer, algo, full, triangle);
  }
  else {
    fullDrawer.triangle(full, triangle);
  }

  // Half-open, deliberately asymmetric, not aligned to the 8x8 tiles.
  // The scissored target starts with the same background/depth as the
  // reference; only pixels inside the rect may change, and pixels outside
  // it are seeded with sentinels to prove they are never touched.
  const int x0 = 5, y0 = 7, x1 = 29, y1 = 23;
  ImageData scissored(W, H);
  ZBuffer scissoredDepth(W, H);
  scissored.fill(BG);
  scissoredDepth.fill(std::numeric_limits<double>::infinity());
  for (int y = 0; y < (int)H; ++y) {
    for (int x = 0; x < (int)W; ++x) {
      const bool inside = x >= x0 && x < x1 && y >= y0 && y < y1;
      if (!inside) {
        scissored.setPixel(x, y, SENTINEL_COLOR);
        scissoredDepth.unsafeSet(x, y, SENTINEL_DEPTH);
      }
    }
  }
  TriangleDrawer drawer(&scissoredDepth);
  drawer.algo = (TriangleDrawer::Algo)algo;
  drawer.drawMode = drawMode;
  drawer.lightsStyle = TriangleDrawer::LIGHTS_STYLE_NONE;
  drawer.lightDirection = lightVector();
  drawer.textureImageData = &texture;
  drawer.setScissor(x0, y0, x1, y1);
  if (directEntry) {
    drawWithEntry(drawer, algo, scissored, triangle);
  }
  else {
    drawer.triangle(scissored, triangle);
  }

  std::vector<u32> insideColors;
  for (int y = 0; y < (int)H; ++y) {
    for (int x = 0; x < (int)W; ++x) {
      const bool inside = x >= x0 && x < x1 && y >= y0 && y < y1;
      if (inside) {
        if (scissored.getPixel(x, y) != full.getPixel(x, y)) return false;
        if (!closeDepth(scissoredDepth.unsafeGet(x, y),
                        fullDepth.unsafeGet(x, y))) {
          return false;
        }
        if (std::find(insideColors.begin(), insideColors.end(),
                      scissored.getPixel(x, y)) == insideColors.end()) {
          insideColors.push_back(scissored.getPixel(x, y));
        }
      }
      else {
        if (scissored.getPixel(x, y) != SENTINEL_COLOR) return false;
        if (!std::isinf(scissoredDepth.unsafeGet(x, y))) return false;
      }
    }
  }
  // Make sure the tested region actually received pixels, and for the
  // textured modes that the asymmetric texture was sampled (several distinct
  // colours rather than a flat fill).
  const bool textured = drawMode == TriangleDrawer::DRAW_PERSPECTIVE ||
                        drawMode == TriangleDrawer::DRAW_AFFINE;
  const std::size_t minimumColors = textured ? 4 : 1;
  return insideColors.size() >= minimumColors;
}

bool scissorEmptyRect(int algo)
{
  const unsigned W = 32, H = 24;
  ImageData texture = makeAsymmetricTexture(false);
  Triangle triangle = triABC(2, 2, 2, 20, 29, 2, 1.0, 0);

  ImageData image(W, H);
  ZBuffer depth(W, H);
  image.fill(SENTINEL_COLOR);
  depth.fill(SENTINEL_DEPTH);

  TriangleDrawer drawer(&depth);
  drawer.algo = (TriangleDrawer::Algo)algo;
  drawer.drawMode = TriangleDrawer::DRAW_PERSPECTIVE;
  drawer.lightsStyle = TriangleDrawer::LIGHTS_STYLE_NONE;
  drawer.lightDirection = lightVector();
  drawer.textureImageData = &texture;
  drawer.setScissor(10, 11, 10, 20); // half-open: empty
  drawer.triangle(image, triangle);

  for (u32 y = 0; y < H; ++y) {
    for (u32 x = 0; x < W; ++x) {
      if (image.getPixel(x, y) != SENTINEL_COLOR) return false;
      if (!std::isinf(depth.unsafeGet(x, y))) return false;
    }
  }
  return true;
}

bool scissorToggleRestoresFullDraw(int algo)
{
  const unsigned W = 33, H = 25;
  ImageData texture = makeAsymmetricTexture(false);
  Triangle triangle = triABC(1, 1, 1, 22, 30, 1, 1.4, 0);

  ImageData refImage(W, H);
  ZBuffer refDepth(W, H);
  refImage.fill(BG);
  refDepth.fill(std::numeric_limits<double>::infinity());
  TriangleDrawer ref(&refDepth);
  ref.algo = (TriangleDrawer::Algo)algo;
  ref.drawMode = TriangleDrawer::DRAW_PERSPECTIVE;
  ref.lightsStyle = TriangleDrawer::LIGHTS_STYLE_NONE;
  ref.lightDirection = lightVector();
  ref.textureImageData = &texture;
  ref.triangle(refImage, triangle);

  ImageData image(W, H);
  ZBuffer depth(W, H);
  image.fill(BG);
  depth.fill(std::numeric_limits<double>::infinity());
  TriangleDrawer drawer(&depth);
  drawer.algo = (TriangleDrawer::Algo)algo;
  drawer.drawMode = TriangleDrawer::DRAW_PERSPECTIVE;
  drawer.lightsStyle = TriangleDrawer::LIGHTS_STYLE_NONE;
  drawer.lightDirection = lightVector();
  drawer.textureImageData = &texture;
  drawer.setScissor(4, 4, 8, 8);
  drawer.triangle(image, triangle);
  drawer.clearScissor();
  drawer.triangle(image, triangle);

  return sameImage(refImage, image) && sameDepth(refDepth, depth);
}

bool scissorRectPastEdges(int algo)
{
  const unsigned W = 29, H = 23;
  Triangle triangle = rightTri(3, 3, 20, 15, 2.0, RED);

  ImageData full(W, H);
  ZBuffer fullDepth(W, H);
  full.fill(BG);
  fullDepth.fill(std::numeric_limits<double>::infinity());
  TriangleDrawer fullDrawer(&fullDepth);
  fullDrawer.algo = (TriangleDrawer::Algo)algo;
  fullDrawer.drawMode = TriangleDrawer::DRAW_SOLID;
  fullDrawer.lightsStyle = TriangleDrawer::LIGHTS_STYLE_NONE;
  fullDrawer.triangle(full, triangle);

  // Rect starts left/above the canvas and ends past right/bottom, so the
  // clip must degrade to a full-canvas draw.
  const int x0 = -4, y0 = -3, x1 = 40, y1 = 31;
  ImageData image(W, H);
  ZBuffer depth(W, H);
  image.fill(BG);
  depth.fill(std::numeric_limits<double>::infinity());
  TriangleDrawer drawer(&depth);
  drawer.algo = (TriangleDrawer::Algo)algo;
  drawer.drawMode = TriangleDrawer::DRAW_SOLID;
  drawer.lightsStyle = TriangleDrawer::LIGHTS_STYLE_NONE;
  drawer.setScissor(x0, y0, x1, y1);
  drawer.triangle(image, triangle);

  return sameImage(full, image) && sameDepth(fullDepth, depth);
}

// ---------------------------------------------------------------------------
// locality group
// ---------------------------------------------------------------------------

const unsigned CANVAS_W = 256;
const unsigned CANVAS_H = 256;
const int TILE_COLS = 32;
const int TILE_ROWS = 32;
const int OBJECT_COUNT = TILE_COLS * TILE_ROWS;

struct LocalityScene
{
  RetainedRenderer renderer;
  std::vector<Obj> objects;

  LocalityScene(int algo)
  : renderer(CANVAS_W, CANVAS_H, (TriangleDrawer::Algo)algo, BG)
  , objects()
  {
  }
};

void buildLocalityScene(LocalityScene& scene)
{
  for (int ty = 0; ty < TILE_ROWS; ++ty) {
    for (int tx = 0; tx < TILE_COLS; ++tx) {
      const std::uint64_t id =
        (std::uint64_t)ty * (std::uint64_t)TILE_COLS + (std::uint64_t)tx + 1;
      const float base = (float)(tx * 8);
      const float top = (float)(ty * 8);
      const u32 color = ImageData::makePixel(
        (u8)(20 + tx * 7), (u8)(30 + ty * 7),
        (u8)(40 + (tx * 5 + ty * 11) % 200), 255);
      const double depth = 1.0 + (double)(id % 5) * 0.5;
      std::vector<Triangle> tris;
      tris.push_back(triABC(base + 3, top + 3,
                            base + 3, top + 6,
                            base + 6, top + 3,
                            depth, color));
      RetainedRenderer::Material mat(
        TriangleDrawer::DRAW_SOLID, TriangleDrawer::LIGHTS_STYLE_NONE,
        lightVector());
      scene.renderer.set(id, tris, mat);
      scene.objects.push_back(Obj(id, tris, mat));
    }
  }
}

bool localityBounds(int algo, unsigned* changedTileX, unsigned* changedTileY)
{
  LocalityScene scene(algo);
  buildLocalityScene(scene);
  if ((int)scene.objects.size() != OBJECT_COUNT) return false;

  scene.renderer.flush();
  RetainedRenderer::Stats first = scene.renderer.stats();
  if (first.dirty_tiles != (std::size_t)OBJECT_COUNT) return false;
  if (first.candidate_visits != (std::size_t)OBJECT_COUNT) return false;
  if (first.raster_calls != (std::size_t)OBJECT_COUNT) return false;

  if (!compareScene(scene.renderer, scene.objects, algo, BG).same) return false;

  // Choose one object and move/redraw it inside its own tile, in front of
  // its previous version so the new colour is definitely visible.
  const std::size_t changed = 499;
  const std::uint64_t id = scene.objects[changed].id;
  const int tx = (int)(id - 1) % TILE_COLS;
  const int ty = (int)(id - 1) / TILE_COLS;
  *changedTileX = (unsigned)tx;
  *changedTileY = (unsigned)ty;

  const float base = (float)(tx * 8);
  const float top = (float)(ty * 8);
  std::vector<Triangle> moved;
  moved.push_back(triABC(base + 4, top + 3,
                         base + 4, top + 6,
                         base + 7, top + 3,
                         0.5, ORANGE));
  RetainedRenderer::Material mat(
    TriangleDrawer::DRAW_SOLID, TriangleDrawer::LIGHTS_STYLE_NONE,
    lightVector());
  scene.renderer.set(id, moved, mat);
  scene.objects[changed].tris = moved;
  scene.objects[changed].mat = mat;

  ImageData before = scene.renderer.image();
  scene.renderer.flush();
  RetainedRenderer::Stats local = scene.renderer.stats();

  // The update is confined to a couple of tiles, not the whole scene.
  if (local.dirty_tiles > 4) return false;
  if (local.candidate_visits > 8) return false;
  if (local.raster_calls > 8) return false;
  if (local.candidate_visits >= (std::size_t)OBJECT_COUNT) return false;
  if (local.raster_calls >= (std::size_t)OBJECT_COUNT) return false;

  if (!compareScene(scene.renderer, scene.objects, algo, BG).same) return false;
  if (scene.renderer.image().getPixel((u32)(base + 4), (u32)(top + 3)) !=
      ORANGE) {
    return false;
  }

  // Clean tiles must keep their exact previous bytes.
  for (u32 y = 0; y < CANVAS_H; ++y) {
    for (u32 x = 0; x < CANVAS_W; ++x) {
      const unsigned px = x / 8;
      const unsigned py = y / 8;
      if (px == (unsigned)tx && py == (unsigned)ty) continue;
      if (scene.renderer.image().getPixel(x, y) != before.getPixel(x, y)) {
        return false;
      }
    }
  }

  // Repeated flushes with no updates do no work at all.
  for (int i = 0; i < 2; ++i) {
    ImageData snapshot = scene.renderer.image();
    scene.renderer.flush();
    RetainedRenderer::Stats idle = scene.renderer.stats();
    if (idle.dirty_tiles != 0 || idle.candidate_visits != 0 ||
        idle.raster_calls != 0) {
      return false;
    }
    if (!sameImage(snapshot, scene.renderer.image())) {
      return false;
    }
  }
  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// case registration
// ---------------------------------------------------------------------------

static int runDamageMoveReveals()
{
  for (int algo = 0; algo < 4; ++algo) {
    TASK_CHECK(damageMoveReveals(algo));
  }
  return 0;
}

static int runDamageEraseReveals()
{
  for (int algo = 0; algo < 4; ++algo) {
    TASK_CHECK(damageEraseReveals(algo));
  }
  return 0;
}

static int runDamageEqualDepth()
{
  for (int algo = 0; algo < 4; ++algo) {
    TASK_CHECK(damageEqualDepthReplaceAndReinsert(algo));
  }
  return 0;
}

static int runDamageTextureSnapshot()
{
  for (int algo = 0; algo < 4; ++algo) {
    TASK_CHECK(damageTextureSnapshot(algo));
  }
  return 0;
}

static int runDamageTransparentHoles()
{
  for (int algo = 0; algo < 4; ++algo) {
    TASK_CHECK(damageTransparentHoles(algo));
  }
  return 0;
}

static int runDamageCrossTile()
{
  for (int algo = 0; algo < 4; ++algo) {
    TASK_CHECK(damageCrossTileContinuous(algo));
  }
  return 0;
}

static int runDamageFirstFlush()
{
  for (int algo = 0; algo < 4; ++algo) {
    TASK_CHECK(damageFirstFlushInitializes(algo));
  }
  return 0;
}

static int runDamageCoalesce()
{
  for (int algo = 0; algo < 4; ++algo) {
    TASK_CHECK(damageUpdatesBeforeFlushCoalesce(algo));
  }
  return 0;
}

static int runScissorDispatch()
{
  for (int algo = 0; algo < 4; ++algo) {
    TASK_CHECK(scissorInsideMatches(algo, false,
                                    TriangleDrawer::DRAW_PERSPECTIVE));
    TASK_CHECK(scissorInsideMatches(algo, false, TriangleDrawer::DRAW_AFFINE));
    TASK_CHECK(scissorInsideMatches(algo, false, TriangleDrawer::DRAW_SOLID));
    TASK_CHECK(scissorInsideMatches(algo, false,
                                    TriangleDrawer::DRAW_RGB_SHADED));
    TASK_CHECK(scissorEmptyRect(algo));
    TASK_CHECK(scissorRectPastEdges(algo));
  }
  return 0;
}

static int runScissorDirectEntries()
{
  for (int algo = 0; algo < 4; ++algo) {
    TASK_CHECK(scissorInsideMatches(algo, true,
                                    TriangleDrawer::DRAW_PERSPECTIVE));
    TASK_CHECK(scissorInsideMatches(algo, true, TriangleDrawer::DRAW_AFFINE));
    TASK_CHECK(scissorInsideMatches(algo, true, TriangleDrawer::DRAW_SOLID));
    TASK_CHECK(scissorInsideMatches(algo, true,
                                    TriangleDrawer::DRAW_RGB_SHADED));
  }
  return 0;
}

static int runScissorToggle()
{
  for (int algo = 0; algo < 4; ++algo) {
    TASK_CHECK(scissorToggleRestoresFullDraw(algo));
  }
  return 0;
}

static int runLocalityBounds()
{
  for (int algo = 0; algo < 4; ++algo) {
    unsigned tx = 0, ty = 0;
    TASK_CHECK(localityBounds(algo, &tx, &ty));
    TASK_CHECK(tx < (unsigned)TILE_COLS);
    TASK_CHECK(ty < (unsigned)TILE_ROWS);
  }
  return 0;
}

extern const struct task_case task_cases[] = {
  {"damage", "move_reveals", runDamageMoveReveals},
  {"damage", "erase_reveals", runDamageEraseReveals},
  {"damage", "equal_depth_replace_reinsert", runDamageEqualDepth},
  {"damage", "texture_snapshot", runDamageTextureSnapshot},
  {"damage", "transparent_holes", runDamageTransparentHoles},
  {"damage", "cross_tile_continuous", runDamageCrossTile},
  {"damage", "first_flush_initializes", runDamageFirstFlush},
  {"damage", "updates_before_flush_coalesce", runDamageCoalesce},

  {"scissor", "dispatch_four_algos", runScissorDispatch},
  {"scissor", "direct_entries", runScissorDirectEntries},
  {"scissor", "toggle_restores_full", runScissorToggle},

  {"locality", "thousand_objects_one_update", runLocalityBounds}
};

extern const size_t task_case_count =
  sizeof(task_cases) / sizeof(task_cases[0]);
