#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include "virtual_texture.h"
#include "our_gl.h"

namespace vtex {

static bool is_pow2(int x) { return x > 0 && (x & (x - 1)) == 0; }
static bool finite_rgb(const std::vector<RGBd> &p) {
    for (const RGBd &c : p)
        if (!std::isfinite(c.r) || !std::isfinite(c.g) || !std::isfinite(c.b)) return false;
    return true;
}

// ---------------------------------------------------------------- mip geometry

int level_count(const TextureDesc &desc) {
    int n = 1;
    int w = desc.base_width, h = desc.base_height;
    while (w > 1 || h > 1) {
        if (w > 1) w /= 2;
        if (h > 1) h /= 2;
        ++n;
    }
    return n;
}

void level_size(const TextureDesc &desc, int level, int &out_w, int &out_h) {
    out_w = std::max(1, desc.base_width  >> level);
    out_h = std::max(1, desc.base_height >> level);
}

int page_count_x(const TextureDesc &desc, int level) {
    int w, h;
    level_size(desc, level, w, h);
    return (w + desc.page_size - 1) / desc.page_size;
}

int page_count_y(const TextureDesc &desc, int level) {
    int w, h;
    level_size(desc, level, w, h);
    return (h + desc.page_size - 1) / desc.page_size;
}

void page_extent(const TextureDesc &desc, int level, int px, int py, int &out_w, int &out_h) {
    int w, h;
    level_size(desc, level, w, h);
    int x0 = px * desc.page_size;
    int y0 = py * desc.page_size;
    out_w = std::min(desc.page_size, w - x0);
    out_h = std::min(desc.page_size, h - y0);
}

// ------------------------------------------------------------- SparseTexturePool

void SparseTexturePool::declare_texture(int texture, const TextureDesc &desc) {
    if (texture < 0 || !is_pow2(desc.base_width) || !is_pow2(desc.base_height) ||
        !is_pow2(desc.page_size))
        throw std::invalid_argument("invalid texture description");
    if (descs.count(texture))
        throw std::invalid_argument("texture already declared");
    descs.emplace(texture, desc); // state changes only after every check passes
}

const TextureDesc& SparseTexturePool::desc(int texture) const {
    return descs.at(texture);
}

int SparseTexturePool::level_count(int texture) const {
    return vtex::level_count(descs.at(texture));
}

void SparseTexturePool::level_size(int texture, int level, int &out_w, int &out_h) const {
    vtex::level_size(descs.at(texture), level, out_w, out_h);
}

int SparseTexturePool::page_count_x(int texture, int level) const {
    return vtex::page_count_x(descs.at(texture), level);
}

int SparseTexturePool::page_count_y(int texture, int level) const {
    return vtex::page_count_y(descs.at(texture), level);
}

void SparseTexturePool::page_extent(int texture, int level, int px, int py, int &out_w, int &out_h) const {
    vtex::page_extent(descs.at(texture), level, px, py, out_w, out_h);
}

void SparseTexturePool::validate_page_write(int texture, int level, int px, int py,
                                            const PageData &data) const {
    auto it = descs.find(texture);
    if (it == descs.end())
        throw std::invalid_argument("unknown texture");
    const TextureDesc &d = it->second;
    int last = vtex::level_count(d) - 1;
    if (level < 0 || level > last)
        throw std::invalid_argument("invalid mip level");
    int npx = vtex::page_count_x(d, level), npy = vtex::page_count_y(d, level);
    if (px < 0 || py < 0 || px >= npx || py >= npy)
        throw std::invalid_argument("invalid page coordinates");
    int ew, eh;
    vtex::page_extent(d, level, px, py, ew, eh);
    if (data.width != ew || data.height != eh ||
        data.pixels.size() != static_cast<size_t>(ew) * static_cast<size_t>(eh))
        throw std::invalid_argument("page pixel count does not match its extent");
    if (!finite_rgb(data.pixels))
        throw std::invalid_argument("non-finite pixel value");
}

void SparseTexturePool::upload_page(int texture, int level, int px, int py, const PageData &data) {
    validate_page_write(texture, level, px, py, data);
    PageKey key{texture, level, px, py};
    if (pages.count(key))
        throw std::invalid_argument("page already resident; use replace_page");
    pages.emplace(key, data);
}

void SparseTexturePool::replace_page(int texture, int level, int px, int py, const PageData &data) {
    validate_page_write(texture, level, px, py, data);
    PageKey key{texture, level, px, py};
    if (!pages.count(key))
        throw std::invalid_argument("page is not resident; use upload_page");
    pages.at(key) = data;
}

void SparseTexturePool::evict_page(int texture, int level, int px, int py) {
    auto it = descs.find(texture);
    if (it == descs.end())
        throw std::invalid_argument("unknown texture");
    int last = vtex::level_count(it->second) - 1;
    if (level == last)
        throw std::invalid_argument("the 1x1 last mip level is pinned and cannot be evicted");
    PageKey key{texture, level, px, py};
    if (!pages.erase(key))
        throw std::invalid_argument("page is not resident");
}

bool SparseTexturePool::is_resident(int texture, int level, int px, int py) const {
    return pages.count(PageKey{texture, level, px, py}) != 0;
}

std::shared_ptr<const ResidencySnapshot> SparseTexturePool::snapshot() const {
    auto out = std::make_shared<ResidencySnapshot>();
    out->descs = descs;
    out->pages = pages;
    out->keys.reserve(pages.size());
    for (const auto &kv : pages) out->keys.push_back(kv.first); // std::map order == feedback order
    return out;
}

// --------------------------------------------------------------- paging sampler

namespace {

struct Tap { int ix, iy; double wx, wy; };

// Four bilinear taps for a level, computed from the original uv every time.
// Periodic modulo on every integer texel index; zero-weight taps are reported
// with weight 0 so the caller can skip their pages entirely.
void bilinear_taps(double u, double v, int W, int H, Tap taps[4]) {
    double fx = u * W - 0.5;
    double fy = v * H - 0.5;
    int x0 = static_cast<int>(std::floor(fx));
    int y0 = static_cast<int>(std::floor(fy));
    double tx = fx - x0, ty = fy - y0;
    int ix0 = ((x0 % W) + W) % W, ix1 = (((x0 + 1) % W) + W) % W;
    int iy0 = ((y0 % H) + H) % H, iy1 = (((y0 + 1) % H) + H) % H;
    taps[0] = {ix0, iy0, 1. - tx, 1. - ty};
    taps[1] = {ix1, iy0, tx,      1. - ty};
    taps[2] = {ix0, iy1, 1. - tx, ty};
    taps[3] = {ix1, iy1, tx,       ty};
}

} // namespace

struct Sampler {
    const ResidencySnapshot &snap;
    int texture;
    const TextureDesc &d;
    int last;

    Sampler(const ResidencySnapshot &s, int tex)
        : snap(s), texture(tex), d(s.desc(tex)), last(vtex::level_count(s.desc(tex)) - 1) {}

    RGBd read_tap(int level, const Tap &t) const {
        const PageData *page = snap.find(texture, level, t.ix / d.page_size, t.iy / d.page_size);
        int lx = t.ix - (t.ix / d.page_size) * d.page_size;
        int ly = t.iy - (t.iy / d.page_size) * d.page_size;
        return page->pixels[lx + ly * page->width];
    }

    // Resolve one LOD band starting at its ideal level. Missing ideal-level
    // pages are appended to `missing`; the footprint then falls back to
    // coarser levels, recomputing all four taps on the original uv each time,
    // until every non-zero tap is resident.
    RGBd resolve_band(double u, double v, int ideal, std::set<PageKey> &missing, int &used_level) const {
        int level = ideal;
        for (;;) {
            int W, H;
            vtex::level_size(d, level, W, H);
            Tap taps[4];
            bilinear_taps(u, v, W, H, taps);

            bool all_resident = true;
            for (const Tap &t : taps) {
                if (t.wx == 0. || t.wy == 0.) continue;
                int px = t.ix / d.page_size, py = t.iy / d.page_size;
                PageKey key{texture, level, px, py};
                if (!snap.resident(key)) {
                    all_resident = false;
                    if (level == ideal) missing.insert(key); // feedback covers the ideal level only
                }
            }
            if (all_resident) {
                used_level = level;
                RGBd color{};
                for (const Tap &t : taps) {
                    double w = t.wx * t.wy;
                    if (w == 0.) continue;
                    RGBd c = read_tap(level, t);
                    color.r += w * c.r;
                    color.g += w * c.g;
                    color.b += w * c.b;
                }
                return color;
            }
            if (level == last) { // pinned 1x1 level makes this unreachable in valid use
                used_level = level;
                return RGBd{};
            }
            ++level;
        }
    }
};

// ------------------------------------------------------------ VirtualTextureFrame

VirtualTextureFrame::VirtualTextureFrame(int width, int height,
                                         std::shared_ptr<const ResidencySnapshot> residency,
                                         const mat<4,4> &vp)
    : w(width), h(height), snap(std::move(residency)), viewport(vp) {
    if (w <= 0 || h <= 0)
        throw std::invalid_argument("frame dimensions must be positive");
    if (!snap)
        throw std::invalid_argument("frame requires a residency snapshot");
    winners.assign(static_cast<size_t>(w) * h, Winner{});
    zbuffer.assign(static_cast<size_t>(w) * h, -1000.);
}

void VirtualTextureFrame::submit(const vec4 clip[3], const vec2 uv[3], int texture, bool discard) {
    if (is_resolved)
        throw std::invalid_argument("frame already resolved");
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(clip[i].x) || !std::isfinite(clip[i].y) ||
            !std::isfinite(clip[i].z) || !std::isfinite(clip[i].w))
            throw std::invalid_argument("non-finite clip coordinate");
        if (!(clip[i].w > 0.))
            throw std::invalid_argument("clip w must be positive");
        if (clip[i].x < -clip[i].w || clip[i].x >  clip[i].w ||
            clip[i].y < -clip[i].w || clip[i].y >  clip[i].w ||
            clip[i].z < -clip[i].w || clip[i].z >  clip[i].w)
            throw std::invalid_argument("vertex outside the clip frustum");
        if (!std::isfinite(uv[i].x) || !std::isfinite(uv[i].y))
            throw std::invalid_argument("non-finite uv");
    }
    if (!snap->has_texture(texture))
        throw std::invalid_argument("unknown texture for bound residency snapshot");

    StoredTriangle t;
    for (int i = 0; i < 3; ++i) { t.clip[i] = clip[i]; t.uv[i] = uv[i]; }
    t.texture = texture;
    t.discard = discard;
    tris.push_back(t);
}

void VirtualTextureFrame::resolve() {
    if (is_resolved) return; // repeated resolution keeps the same results

    std::set<PageKey> missing_set;

    for (int face = 0; face < static_cast<int>(tris.size()); ++face) {
        const StoredTriangle &tri = tris[face];
        ProjectedTriangle pt;
        if (!project_triangle(tri.clip, viewport, pt)) continue; // shared coverage/cull rule

        auto [bbminx,bbmaxx] = std::minmax({pt.screen[0].x, pt.screen[1].x, pt.screen[2].x});
        auto [bbminy,bbmaxy] = std::minmax({pt.screen[0].y, pt.screen[1].y, pt.screen[2].y});

        mat<3,3> Minv = pt.ABC.invert_transpose(); // rows map (x,y,1) to screen barycentrics
        double w0 = tri.clip[0].w, w1 = tri.clip[1].w, w2 = tri.clip[2].w;

        for (int x = std::max<int>(bbminx, 0); x <= std::min<int>(bbmaxx, w - 1); ++x) {
            for (int y = std::max<int>(bbminy, 0); y <= std::min<int>(bbmaxy, h - 1); ++y) {
                vec3 bc_screen = screen_barycentric(pt.ABC, static_cast<double>(x),
                                                    static_cast<double>(y)); // shared barycentrics
                if (bc_screen.x < 0 || bc_screen.y < 0 || bc_screen.z < 0) continue;
                double z = interpolate_depth(pt.ndc, bc_screen);             // shared depth
                size_t idx = static_cast<size_t>(x) + static_cast<size_t>(y) * w;
                if (z <= zbuffer[idx]) continue; // larger z/w is nearer; ties keep the first face
                if (tri.discard) continue;        // discard never writes depth or color

                vec3 bc = perspective_barycentric(bc_screen, tri.clip);
                vec2 uv{ tri.uv[0].x*bc.x + tri.uv[1].x*bc.y + tri.uv[2].x*bc.z,
                         tri.uv[0].y*bc.x + tri.uv[1].y*bc.y + tri.uv[2].y*bc.z };

                // Analytic derivatives of the perspective-interpolated quotient
                // lambda_i = (b_i/w_i)/S, differentiating the quotient itself.
                double S  = bc_screen.x/w0 + bc_screen.y/w1 + bc_screen.z/w2;
                double Sx = Minv[0][0]/w0 + Minv[1][0]/w1 + Minv[2][0]/w2;
                double Sy = Minv[0][1]/w0 + Minv[1][1]/w1 + Minv[2][1]/w2;
                double a[3] = { bc_screen.x/w0, bc_screen.y/w1, bc_screen.z/w2 };
                double db_dx[3] = { Minv[0][0], Minv[1][0], Minv[2][0] };
                double db_dy[3] = { Minv[0][1], Minv[1][1], Minv[2][1] };
                vec2 dudx{}, dudy{};
                for (int i = 0; i < 3; ++i) {
                    double dlx = (db_dx[i]/tri.clip[i].w * S - a[i]*Sx) / (S*S);
                    double dly = (db_dy[i]/tri.clip[i].w * S - a[i]*Sy) / (S*S);
                    dudx.x += dlx * tri.uv[i].x;
                    dudx.y += dlx * tri.uv[i].y;
                    dudy.x += dly * tri.uv[i].x;
                    dudy.y += dly * tri.uv[i].y;
                }

                const TextureDesc &d = snap->desc(tri.texture);
                double gx0 = d.base_width  * dudx.x, gy0 = d.base_height * dudx.y;
                double gx1 = d.base_width  * dudy.x, gy1 = d.base_height * dudy.y;
                double rho = std::max(std::sqrt(gx0*gx0 + gy0*gy0),
                                       std::sqrt(gx1*gx1 + gy1*gy1));
                int last = vtex::level_count(d) - 1;
                double lod = std::clamp(std::log2(std::max(1., rho)), 0., static_cast<double>(last));

                zbuffer[idx] = z;
                Winner &win = winners[idx];
                win.valid = true;
                win.face = face;
                win.texture = tri.texture;
                win.depth = z;
                win.uv = uv;
                win.dudx = dudx;
                win.dudy = dudy;
                win.lod = lod;
            }
        }
    }

    // Sampling and feedback run only for the final winners; occluded candidates
    // never published any requests.
    fragments.assign(static_cast<size_t>(w) * h, FragmentResult{});
    for (size_t idx = 0; idx < winners.size(); ++idx) {
        const Winner &win = winners[idx];
        FragmentResult &fr = fragments[idx];
        if (!win.valid) continue; // background: zero color, face -1, no request

        Sampler sampler(*snap, win.texture);
        int flo = static_cast<int>(std::floor(win.lod));
        int fhi = std::min(flo + 1, sampler.last);
        double t = win.lod - flo;

        int used_lo = -1, used_hi = -1;
        RGBd clo = sampler.resolve_band(win.uv.x, win.uv.y, flo, missing_set, used_lo);
        RGBd chi{};
        if (t > 0. && fhi != flo) { // zero-weight band is neither sampled nor reported
            chi = sampler.resolve_band(win.uv.x, win.uv.y, fhi, missing_set, used_hi);
            fr.color.r = clo.r*(1.-t) + chi.r*t;
            fr.color.g = clo.g*(1.-t) + chi.g*t;
            fr.color.b = clo.b*(1.-t) + chi.b*t;
        } else {
            fr.color = clo;
        }
        fr.face = win.face;
        fr.depth = win.depth;
        fr.uv = win.uv;
        fr.duv_dx = win.dudx;
        fr.duv_dy = win.dudy;
        fr.lod = win.lod;
        fr.lod_lo = used_lo;
        fr.lod_hi = used_hi;
    }

    feedback.assign(missing_set.begin(), missing_set.end()); // already ordered like PageKey
    is_resolved = true;
}

const FragmentResult& VirtualTextureFrame::fragment(int x, int y) const {
    if (!is_resolved)
        throw std::invalid_argument("frame has not been resolved");
    if (x < 0 || y < 0 || x >= w || y >= h)
        throw std::invalid_argument("pixel outside the viewport");
    return fragments[static_cast<size_t>(x) + static_cast<size_t>(y) * w];
}

const std::vector<PageKey>& VirtualTextureFrame::missing_pages() const {
    if (!is_resolved)
        throw std::invalid_argument("frame has not been resolved");
    return feedback;
}

} // namespace vtex
