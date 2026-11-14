#ifndef ANDREW_LIM_GRAPHICS_RETAINED_RENDERER_H
#define ANDREW_LIM_GRAPHICS_RETAINED_RENDERER_H

#include "TriangleDrawer.h"
#include "Triangle.h"
#include "ImageData.h"
#include "Grid.h"
#include "Vector3f.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace al { namespace graphics {

// A retained, tile based scene renderer.
//
// Objects are kept in insertion order. set(), erase() and the first flush
// all feed the same damage path: the union of an object's previous and new
// tile coverage is queued, and flush() clears and replays only those tiles.
// The actual pixel filling is still performed by TriangleDrawer, restricted
// with its half-open scissor rectangle.
class RetainedRenderer
{
public:
  struct Material
  {
    TriangleDrawer::DrawMode drawMode;
    TriangleDrawer::LightsStyle lightsStyle;
    Vector3f lightDirection;
    // Deep copied by set(). A texture is present when it has a non-zero
    // width and height.
    ImageData texture;

    Material()
    : drawMode(TriangleDrawer::DRAW_SOLID)
    , lightsStyle(TriangleDrawer::LIGHTS_STYLE_NONE)
    , lightDirection(G3D::normalize(Vector3f(1.0f, 0.0f, 1.0f)))
    , texture()
    {
    }

    Material(TriangleDrawer::DrawMode mode,
             TriangleDrawer::LightsStyle style)
    : drawMode(mode)
    , lightsStyle(style)
    , lightDirection(G3D::normalize(Vector3f(1.0f, 0.0f, 1.0f)))
    , texture()
    {
    }

    Material(TriangleDrawer::DrawMode mode,
             TriangleDrawer::LightsStyle style,
             const Vector3f& direction)
    : drawMode(mode)
    , lightsStyle(style)
    , lightDirection(direction)
    , texture()
    {
    }

    Material(TriangleDrawer::DrawMode mode,
             TriangleDrawer::LightsStyle style,
             const Vector3f& direction,
             const ImageData& image)
    : drawMode(mode)
    , lightsStyle(style)
    , lightDirection(direction)
    , texture(image)
    {
    }

    // Convenience for callers that hold an optional texture pointer. The
    // texture is deep copied, so the pointer need not outlive this object.
    Material(TriangleDrawer::DrawMode mode,
             TriangleDrawer::LightsStyle style,
             const Vector3f& direction,
             const ImageData* image)
    : drawMode(mode)
    , lightsStyle(style)
    , lightDirection(direction)
    , texture()
    {
      if (image) {
        texture = *image;
      }
    }

    bool hasTexture() const
    {
      return texture.width() > 0 && texture.height() > 0;
    }

    Material& setTexture(const ImageData& image)
    {
      texture = image;
      return *this;
    }

    void clearTexture()
    {
      texture = ImageData();
    }
  }; // struct Material

  struct Stats
  {
    std::size_t dirty_tiles;
    std::size_t candidate_visits;
    std::size_t raster_calls;

    Stats()
    : dirty_tiles(0)
    , candidate_visits(0)
    , raster_calls(0)
    {
    }
  }; // struct Stats

  RetainedRenderer(unsigned width, unsigned height,
                   TriangleDrawer::Algo algo, u32 background);

  // Adds or replaces an object. The triangles and the material (including
  // its texture) are copied. A replacement keeps the object's original draw
  // order; a new id is appended after all existing objects.
  void set(std::uint64_t id, const std::vector<Triangle>& triangles,
           const Material& material);

  // Removes an object. Returns true when the id existed.
  bool erase(std::uint64_t id);

  // Repaints all damaged tiles (and, on the first call, the whole canvas).
  void flush();

  const ImageData& image() const { return image_; }
  const ZBuffer& depth() const { return depth_; }
  const Stats& stats() const { return stats_; }

  unsigned width() const { return width_; }
  unsigned height() const { return height_; }
  TriangleDrawer::Algo algo() const { return algo_; }

private:
  struct TriRecord
  {
    Triangle triangle;
    // Inclusive tile bounds. An empty triangle has x1 < x0.
    int tileX0;
    int tileY0;
    int tileX1;
    int tileY1;

    TriRecord()
    : triangle(), tileX0(0), tileY0(0), tileX1(-1), tileY1(-1)
    {
    }
  };

  struct ObjectRecord
  {
    std::uint64_t id;
    std::uint64_t order;
    std::vector<TriRecord> triangles;
    Material material;
    std::vector<std::size_t> tiles;

    ObjectRecord()
    : id(0), order(0), triangles(), material(), tiles()
    {
    }
  };

  struct Candidate
  {
    std::uint64_t order;
    std::uint64_t id;
    std::size_t triIndex;

    Candidate() : order(0), id(0), triIndex(0) {}
    Candidate(std::uint64_t order_, std::uint64_t id_, std::size_t triIndex_)
    : order(order_), id(id_), triIndex(triIndex_)
    {
    }

    bool operator<(const Candidate& other) const
    {
      if (order != other.order) {
        return order < other.order;
      }
      return triIndex < other.triIndex;
    }
  };

  void markDirty(std::size_t tile);
  void addCandidate(std::size_t tile, std::uint64_t order,
                    std::uint64_t id, std::size_t triIndex);
  void removeCandidate(std::size_t tile, std::uint64_t id,
                       std::size_t triIndex);

  TriRecord makeTriRecord(const Triangle& triangle) const;
  void clearTile(std::size_t tile);
  void configureDrawer(Material& material);

  unsigned width_;
  unsigned height_;
  TriangleDrawer::Algo algo_;
  u32 background_;

  ImageData image_;
  ZBuffer depth_;
  TriangleDrawer drawer_;

  int tilesX_;
  int tilesY_;
  std::size_t tileCount_;

  std::vector<std::vector<Candidate> > tileCandidates_;
  std::vector<unsigned char> tileDirty_;
  std::vector<std::size_t> dirtyTiles_;

  std::map<std::uint64_t, ObjectRecord> objects_;
  std::uint64_t nextOrder_;

  ImageData blankTexture_;

  Stats stats_;
}; // class RetainedRenderer

} // namespace al
} // namespace graphics

#endif
