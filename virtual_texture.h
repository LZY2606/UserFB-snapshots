#pragma once
// Sparse virtual texturing path.
//
// The caller declares a full logical mip chain per texture but only ever
// uploads individual fixed-size pages; the full logical images are never
// allocated. An immutable, owning ResidencySnapshot is taken after every
// mutation; frames bind one snapshot and keep working against it regardless
// of later pool updates.
#include <cstdint>
#include <map>
#include <memory>
#include <vector>
#include "geometry.h"

namespace vtex {

// Linear-space RGB sample, double precision.
struct RGBd {
    double r = 0.;
    double g = 0.;
    double b = 0.;
};

// Identity of one page: texture number, mip level and page coordinates in the
// page grid of that level. Ordering matches the required feedback ordering
// (texture, level, page y, page x).
struct PageKey {
    int texture = 0;
    int level   = 0;
    int page_x  = 0;
    int page_y  = 0;

    bool operator==(const PageKey &o) const {
        return texture==o.texture && level==o.level && page_x==o.page_x && page_y==o.page_y;
    }
    bool operator<(const PageKey &o) const {
        if (texture != o.texture) return texture < o.texture;
        if (level   != o.level)   return level   < o.level;
        if (page_y  != o.page_y)  return page_y  < o.page_y;
        return page_x < o.page_x;
    }
};

// Logical description of a texture. The base dimensions and the (square) page
// edge are powers of two. Each mip level halves every axis independently until
// the level becomes 1x1; tail levels may be smaller than a page.
struct TextureDesc {
    int base_width  = 0;
    int base_height = 0;
    int page_size   = 0;
};

// Row-major linear RGB pixels covering the actual (possibly tail-clipped)
// extent of one page.
struct PageData {
    int width  = 0;
    int height = 0;
    std::vector<RGBd> pixels; // width*height entries, tightly packed rows
};

// Logical mip-chain geometry. Level 0 is the base size; every level halves
// each axis independently until the level is 1x1.
int  level_count(const TextureDesc &desc);
void level_size(const TextureDesc &desc, int level, int &out_w, int &out_h);
int  page_count_x(const TextureDesc &desc, int level);
int  page_count_y(const TextureDesc &desc, int level);
// Actual pixel extent of a page (tail pages may be smaller than the page edge).
void page_extent(const TextureDesc &desc, int level, int px, int py, int &out_w, int &out_h);

class SparseTexturePool;

// Immutable, owning residency state. Every lookup a frame does goes through
// the snapshot it bound; updating or evicting pages in the pool afterwards
// never reaches existing snapshots or frames.
class ResidencySnapshot {
public:
    ResidencySnapshot() = default;

    bool has_texture(int texture) const { return descs.count(texture) != 0; }
    const TextureDesc& desc(int texture) const { return descs.at(texture); }

    bool resident(const PageKey &key) const { return pages.find(key) != pages.end(); }
    bool resident(int texture, int level, int px, int py) const {
        return resident(PageKey{texture, level, px, py});
    }
    // nullptr when the page is not resident in this snapshot.
    const PageData* find(const PageKey &key) const {
        auto it = pages.find(key);
        return it == pages.end() ? nullptr : &it->second;
    }
    const PageData* find(int texture, int level, int px, int py) const {
        return find(PageKey{texture, level, px, py});
    }
    const std::vector<PageKey>& page_keys() const { return keys; }

private:
    friend class SparseTexturePool;
    std::map<int, TextureDesc> descs;
    std::map<PageKey, PageData> pages;
    std::vector<PageKey>        keys;
};

// Holds the logical texture descriptions and the pages that have actually been
// uploaded. No full logical image is ever allocated. All mutators validate
// their complete input before touching any state and throw std::invalid_argument
// on rejection.
class SparseTexturePool {
public:
    SparseTexturePool() = default;

    void declare_texture(int texture, const TextureDesc &desc);
    int  texture_count() const { return static_cast<int>(descs.size()); }
    bool has_texture(int texture) const { return descs.count(texture) != 0; }
    const TextureDesc& desc(int texture) const;

    int  level_count(int texture) const;
    void level_size(int texture, int level, int &out_w, int &out_h) const;
    int  page_count_x(int texture, int level) const;
    int  page_count_y(int texture, int level) const;
    // Actual pixel extent of a page in a level (tail pages may be smaller).
    void page_extent(int texture, int level, int px, int py, int &out_w, int &out_h) const;

    // Upload a new page, overwrite an existing one, or remove one. The 1x1
    // last mip level is pinned and cannot be evicted.
    void upload_page(int texture, int level, int px, int py, const PageData &data);
    void replace_page(int texture, int level, int px, int py, const PageData &data);
    void evict_page(int texture, int level, int px, int py);
    bool is_resident(int texture, int level, int px, int py) const;

    // Fresh owning snapshot of the current residency state.
    std::shared_ptr<const ResidencySnapshot> snapshot() const;

private:
    void validate_page_write(int texture, int level, int px, int py, const PageData &data) const;
    std::map<int, TextureDesc>      descs;
    std::map<PageKey, PageData>     pages;
};

// Everything a resolved frame reports for one pixel.
struct FragmentResult {
    RGBd   color  {};   // sampled linear RGB; black for background pixels
    int    face   = -1; // submission index of the winning triangle; -1 on background
    double depth  = 0.; // NDC z of the winning fragment
    vec2   uv     {};   // perspective-interpolated uv
    vec2   duv_dx {};   // analytic screen-space derivatives of the interpolated quotient
    vec2   duv_dy {};
    double lod    = 0.; // clamped floating-point LOD
    int    lod_lo = -1; // mip level actually sampled by the coarse band (-1 if none)
    int    lod_hi = -1; // mip level actually sampled by the fine band (-1 if skipped)
};

// One frame's worth of submitted triangles, resolved against a fixed residency
// snapshot and viewport. The frame owns all of its input and output; it never
// references the caller's triangle arrays, the pool, or the global viewport.
class VirtualTextureFrame {
public:
    VirtualTextureFrame(int width, int height,
                        std::shared_ptr<const ResidencySnapshot> residency,
                        const mat<4,4> &viewport);

    int width()  const { return w; }
    int height() const { return h; }

    // Append a triangle. Input must be finite, have positive w and lie inside
    // the clip frustum; otherwise std::invalid_argument is thrown before the
    // frame is modified. `discard` fragments pass the depth test but write
    // neither color nor depth.
    void submit(const vec4 clip[3], const vec2 uv[3], int texture, bool discard = false);

    // Run rasterization, sampling and feedback collection. Idempotent: calling
    // it again produces identical results.
    void resolve();

    bool resolved() const { return is_resolved; }
    const FragmentResult& fragment(int x, int y) const;
    const std::vector<PageKey>& missing_pages() const;
    const ResidencySnapshot& residency() const { return *snap; }

private:
    struct StoredTriangle {
        vec4 clip[3];
        vec2 uv[3];
        int  texture = 0;
        bool discard = false;
    };
    struct Winner {
        bool valid = false;
        int  face  = -1;
        int  texture = 0;
        double depth = 0.;
        vec2   uv{}, dudx{}, dudy{};
        double lod = 0.;
    };

    int  w = 0, h = 0;
    std::shared_ptr<const ResidencySnapshot> snap;
    mat<4,4> viewport;
    std::vector<StoredTriangle> tris;
    std::vector<Winner>         winners;
    std::vector<double>         zbuffer;
    std::vector<FragmentResult> fragments;
    std::vector<PageKey>        feedback;
    bool is_resolved = false;
};

} // namespace vtex
