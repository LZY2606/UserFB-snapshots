#include "RetainedRenderer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

using namespace al::graphics;
using namespace std;

namespace {

const int TILE_SIZE = 8;

int tileOf(int coordinate)
{
  // floor division by TILE_SIZE that also works for negative coordinates.
  if (coordinate >= 0) {
    return coordinate / TILE_SIZE;
  }
  return -(((-coordinate) + TILE_SIZE - 1) / TILE_SIZE);
}

int clampInt(int value, int low, int high)
{
  if (value < low) {
    return low;
  }
  if (value > high) {
    return high;
  }
  return value;
}

// Converts to int without ever invoking undefined behaviour on huge or
// infinite inputs. The result is only used to pick tile buckets, so a
// saturated value is harmless.
int safeFloor(double value)
{
  if (value <= -2147483000.0) {
    return -2147483000;
  }
  if (value >= 2147483000.0) {
    return 2147483000;
  }
  return (int)std::floor(value);
}

int safeCeil(double value)
{
  if (value <= -2147483000.0) {
    return -2147483000;
  }
  if (value >= 2147483000.0) {
    return 2147483000;
  }
  return (int)std::ceil(value);
}

} // namespace

RetainedRenderer::RetainedRenderer(unsigned width, unsigned height,
                                   TriangleDrawer::Algo algo, u32 background)
: width_(width)
, height_(height)
, algo_(algo)
, background_(background)
, image_()
, depth_((size_t)width, (size_t)height)
, drawer_(&depth_)
, tilesX_(0)
, tilesY_(0)
, tileCount_(0)
, tileCandidates_()
, tileDirty_()
, dirtyTiles_()
, objects_()
, nextOrder_(0)
, blankTexture_()
, stats_()
{
  if (width < 16 || height < 16) {
    throw std::invalid_argument("RetainedRenderer: canvas must be at least 16x16");
  }

  image_.create(width_, height_);
  image_.fill(background_);

  depth_.fill(std::numeric_limits<double>::infinity());

  drawer_.algo = algo_;
  drawer_.depthBuffer = &depth_;
  drawer_.textureImageData = 0;

  // A 1x1 transparent texture is used when a texturing draw mode has no
  // texture supplied, so such triangles simply contribute nothing instead
  // of dereferencing a null image.
  blankTexture_.create(1, 1);
  blankTexture_.fill(ImageData::makePixel(0, 0, 0, 0));

  tilesX_ = (int)((width_ + TILE_SIZE - 1) / TILE_SIZE);
  tilesY_ = (int)((height_ + TILE_SIZE - 1) / TILE_SIZE);
  tileCount_ = (std::size_t)tilesX_ * (std::size_t)tilesY_;

  tileCandidates_.resize(tileCount_);
  tileDirty_.assign(tileCount_, 0);
  dirtyTiles_.reserve(tileCount_);

  // The first flush must initialize the whole canvas.
  for (std::size_t tile = 0; tile < tileCount_; ++tile) {
    markDirty(tile);
  }
}

void RetainedRenderer::markDirty(std::size_t tile)
{
  if (tile >= tileCount_ || tileDirty_[tile]) {
    return;
  }
  tileDirty_[tile] = 1;
  dirtyTiles_.push_back(tile);
}

void RetainedRenderer::addCandidate(std::size_t tile, std::uint64_t order,
                                    std::uint64_t id, std::size_t triIndex)
{
  std::vector<Candidate>& list = tileCandidates_[tile];
  Candidate candidate(order, id, triIndex);
  std::vector<Candidate>::iterator it =
    std::lower_bound(list.begin(), list.end(), candidate);
  list.insert(it, candidate);
}

void RetainedRenderer::removeCandidate(std::size_t tile, std::uint64_t id,
                                       std::size_t triIndex)
{
  std::vector<Candidate>& list = tileCandidates_[tile];
  for (std::vector<Candidate>::iterator it = list.begin();
       it != list.end(); ++it) {
    if (it->id == id && it->triIndex == triIndex) {
      list.erase(it);
      return;
    }
  }
}

RetainedRenderer::TriRecord
RetainedRenderer::makeTriRecord(const Triangle& triangle) const
{
  TriRecord record;
  record.triangle = triangle;

  const float x0 = triangle.vertices[0].x();
  const float y0 = triangle.vertices[0].y();
  const float x1 = triangle.vertices[1].x();
  const float y1 = triangle.vertices[1].y();
  const float x2 = triangle.vertices[2].x();
  const float y2 = triangle.vertices[2].y();

  const float minXf = std::min(x0, std::min(x1, x2));
  const float maxXf = std::max(x0, std::max(x1, x2));
  const float minYf = std::min(y0, std::min(y1, y2));
  const float maxYf = std::max(y0, std::max(y1, y2));

  if (!std::isfinite(minXf) || !std::isfinite(maxXf) ||
      !std::isfinite(minYf) || !std::isfinite(maxYf)) {
    // Cannot reason about coverage; treat as empty rather than going wild.
    record.tileX0 = 0;
    record.tileY0 = 0;
    record.tileX1 = -1;
    record.tileY1 = -1;
    return record;
  }

  // The three barycentric paths scan the pixel box floor(min) .. ceil(max)-1,
  // and the scanline path truncates edges to int and clamps to the canvas.
  // floor(min)..ceil(max) therefore contains every pixel any Algo can touch.
  const int loX = safeFloor((double)minXf);
  const int hiX = safeCeil((double)maxXf);
  const int loY = safeFloor((double)minYf);
  const int hiY = safeCeil((double)maxYf);

  if (hiX < 0 || loX > (int)width_ - 1 ||
      hiY < 0 || loY > (int)height_ - 1) {
    record.tileX0 = 0;
    record.tileY0 = 0;
    record.tileX1 = -1;
    record.tileY1 = -1;
    return record;
  }

  // Conservative: every pixel the rasterizer can touch lies inside this
  // range, so the tiles containing it are a superset of the triangle's tile
  // membership. Clamp first so a triangle extending past an edge still owns
  // the edge tiles.
  record.tileX0 = clampInt(tileOf(loX), 0, tilesX_ - 1);
  record.tileY0 = clampInt(tileOf(loY), 0, tilesY_ - 1);
  record.tileX1 = clampInt(tileOf(hiX), 0, tilesX_ - 1);
  record.tileY1 = clampInt(tileOf(hiY), 0, tilesY_ - 1);
  return record;
}

void RetainedRenderer::set(std::uint64_t id,
                           const std::vector<Triangle>& triangles,
                           const Material& material)
{
  std::map<std::uint64_t, ObjectRecord>::iterator existing =
    objects_.find(id);

  std::uint64_t order = 0;
  if (existing != objects_.end()) {
    // Replacement keeps the object's original draw order.
    order = existing->second.order;

    // Drop the old candidate entries from the reverse tile index and mark
    // every image the old version could have touched as damaged.
    for (std::size_t i = 0; i < existing->second.tiles.size(); ++i) {
      markDirty(existing->second.tiles[i]);
    }
    for (std::size_t tri = 0; tri < existing->second.triangles.size(); ++tri) {
      const TriRecord& oldRecord = existing->second.triangles[tri];
      for (int ty = oldRecord.tileY0; ty <= oldRecord.tileY1; ++ty) {
        for (int tx = oldRecord.tileX0; tx <= oldRecord.tileX1; ++tx) {
          const std::size_t tile = (std::size_t)ty * tilesX_ + tx;
          removeCandidate(tile, id, tri);
        }
      }
    }
  }
  else {
    order = nextOrder_++;
  }

  ObjectRecord& object = objects_[id];
  object.id = id;
  object.order = order;
  object.material = material;
  object.triangles.clear();
  object.tiles.clear();
  object.triangles.reserve(triangles.size());

  for (std::size_t tri = 0; tri < triangles.size(); ++tri) {
    object.triangles.push_back(makeTriRecord(triangles[tri]));
  }

  // Rebuild this object's reverse tile membership.
  for (std::size_t tri = 0; tri < object.triangles.size(); ++tri) {
    const TriRecord& record = object.triangles[tri];
    for (int ty = record.tileY0; ty <= record.tileY1; ++ty) {
      for (int tx = record.tileX0; tx <= record.tileX1; ++tx) {
        const std::size_t tile = (std::size_t)ty * tilesX_ + tx;
        addCandidate(tile, order, id, tri);
        object.tiles.push_back(tile);
      }
    }
  }
  std::sort(object.tiles.begin(), object.tiles.end());
  object.tiles.erase(std::unique(object.tiles.begin(), object.tiles.end()),
                     object.tiles.end());

  for (std::size_t i = 0; i < object.tiles.size(); ++i) {
    markDirty(object.tiles[i]);
  }
}

bool RetainedRenderer::erase(std::uint64_t id)
{
  std::map<std::uint64_t, ObjectRecord>::iterator existing =
    objects_.find(id);
  if (existing == objects_.end()) {
    return false;
  }

  const ObjectRecord& object = existing->second;
  for (std::size_t i = 0; i < object.tiles.size(); ++i) {
    markDirty(object.tiles[i]);
  }
  for (std::size_t tri = 0; tri < object.triangles.size(); ++tri) {
    const TriRecord& record = object.triangles[tri];
    for (int ty = record.tileY0; ty <= record.tileY1; ++ty) {
      for (int tx = record.tileX0; tx <= record.tileX1; ++tx) {
        const std::size_t tile = (std::size_t)ty * tilesX_ + tx;
        removeCandidate(tile, id, tri);
      }
    }
  }

  objects_.erase(existing);
  return true;
}

void RetainedRenderer::configureDrawer(Material& material)
{
  drawer_.algo = algo_;
  drawer_.depthBuffer = &depth_;
  drawer_.drawMode = material.drawMode;
  drawer_.lightsStyle = material.lightsStyle;
  drawer_.lightDirection = material.lightDirection;
  if (material.hasTexture()) {
    drawer_.textureImageData = &material.texture;
  }
  else {
    drawer_.textureImageData = &blankTexture_;
  }
}

void RetainedRenderer::clearTile(std::size_t tile)
{
  const int tileX = (int)(tile % (std::size_t)tilesX_);
  const int tileY = (int)(tile / (std::size_t)tilesX_);
  const int x0 = tileX * TILE_SIZE;
  const int y0 = tileY * TILE_SIZE;
  const int x1 = std::min((int)width_, x0 + TILE_SIZE);
  const int y1 = std::min((int)height_, y0 + TILE_SIZE);
  const double infinity = std::numeric_limits<double>::infinity();

  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      image_.setPixel(x, y, background_);
      depth_.unsafeSet(x, y, infinity);
    }
  }
}

void RetainedRenderer::flush()
{
  stats_ = Stats();

  const std::size_t dirtyCount = dirtyTiles_.size();

  for (std::size_t i = 0; i < dirtyCount; ++i) {
    const std::size_t tile = dirtyTiles_[i];
    const int tileX = (int)(tile % (std::size_t)tilesX_);
    const int tileY = (int)(tile / (std::size_t)tilesX_);
    const int x0 = tileX * TILE_SIZE;
    const int y0 = tileY * TILE_SIZE;
    const int x1 = std::min((int)width_, x0 + TILE_SIZE);
    const int y1 = std::min((int)height_, y0 + TILE_SIZE);

    clearTile(tile);
    ++stats_.dirty_tiles;

    drawer_.setScissor(x0, y0, x1, y1);

    std::vector<Candidate>& candidates = tileCandidates_[tile];
    for (std::size_t c = 0; c < candidates.size(); ++c) {
      const Candidate& candidate = candidates[c];
      ++stats_.candidate_visits;

      std::map<std::uint64_t, ObjectRecord>::iterator object =
        objects_.find(candidate.id);
      if (object == objects_.end()) {
        continue;
      }
      if (candidate.triIndex >= object->second.triangles.size()) {
        continue;
      }

      configureDrawer(object->second.material);

      const TriRecord& record = object->second.triangles[candidate.triIndex];
      if (record.tileX1 < record.tileX0 || record.tileY1 < record.tileY0) {
        continue;
      }

      ++stats_.raster_calls;
      drawer_.triangle(image_, record.triangle);
    }
  }

  for (std::size_t i = 0; i < dirtyCount; ++i) {
    tileDirty_[dirtyTiles_[i]] = 0;
  }
  dirtyTiles_.clear();
}
