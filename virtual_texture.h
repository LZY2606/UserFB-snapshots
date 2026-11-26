#pragma once
// Sparse virtual texturing path.
//
// The caller describes complete logical mip chains but only uploads the pages
// actually needed. A frame is rasterized against an immutable residency
// snapshot; every visible fragment reports the color it sampled together with
// the pages that would have to be uploaded for ideal-level sampling.
#include "geometry.h"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace vt {

// Logical description of one texture: a power-of-two base size halved on each
// axis until 1x1, and a fixed power-of-two page edge.
struct TextureDesc {
    int texture_id  = 0;
    int base_width  = 0;
    int base_height = 0;
    int page_size   = 0;
};

// Identity of a single page. Level 0 is the base level; the last level is 1x1.
struct PageKey {
    int texture_id = 0;
    int level      = 0;
    int page_x     = 0;
    int page_y     = 0;
    bool operator==(const PageKey &other) const {
        return texture_id == other.texture_id && level == other.level &&
               page_x == other.page_x && page_y == other.page_y;
    }
};

// Order required for the missing-page feedback:
// texture id, level, page y, page x.
bool page_key_less(const PageKey &a, const PageKey &b);

// A page upload: the actual extents of the page in texels (tail levels can be
// smaller than a full page) followed by width*height*3 doubles in row-major
// order, linear RGB.
struct PageUpload {
    PageKey key;
    int width  = 0;
    int height = 0;
    std::vector<double> rgb;
};

// Immutable residency snapshot. Taking a snapshot after upload/replace/evict
// yields an owning copy; snapshots and frames keep their contents forever,
// independently of later pool updates.
class ResidentSnapshot {
public:
    virtual ~ResidentSnapshot() = default;

    virtual const std::vector<TextureDesc>& textures() const = 0;
    virtual bool contains(const PageKey &key) const = 0;
    virtual std::size_t page_count() const = 0;
    virtual std::vector<PageKey> pages() const = 0; // sorted with page_key_less
};

// The pool owns the sparse pages of a fixed set of logical textures. Only
// uploaded pages allocate pixel storage; the full logical images are never
// allocated. The 1x1 page of the last level is pinned and cannot be evicted.
class VirtualTexturePool {
public:
    explicit VirtualTexturePool(std::vector<TextureDesc> textures);

    std::size_t texture_count() const;
    const TextureDesc& desc(int texture_id) const;
    int level_count(int texture_id) const;       // number of mip levels
    int last_level(int texture_id) const;
    int level_width(int texture_id, int level) const;
    int level_height(int texture_id, int level) const;
    int pages_x(int texture_id, int level) const;
    int pages_y(int texture_id, int level) const;
    int page_width(int texture_id, int level, int page_x) const;  // actual extent
    int page_height(int texture_id, int level, int page_y) const;

    bool resident(const PageKey &key) const;
    void upload(const PageUpload &page);         // invalid input throws before any mutation; re-upload replaces
    void evict(const PageKey &key);              // missing/pinned/invalid keys throw
    std::shared_ptr<const ResidentSnapshot> snapshot() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// Everything a frame found for one pixel. Background pixels stay zero-initialized
// with face == -1.
struct FragmentInfo {
    double r = 0., g = 0., b = 0.;   // linear RGB
    int    face = -1;                // winning submission index, -1 on background
    double depth = 0.;               // interpolated NDC depth (larger = closer)
    double u = 0., v = 0.;           // perspective-correct interpolated UV
    double dudx = 0., dvdx = 0.;     // analytical derivatives of the UV quotient
    double dudy = 0., dvdy = 0.;
    double lod = 0.;                 // clamped fractional LOD
    int    level0 = 0, level1 = -1;  // ideal dwords (-1 == weight-zero dword)
    int    used_level0 = 0, used_level1 = -1; // levels actually sampled after fallback
    double lod_frac = 0.;            // fractional weight between the two dwords
};

// A frame owns its submitted triangles, the viewport and the snapshot it was
// bound to. Results do not reference the caller's arrays or any mutable global
// state, and re-parsing a finished frame always gives the same answer.
class VirtualTextureFrame {
public:
    VirtualTextureFrame(int width, int height, const mat<4,4> &viewport,
                        std::shared_ptr<const ResidentSnapshot> snapshot);
    ~VirtualTextureFrame();

    int width() const;
    int height() const;

    // Clip-space triangle with one UV and one texture id per corner.
    // Finite coordinates, positive w and an in-frustum NDC position are required.
    void submit_triangle(int texture_id, bool discard,
                         const vec4 clip[3], const vec2 uv[3]);

    // Rasterize every submission and produce the results. Idempotent: calling
    // it more than once has no effect and never changes the first result.
    void generate();

    const FragmentInfo& fragment(int x, int y) const; // valid after generate()
    const std::vector<PageKey>& missing_pages() const; // sorted and de-duplicated

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vt
