#include "virtual_texture.h"
#include "our_gl.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace vt {

namespace {

bool is_pow2(int x) { return x > 0 && (x & (x - 1)) == 0; }

int period_mod(int i, int n) {
    int r = i % n;
    if (r < 0) r += n;
    return r;
}

struct PageKeyCmp {
    bool operator()(const PageKey &a, const PageKey &b) const {
        return page_key_less(a, b);
    }
};

struct StoredPage {
    int width  = 0;
    int height = 0;
    std::vector<double> rgb; // width*height*3 doubles, row-major linear RGB
};

// Level geometry of one logical texture.
struct LevelInfo {
    int width  = 0;
    int height = 0;
    int pages_x = 0;
    int pages_y = 0;
};

struct TextureInfo {
    TextureDesc desc;
    std::vector<LevelInfo> levels;
};

std::vector<LevelInfo> build_levels(const TextureDesc &d) {
    std::vector<LevelInfo> out;
    int W = d.base_width, H = d.base_height;
    while (true) {
        LevelInfo li;
        li.width = W;
        li.height = H;
        li.pages_x = (W + d.page_size - 1) / d.page_size;
        li.pages_y = (H + d.page_size - 1) / d.page_size;
        out.push_back(li);
        if (W == 1 && H == 1) break;
        W = std::max(1, W / 2);
        H = std::max(1, H / 2);
    }
    return out;
}

class SnapshotImpl : public ResidentSnapshot {
public:
    std::vector<TextureDesc> descs;
    std::map<int, TextureInfo> infos;
    std::map<PageKey, StoredPage, PageKeyCmp> page_map;

    const std::vector<TextureDesc>& textures() const override { return descs; }

    bool contains(const PageKey &key) const override {
        return page_map.find(key) != page_map.end();
    }

    std::size_t page_count() const override { return page_map.size(); }

    std::vector<PageKey> pages() const override {
        std::vector<PageKey> out;
        out.reserve(page_map.size());
        for (const auto &[key, page] : page_map) out.push_back(key);
        return out;
    }

    const StoredPage* find(const PageKey &key) const {
        auto it = page_map.find(key);
        return it == page_map.end() ? nullptr : &it->second;
    }
};

struct Tap {
    int ix = 0, iy = 0;   // periodic texel indices (modulo applied at fetch time)
    double weight = 0.;
};

// The four (or fewer, zero-weight taps are dropped) bilinear taps of a level,
// recomputed on the original UV every time.
int level_taps(int W, int H, double u, double v, Tap taps[4]) {
    const double gx = u * W - 0.5;
    const double gy = v * H - 0.5;
    const int x0 = static_cast<int>(std::floor(gx));
    const int y0 = static_cast<int>(std::floor(gy));
    const double fx = gx - x0;
    const double fy = gy - y0;
    const double wx[2] = {1.0 - fx, fx};
    const double wy[2] = {1.0 - fy, fy};
    int n = 0;
    for (int oy = 0; oy < 2; ++oy) {
        if (!(wy[oy] > 0.0)) continue;
        for (int ox = 0; ox < 2; ++ox) {
            if (!(wx[ox] > 0.0)) continue;
            taps[n++] = {x0 + ox, y0 + oy, wx[ox] * wy[oy]};
        }
    }
    return n;
}

PageKey tap_page_key(const Tap &tap, int texture_id, int level,
                     int page_size, int W, int H) {
    // Periodic modulo first; never clamp at the page edge.
    const int ix = period_mod(tap.ix, W);
    const int iy = period_mod(tap.iy, H);
    return {texture_id, level, ix / page_size, iy / page_size};
}

// Bilinear fetch; every page behind the given taps must already be resident.
vec3 fetch_bilinear(const SnapshotImpl &snap, const TextureInfo &info,
                    int level, double u, double v) {
    const LevelInfo &li = info.levels[level];
    Tap taps[4];
    const int n = level_taps(li.width, li.height, u, v, taps);
    vec3 color{0., 0., 0.};
    for (int t = 0; t < n; ++t) {
        const int ix = period_mod(taps[t].ix, li.width);
        const int iy = period_mod(taps[t].iy, li.height);
        PageKey key{info.desc.texture_id, level,
                    ix / info.desc.page_size, iy / info.desc.page_size};
        const StoredPage *page = snap.find(key);
        const int lx = ix % info.desc.page_size;
        const int ly = iy % info.desc.page_size;
        const double *px = page->rgb.data() + (ly * page->width + lx) * 3;
        color = color + vec3{px[0], px[1], px[2]} * taps[t].weight;
    }
    return color;
}

struct DwordSample {
    vec3 color{0., 0., 0.};
    int used_level = 0;
    std::vector<PageKey> missing; // ideal-level missing pages only
};

// Resolve one LOD dword: check the ideal level, then walk to coarser levels on
// the same UV until every tap is resident.
DwordSample resolve_dword(const SnapshotImpl &snap, const TextureInfo &info,
                          int ideal_level, double u, double v) {
    DwordSample result;
    const LevelInfo &ideal = info.levels[ideal_level];
    Tap ideal_taps[4];
    const int n = level_taps(ideal.width, ideal.height, u, v, ideal_taps);

    std::set<PageKey, PageKeyCmp> needed;
    for (int t = 0; t < n; ++t) {
        needed.insert(tap_page_key(ideal_taps[t], info.desc.texture_id,
                                   ideal_level, info.desc.page_size,
                                   ideal.width, ideal.height));
    }
    for (const PageKey &key : needed) {
        if (!snap.contains(key)) result.missing.push_back(key);
    }

    if (result.missing.empty()) {
        result.used_level = ideal_level;
        result.color = fetch_bilinear(snap, info, ideal_level, u, v);
        return result;
    }

    // Fallback: re-derive the four taps on the original UV at every level.
    for (int lev = ideal_level + 1;
         lev < static_cast<int>(info.levels.size()); ++lev) {
        const LevelInfo &li = info.levels[lev];
        Tap taps[4];
        const int tn = level_taps(li.width, li.height, u, v, taps);
        bool all_resident = true;
        for (int t = 0; t < tn; ++t) {
            PageKey key = tap_page_key(taps[t], info.desc.texture_id, lev,
                                       info.desc.page_size, li.width, li.height);
            if (!snap.contains(key)) { all_resident = false; break; }
        }
        if (all_resident) {
            result.used_level = lev;
            result.color = fetch_bilinear(snap, info, lev, u, v);
            return result;
        }
    }
    return result; // unreachable: the pinned 1x1 last level is always resident
}

struct StoredTriangle {
    int texture_id = 0;
    bool discard = false;
    vec4 clip[3]{};
    vec2 uv[3]{};
    int face = -1;
};

} // namespace

bool page_key_less(const PageKey &a, const PageKey &b) {
    if (a.texture_id != b.texture_id) return a.texture_id < b.texture_id;
    if (a.level      != b.level)      return a.level      < b.level;
    if (a.page_y     != b.page_y)     return a.page_y     < b.page_y;
    return a.page_x < b.page_x;
}

// ---------------------------------------------------------------------------
// Pool
// ---------------------------------------------------------------------------

struct VirtualTexturePool::Impl {
    std::map<int, TextureInfo> textures;
    std::map<PageKey, StoredPage, PageKeyCmp> pages;

    const TextureInfo& info(int texture_id) const {
        auto it = textures.find(texture_id);
        if (it == textures.end())
            throw std::invalid_argument("unknown texture id");
        return it->second;
    }
};

VirtualTexturePool::VirtualTexturePool(std::vector<TextureDesc> texture_descs)
    : impl_(std::make_shared<Impl>()) {
    std::set<int> seen;
    for (const TextureDesc &d : texture_descs) {
        if (!is_pow2(d.base_width) || !is_pow2(d.base_height) ||
            !is_pow2(d.page_size))
            throw std::invalid_argument("texture dimensions and page size must be positive powers of two");
        if (!seen.insert(d.texture_id).second)
            throw std::invalid_argument("duplicate texture id");
        TextureInfo ti;
        ti.desc = d;
        ti.levels = build_levels(d);
        impl_->textures.emplace(d.texture_id, std::move(ti));
    }

    // The 1x1 last level of every texture is permanently resident (black until
    // the caller replaces it), so every fallback walk always terminates.
    for (const auto &[id, ti] : impl_->textures) {
        const int last = static_cast<int>(ti.levels.size()) - 1;
        StoredPage pinned;
        pinned.width = 1;
        pinned.height = 1;
        pinned.rgb = {0., 0., 0.};
        impl_->pages.emplace(PageKey{id, last, 0, 0}, std::move(pinned));
    }
}

std::size_t VirtualTexturePool::texture_count() const {
    return impl_->textures.size();
}

const TextureDesc& VirtualTexturePool::desc(int texture_id) const {
    return impl_->info(texture_id).desc;
}

int VirtualTexturePool::level_count(int texture_id) const {
    return static_cast<int>(impl_->info(texture_id).levels.size());
}

int VirtualTexturePool::last_level(int texture_id) const {
    return level_count(texture_id) - 1;
}

int VirtualTexturePool::level_width(int texture_id, int level) const {
    const TextureInfo &ti = impl_->info(texture_id);
    if (level < 0 || level >= static_cast<int>(ti.levels.size()))
        throw std::invalid_argument("level out of range");
    return ti.levels[level].width;
}

int VirtualTexturePool::level_height(int texture_id, int level) const {
    const TextureInfo &ti = impl_->info(texture_id);
    if (level < 0 || level >= static_cast<int>(ti.levels.size()))
        throw std::invalid_argument("level out of range");
    return ti.levels[level].height;
}

int VirtualTexturePool::pages_x(int texture_id, int level) const {
    const TextureInfo &ti = impl_->info(texture_id);
    if (level < 0 || level >= static_cast<int>(ti.levels.size()))
        throw std::invalid_argument("level out of range");
    return ti.levels[level].pages_x;
}

int VirtualTexturePool::pages_y(int texture_id, int level) const {
    const TextureInfo &ti = impl_->info(texture_id);
    if (level < 0 || level >= static_cast<int>(ti.levels.size()))
        throw std::invalid_argument("level out of range");
    return ti.levels[level].pages_y;
}

int VirtualTexturePool::page_width(int texture_id, int level, int page_x) const {
    const TextureInfo &ti = impl_->info(texture_id);
    if (level < 0 || level >= static_cast<int>(ti.levels.size()) ||
        page_x < 0 || page_x >= ti.levels[level].pages_x)
        throw std::invalid_argument("page coordinate out of range");
    const LevelInfo &li = ti.levels[level];
    return std::min(ti.desc.page_size, li.width - page_x * ti.desc.page_size);
}

int VirtualTexturePool::page_height(int texture_id, int level, int page_y) const {
    const TextureInfo &ti = impl_->info(texture_id);
    if (level < 0 || level >= static_cast<int>(ti.levels.size()) ||
        page_y < 0 || page_y >= ti.levels[level].pages_y)
        throw std::invalid_argument("page coordinate out of range");
    const LevelInfo &li = ti.levels[level];
    return std::min(ti.desc.page_size, li.height - page_y * ti.desc.page_size);
}

bool VirtualTexturePool::resident(const PageKey &key) const {
    return impl_->pages.find(key) != impl_->pages.end();
}

void VirtualTexturePool::upload(const PageUpload &page) {
    // Validate everything before touching the working set.
    const TextureInfo &ti = impl_->info(page.key.texture_id);
    const PageKey &k = page.key;
    if (k.level < 0 || k.level >= static_cast<int>(ti.levels.size()))
        throw std::invalid_argument("page level out of range");
    const LevelInfo &li = ti.levels[k.level];
    if (k.page_x < 0 || k.page_x >= li.pages_x ||
        k.page_y < 0 || k.page_y >= li.pages_y)
        throw std::invalid_argument("page coordinate out of range");
    const int expect_w =
        std::min(ti.desc.page_size, li.width - k.page_x * ti.desc.page_size);
    const int expect_h =
        std::min(ti.desc.page_size, li.height - k.page_y * ti.desc.page_size);
    if (page.width != expect_w || page.height != expect_h)
        throw std::invalid_argument("page extents do not match its actual range");
    if (static_cast<int>(page.rgb.size()) != expect_w * expect_h * 3)
        throw std::invalid_argument("page pixel count does not match its extents");
    for (double c : page.rgb)
        if (!std::isfinite(c))
            throw std::invalid_argument("page pixels must be finite");

    StoredPage stored;
    stored.width = page.width;
    stored.height = page.height;
    stored.rgb = page.rgb; // owning copy; re-upload replaces the old page
    impl_->pages[k] = std::move(stored);
}

void VirtualTexturePool::evict(const PageKey &key) {
    const TextureInfo &ti = impl_->info(key.texture_id);
    if (key.level < 0 || key.level >= static_cast<int>(ti.levels.size()))
        throw std::invalid_argument("page level out of range");
    const LevelInfo &li = ti.levels[key.level];
    if (key.page_x < 0 || key.page_x >= li.pages_x ||
        key.page_y < 0 || key.page_y >= li.pages_y)
        throw std::invalid_argument("page coordinate out of range");
    if (key.level == static_cast<int>(ti.levels.size()) - 1)
        throw std::invalid_argument("the 1x1 last-level page is pinned");
    auto it = impl_->pages.find(key);
    if (it == impl_->pages.end())
        throw std::invalid_argument("page is not resident");
    impl_->pages.erase(it);
}

std::shared_ptr<const ResidentSnapshot> VirtualTexturePool::snapshot() const {
    auto out = std::make_shared<SnapshotImpl>();
    out->descs.reserve(impl_->textures.size());
    for (const auto &[id, ti] : impl_->textures) {
        out->descs.push_back(ti.desc);
        out->infos.emplace(id, ti);
    }
    out->page_map = impl_->pages; // deep copy of every uploaded pixel
    return out;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

struct VirtualTextureFrame::Impl {
    int width = 0, height = 0;
    mat<4,4> viewport{};
    std::shared_ptr<const SnapshotImpl> snapshot;
    std::vector<StoredTriangle> triangles;
    std::vector<FragmentInfo> fragments;
    std::vector<PageKey> missing;
    bool generated = false;
};

VirtualTextureFrame::VirtualTextureFrame(int width, int height,
                                         const mat<4,4> &viewport,
                                         std::shared_ptr<const ResidentSnapshot> snapshot)
    : impl_(std::make_unique<Impl>()) {
    if (width <= 0 || height <= 0)
        throw std::invalid_argument("frame dimensions must be positive");
    if (!snapshot)
        throw std::invalid_argument("frame requires a residency snapshot");
    impl_->width = width;
    impl_->height = height;
    impl_->viewport = viewport;
    impl_->snapshot = std::dynamic_pointer_cast<const SnapshotImpl>(snapshot);
    if (!impl_->snapshot)
        throw std::invalid_argument("snapshot was not created by VirtualTexturePool");
    impl_->fragments.assign(width * height, FragmentInfo{});
}

int VirtualTextureFrame::width() const { return impl_->width; }
int VirtualTextureFrame::height() const { return impl_->height; }

VirtualTextureFrame::~VirtualTextureFrame() = default;

void VirtualTextureFrame::submit_triangle(int texture_id, bool discard,
                                          const vec4 clip[3], const vec2 uv[3]) {
    if (impl_->generated)
        throw std::logic_error("frame already generated");
    if (impl_->snapshot->infos.find(texture_id) == impl_->snapshot->infos.end())
        throw std::invalid_argument("unknown texture id");
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 4; ++k)
            if (!std::isfinite(clip[i][k]))
                throw std::invalid_argument("non-finite clip coordinate");
        if (!(clip[i].w > 0.0))
            throw std::invalid_argument("clip w must be positive");
        if (!std::isfinite(uv[i][0]) || !std::isfinite(uv[i][1]))
            throw std::invalid_argument("non-finite uv coordinate");
        const double nx = clip[i].x / clip[i].w;
        const double ny = clip[i].y / clip[i].w;
        const double nz = clip[i].z / clip[i].w;
        if (nx < -1.0 || nx > 1.0 || ny < -1.0 || ny > 1.0 ||
            nz < -1.0 || nz > 1.0)
            throw std::invalid_argument("vertex outside the view frustum");
    }

    StoredTriangle tri;
    tri.texture_id = texture_id;
    tri.discard = discard;
    tri.face = static_cast<int>(impl_->triangles.size());
    for (int i = 0; i < 3; ++i) {
        tri.clip[i] = clip[i];
        tri.uv[i] = uv[i];
    }
    impl_->triangles.push_back(tri);
}

void VirtualTextureFrame::generate() {
    if (impl_->generated) return;
    impl_->generated = true;

    const int W = impl_->width, H = impl_->height;
    std::vector<double> zbuffer(W * H, -1000.);
    std::vector<int> winner(W * H, -1);

    // Pass 1: visibility only, using the shared coverage/barycentric/depth code.
    for (const StoredTriangle &tri : impl_->triangles) {
        vec4 ndc[3];
        vec2 screen[3];
        gl_project_triangle(tri.clip, impl_->viewport, ndc, screen);
        PixelBounds bb = gl_pixel_bounds(screen, W, H);
        for (int x = bb.x0; x <= bb.x1; ++x) {
            for (int y = bb.y0; y <= bb.y1; ++y) {
                vec3 bc_screen;
                if (!gl_screen_barycentric(screen, x, y, bc_screen)) continue;
                const double z = gl_interpolated_depth(bc_screen, ndc);
                const int idx = x + y * W;
                if (z <= zbuffer[idx]) continue;   // larger z/w is closer; ties keep the first face
                if (tri.discard) continue;         // discard never writes depth
                zbuffer[idx] = z;
                winner[idx] = tri.face;
            }
        }
    }

    std::set<PageKey, PageKeyCmp> missing_set;

    // Pass 2: only winning fragments get sampled and emit page requests.
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const int idx = x + y * W;
            const int face = winner[idx];
            if (face < 0) continue;               // background emits no request
            const StoredTriangle &tri = impl_->triangles[face];
            const SnapshotImpl &snap = *impl_->snapshot;
            const TextureInfo &info = snap.infos.at(tri.texture_id);

            vec4 ndc[3];
            vec2 screen[3];
            gl_project_triangle(tri.clip, impl_->viewport, ndc, screen);

            vec3 bc_screen;
            if (!gl_screen_barycentric(screen, x, y, bc_screen)) continue;
            const vec3 bc = gl_perspective_barycentric(bc_screen, tri.clip);

            vec2 uv = tri.uv[0] * bc[0] + tri.uv[1] * bc[1] + tri.uv[2] * bc[2];

            // Analytical derivatives of the perspective-correct quotient
            // U = (sum a_i uv_i) / (sum a_i), with a_i = lambda_i / w_i.
            mat<3,3> ABC = {{ {screen[0].x, screen[0].y, 1.},
                              {screen[1].x, screen[1].y, 1.},
                              {screen[2].x, screen[2].y, 1.} }};
            const mat<3,3> Minv = ABC.invert_transpose();
            const vec3 dlambda_dx{Minv[0][0], Minv[1][0], Minv[2][0]};
            const vec3 dlambda_dy{Minv[0][1], Minv[1][1], Minv[2][1]};
            const vec3 a{bc_screen[0] / tri.clip[0].w,
                         bc_screen[1] / tri.clip[1].w,
                         bc_screen[2] / tri.clip[2].w};
            const vec3 da_dx{dlambda_dx[0] / tri.clip[0].w,
                             dlambda_dx[1] / tri.clip[1].w,
                             dlambda_dx[2] / tri.clip[2].w};
            const vec3 da_dy{dlambda_dy[0] / tri.clip[0].w,
                             dlambda_dy[1] / tri.clip[1].w,
                             dlambda_dy[2] / tri.clip[2].w};
            const double s = a[0] + a[1] + a[2];
            vec2 num{0., 0.};
            vec2 dnum_dx{0., 0.}, dnum_dy{0., 0.};
            for (int i = 0; i < 3; ++i) {
                num     = num     + tri.uv[i] * a[i];
                dnum_dx = dnum_dx + tri.uv[i] * da_dx[i];
                dnum_dy = dnum_dy + tri.uv[i] * da_dy[i];
            }
            const double ds_dx = da_dx[0] + da_dx[1] + da_dx[2];
            const double ds_dy = da_dy[0] + da_dy[1] + da_dy[2];
            const vec2 duv_dx = (dnum_dx * s - num * ds_dx) * (1.0 / (s * s));
            const vec2 duv_dy = (dnum_dy * s - num * ds_dy) * (1.0 / (s * s));

            const int last = static_cast<int>(info.levels.size()) - 1;
            const int W0 = info.levels[0].width;
            const int H0 = info.levels[0].height;
            const double rx = std::hypot(W0 * duv_dx[0], H0 * duv_dx[1]);
            const double ry = std::hypot(W0 * duv_dy[0], H0 * duv_dy[1]);
            const double rho = std::max(rx, ry);
            const double lod = std::clamp(std::log2(std::max(1.0, rho)),
                                          0.0, static_cast<double>(last));
            const int level0 = static_cast<int>(std::floor(lod));
            const int level1 = std::min(level0 + 1, last);
            const double frac = lod - level0;

            DwordSample s0 = resolve_dword(snap, info, level0, uv[0], uv[1]);
            vec3 color = s0.color;
            DwordSample s1;
            int used1 = -1;
            const bool use_second = frac > 0.0 && level1 > level0;
            if (use_second) {
                s1 = resolve_dword(snap, info, level1, uv[0], uv[1]);
                color = s0.color * (1.0 - frac) + s1.color * frac;
                used1 = s1.used_level;
            }
            for (const PageKey &key : s0.missing) missing_set.insert(key);
            if (use_second)
                for (const PageKey &key : s1.missing) missing_set.insert(key);

            FragmentInfo &f = impl_->fragments[idx];
            f.r = color[0]; f.g = color[1]; f.b = color[2];
            f.face = face;
            f.depth = zbuffer[idx];
            f.u = uv[0]; f.v = uv[1];
            f.dudx = duv_dx[0]; f.dvdx = duv_dx[1];
            f.dudy = duv_dy[0]; f.dvdy = duv_dy[1];
            f.lod = lod;
            f.level0 = level0;
            f.level1 = use_second ? level1 : -1;
            f.used_level0 = s0.used_level;
            f.used_level1 = used1;
            f.lod_frac = frac;
        }
    }

    impl_->missing.assign(missing_set.begin(), missing_set.end());
}

const FragmentInfo& VirtualTextureFrame::fragment(int x, int y) const {
    if (!impl_->generated)
        throw std::logic_error("frame has not been generated");
    if (x < 0 || y < 0 || x >= impl_->width || y >= impl_->height)
        throw std::invalid_argument("pixel out of range");
    return impl_->fragments[x + y * impl_->width];
}

const std::vector<PageKey>& VirtualTextureFrame::missing_pages() const {
    if (!impl_->generated)
        throw std::logic_error("frame has not been generated");
    return impl_->missing;
}

} // namespace vt
