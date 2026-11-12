// task_tests/target.cpp
//
// Content regression tests for the libimagequant C implementation, covering the
// real relationship: RGBA input -> internal (premultiplied, gamma-encoded) color
// distance -> palette index output.
//
// The reference distance is recomputed here in double precision from the public
// palette bytes only. Nothing from pam.h/nearest.c (their color transforms,
// colordifference, VP-tree or the distances stored in the tree) is reused.
//
// Registered groups: metric, search, layout, reuse.
// All material is generated in-repo from fixed seeds.

#include "libimagequant.h"
#include "registry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

namespace liqtest {

static constexpr double INTERNAL_GAMMA = 0.5499;
static constexpr double DIST_EPS = 1e-5;

struct RGBA {
    unsigned char r = 0, g = 0, b = 0, a = 0;
    bool operator==(const RGBA& o) const {
        return r == o.r && g == o.g && b == o.b && a == o.a;
    }
    bool operator!=(const RGBA& o) const { return !(*this == o); }
};

struct Linear { double a, r, g, b; };

// Deterministic in-repo generator (PCG-style LCG), no random_device / time seed.
struct Rng {
    unsigned long long state;
    explicit Rng(unsigned long long seed) : state(seed) {}
    unsigned next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<unsigned>(state >> 33);
    }
    unsigned below(unsigned n) { return n ? next() % n : 0; }
};

// Independent re-implementation of the library's byte->linear transform:
//   a   = byte_alpha / 255
//   ch  = pow(byte_ch/255, internal_gamma/gamma) * a   (premultiplied)
// Input bytes use the input gamma; palette bytes use the output gamma.
static Linear encode(const RGBA& c, double gamma) {
    const double a = c.a / 255.0;
    const double e = INTERNAL_GAMMA / gamma;
    return {
        a,
        std::pow(c.r / 255.0, e) * a,
        std::pow(c.g / 255.0, e) * a,
        std::pow(c.b / 255.0, e) * a,
    };
}

// One channel: squared difference when composited on black, and on white;
// take the larger. Premultiplied form with backgrounds 0/1 shortens to this.
static double channel_diff(double x, double y, double alpha_diff) {
    const double on_black = x - y;
    const double on_white = on_black + alpha_diff;
    return std::max(on_black * on_black, on_white * on_white);
}

// Reference color distance. x/y are premultiplied linear colors. The alpha
// difference is y.a - x.a, matching the C implementation exactly but in double.
static double color_distance(const Linear& x, const Linear& y) {
    const double da = y.a - x.a;
    return channel_diff(x.r, y.r, da)
         + channel_diff(x.g, y.g, da)
         + channel_diff(x.b, y.b, da);
}

static double black_only_sq(const Linear& x, const Linear& y) {
    const double dr = x.r - y.r, dg = x.g - y.g, db = x.b - y.b;
    return dr * dr + dg * dg + db * db;
}

static double white_only_sq(const Linear& x, const Linear& y) {
    const double da = y.a - x.a;
    const double dr = x.r - y.r + da;
    const double dg = x.g - y.g + da;
    const double db = x.b - y.b + da;
    return dr * dr + dg * dg + db * db;
}

struct Palette {
    std::vector<RGBA> bytes;
    std::vector<Linear> linear;
    double gamma;
};

static Palette decode_palette(const liq_palette* p, double output_gamma) {
    Palette pal;
    pal.gamma = output_gamma;
    pal.bytes.reserve(p->count);
    pal.linear.reserve(p->count);
    for (unsigned i = 0; i < p->count; ++i) {
        RGBA c{ p->entries[i].r, p->entries[i].g, p->entries[i].b, p->entries[i].a };
        pal.bytes.push_back(c);
        // Palette is interpreted with the output gamma.
        pal.linear.push_back(encode(c, output_gamma));
    }
    return pal;
}

struct Nearest { unsigned index; double min_distance; };

// Independent exhaustive search over every palette entry. Ties keep the first
// index, but callers compare distances, never internal ordering.
static Nearest exhaustive_nearest(const Linear& needle, const Palette& pal) {
    double best = 1e300;
    unsigned idx = 0;
    for (unsigned i = 0; i < pal.linear.size(); ++i) {
        const double d = color_distance(needle, pal.linear[i]);
        if (d < best) { best = d; idx = i; }
    }
    return { idx, best };
}

static std::string fmt_pixel(const RGBA& c) {
    std::ostringstream os;
    os << "RGBA(" << int(c.r) << "," << int(c.g) << "," << int(c.b) << "," << int(c.a) << ")";
    return os.str();
}

// Core content assertion: every output index must be at an exhaustive-optimal
// distance (within 1e-5). Reports the first offending pixel with all details.
static void verify_indices_are_optimal(Case& cse, const char* context,
                                       const std::vector<RGBA>& pixels,
                                       int width, int height, double input_gamma,
                                       const std::vector<unsigned char>& indices,
                                       const Palette& pal) {
    cse.check(pixels.size() == indices.size(), "index count must match pixel count");
    cse.check(!pal.linear.empty(), "palette must not be empty");
    cse.check(indices.size() == static_cast<size_t>(width * height), "index buffer geometry");
    for (size_t i = 0; i < pixels.size(); ++i) {
        const Linear needle = encode(pixels[i], input_gamma);
        const Nearest ex = exhaustive_nearest(needle, pal);
        const double chosen = color_distance(needle, pal.linear[indices[i]]);
        if (chosen > ex.min_distance + DIST_EPS) {
            std::ostringstream os;
            os << context << ": first non-optimal pixel index=" << i
               << " (x=" << (i % width) << ",y=" << i / width << ")"
               << " " << fmt_pixel(pixels[i])
               << " output_index=" << static_cast<int>(indices[i])
               << " actual_distance=" << chosen
               << " exhaustive_min_distance=" << ex.min_distance
               << " gap=" << (chosen - ex.min_distance);
            cse.check(false, os.str().c_str());
        }
        cse.check(chosen <= ex.min_distance + DIST_EPS, "distance within tolerance of exhaustive minimum");
    }
}

static liq_error add_fixed(liq_histogram* h, const std::vector<RGBA>& fixed, double gamma) {
    for (const RGBA& c : fixed) {
        liq_error e = liq_histogram_add_fixed_color(h, liq_color{c.r, c.g, c.b, c.a}, gamma);
        if (e != LIQ_OK) return e;
    }
    return LIQ_OK;
}

// Quantize with an all-fixed palette. liq_histogram_quantize marks every result
// color fixed, so K-means cannot move any center and the reference premise holds.
static liq_result* quantize_fixed_histogram(liq_attr* attr,
                                            const std::vector<RGBA>& fixed,
                                            const std::vector<RGBA>& histogram_colors,
                                            double gamma) {
    liq_histogram* h = liq_histogram_create(attr);
    if (!h) return nullptr;
    if (add_fixed(h, fixed, gamma) != LIQ_OK) { liq_histogram_destroy(h); return nullptr; }
    std::vector<liq_histogram_entry> entries;
    entries.reserve(histogram_colors.size());
    for (const RGBA& c : histogram_colors)
        entries.push_back(liq_histogram_entry{ liq_color{c.r, c.g, c.b, c.a}, 1 });
    liq_error e = liq_histogram_add_colors(h, attr, entries.data(),
                                            static_cast<int>(entries.size()), gamma);
    if (e == LIQ_OK) {
        liq_result* r = nullptr;
        e = liq_histogram_quantize(h, attr, &r);
        if (e == LIQ_OK) {
            liq_set_dithering_level(r, 0.0f);
            liq_histogram_destroy(h);
            return r;
        }
    }
    liq_histogram_destroy(h);
    return nullptr;
}

// Same all-fixed premise, but fed through an image (exercises add_image path).
static liq_result* quantize_fixed_from_image(liq_attr* attr,
                                             const std::vector<RGBA>& fixed,
                                             liq_image* img, double gamma) {
    liq_histogram* h = liq_histogram_create(attr);
    if (!h) return nullptr;
    if (add_fixed(h, fixed, gamma) != LIQ_OK) { liq_histogram_destroy(h); return nullptr; }
    liq_error e = liq_histogram_add_image(h, attr, img);
    if (e == LIQ_OK) {
        liq_result* r = nullptr;
        e = liq_histogram_quantize(h, attr, &r);
        if (e == LIQ_OK) {
            liq_set_dithering_level(r, 0.0f);
            liq_histogram_destroy(h);
            return r;
        }
    }
    liq_histogram_destroy(h);
    return nullptr;
}

static std::vector<unsigned char> remap(liq_result* r, liq_image* img, size_t n) {
    std::vector<unsigned char> out(n);
    liq_error e = liq_write_remapped_image(r, img, out.data(), out.size());
    if (e != LIQ_OK) throw std::runtime_error("liq_write_remapped_image failed");
    return out;
}

// Every output palette byte must be one of the requested fixed colors at equal
// input/output gamma. Guards against a movable center sneaking into the palette.
static void assert_palette_is_subset_of_fixed(Case& cse, const Palette& pal,
                                              const std::vector<RGBA>& fixed) {
    cse.check(pal.bytes.size() == fixed.size(), "fixed palette size preserved");
    for (const RGBA& got : pal.bytes) {
        bool found = false;
        for (const RGBA& want : fixed) if (got == want) { found = true; break; }
        cse.check(found, "palette entry is one of the fixed colors (no K-means-moved center)");
    }
}

// ---- Deterministic shared material -----------------------------------------

static std::vector<RGBA> fixed_sixteen() {
    return {
        RGBA{10,200,30,255},  RGBA{200,10,40,255},  RGBA{30,40,210,255},  RGBA{240,240,10,255},
        RGBA{120,10,120,255}, RGBA{10,130,130,255}, RGBA{180,90,20,255},  RGBA{20,20,20,255},
        RGBA{230,230,230,255},RGBA{160,160,40,255}, RGBA{40,160,160,255},  RGBA{160,40,160,255},
        RGBA{90,180,60,128},  RGBA{60,90,180,128},  RGBA{200,200,200,64},  RGBA{0,0,0,0},
    };
}

// 16 asymmetric fixed colors from a fixed seed; alpha tiers force an internal
// VP-tree (>=7 palette entries recurse) and make far-side pruning reachable.
static std::vector<RGBA> rng_fixed_sixteen() {
    Rng rng(0xC0FFEE123ULL);
    static const int alpha_tiers[4] = {255, 255, 128, 40};
    std::vector<RGBA> fixed;
    for (unsigned i = 0; i < 16; ++i) {
        if (i == 15) { fixed.push_back(RGBA{0, 0, 0, 0}); continue; }
        auto clamp = [](int v){ return v < 0 ? 0 : v > 255 ? 255 : v; };
        RGBA c{
            static_cast<unsigned char>(clamp(20 + rng.below(230))),
            static_cast<unsigned char>(clamp(20 + rng.below(230))),
            static_cast<unsigned char>(clamp(20 + rng.below(230))),
            static_cast<unsigned char>(alpha_tiers[i % 4]),
        };
        const int strong = i % 3;
        const int boosted = clamp(150 + rng.below(100));
        if (strong == 0) c.r = static_cast<unsigned char>(boosted);
        if (strong == 1) c.g = static_cast<unsigned char>(boosted);
        if (strong == 2) c.b = static_cast<unsigned char>(boosted);
        fixed.push_back(c);
    }
    return fixed;
}

// Non-uniform 9x7 image: perturbed copies of fixed colors, duplicated runs, and
// varied histogram counts. Fixed RNG makes it fully reproducible.
static std::vector<RGBA> rng_pixels(const std::vector<RGBA>& fixed,
                                    int width, int height) {
    Rng rng(0x5EED9A77ULL);
    auto clamp = [](int v){ return v < 0 ? 0 : v > 255 ? 255 : v; };
    std::vector<RGBA> px(width * height);
    for (int i = 0; i < width * height; ++i) {
        RGBA base = fixed[rng.below(static_cast<unsigned>(fixed.size()))];
        const int channel = rng.below(4);
        const int delta = static_cast<int>(rng.below(15)) - 7;
        RGBA c = base;
        if (channel == 0) c.r = static_cast<unsigned char>(clamp(base.r + delta));
        if (channel == 1) c.g = static_cast<unsigned char>(clamp(base.g + delta));
        if (channel == 2) c.b = static_cast<unsigned char>(clamp(base.b + delta));
        if (channel == 3) c.a = static_cast<unsigned char>(clamp(base.a + delta));
        px[i] = c;
        if (i > 0 && rng.below(3) == 0) px[i] = px[i - 1]; // duplicate -> non-uniform
    }
    return px;
}

// ---- Artifacts: explicit rerunnable input + enlarged PPMs -------------------

static void write_ppm(const std::string& path, const std::vector<RGBA>& px,
                      int width, int height, int scale, unsigned char bg) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path);
    f << "P6\n" << width * scale << " " << height * scale << "\n255\n";
    auto composite = [&](const RGBA& c) -> std::tuple<unsigned char,unsigned char,unsigned char> {
        const double af = c.a / 255.0;
        auto mix = [&](unsigned char v) {
            return static_cast<unsigned char>(std::lround(v * af + bg * (1.0 - af)));
        };
        return { mix(c.r), mix(c.g), mix(c.b) };
    };
    for (int y = 0; y < height * scale; ++y)
        for (int x = 0; x < width * scale; ++x) {
            const RGBA& c = px[(y / scale) * width + (x / scale)];
            auto [r, g, b] = composite(c);
            char buf[3] = { static_cast<char>(r), static_cast<char>(g), static_cast<char>(b) };
            f.write(buf, 3);
        }
}

static void ensure_artifacts() {
    static bool done = false;
    if (done) return;
    done = true;
    mkdir("artifacts", 0755); // ignore failure (e.g. already exists); file open below reports real errors
    const int W = 9, H = 7;
    const std::vector<RGBA> fixed = rng_fixed_sixteen();
    const std::vector<RGBA> px = rng_pixels(fixed, W, H);

    std::ofstream rep("artifacts/target_replay.txt");
    if (rep) {
        rep << "# libimagequant target regression - explicit rerunnable input\n";
        rep << "# search group; fixed RNG seeds 0xC0FFEE123 (palette) 0x5EED9A77 (pixels)\n";
        rep << "width " << W << "\nheight " << H << "\ngamma 0.45455\n";
        rep << "fixed_colors " << fixed.size() << "\n";
        for (const RGBA& c : fixed)
            rep << "fixed " << int(c.r) << " " << int(c.g) << " " << int(c.b) << " " << int(c.a) << "\n";
        rep << "pixels " << px.size() << "\n";
        for (size_t i = 0; i < px.size(); ++i)
            rep << "pixel " << i << " " << int(px[i].r) << " " << int(px[i].g) << " "
                << int(px[i].b) << " " << int(px[i].a) << "\n";
        rep << "# metric reversal triple (gamma 0.45455):\n";
        rep << "# needle RGBA(120,120,120,128) A RGBA(20,20,20,16) B RGBA(220,220,220,48)\n";
        rep << "# black background prefers B, white background prefers A;\n";
        rep << "# per-channel max rule selects A, black/white average would select B.\n";
    }
    write_ppm("artifacts/target_zoom_black.ppm", px, W, H, 12, 0);
    write_ppm("artifacts/target_zoom_white.ppm", px, W, H, 12, 255);
}

// ---- metric group -----------------------------------------------------------

static void metric_reversal(Case& cse) {
    ensure_artifacts();
    const double gamma = 0.45455;
    const RGBA needle{120, 120, 120, 128};
    const RGBA colA{20, 20, 20, 16};      // faint dark
    const RGBA colB{220, 220, 220, 48};   // faint light
    const Linear fn = encode(needle, gamma), fa = encode(colA, gamma), fb = encode(colB, gamma);

    // Semi-transparent needle whose candidate ordering flips with the backdrop.
    const bool black_prefers_B = black_only_sq(fn, fb) < black_only_sq(fn, fa);
    const bool white_prefers_A = white_only_sq(fn, fa) < white_only_sq(fn, fb);
    cse.check(black_prefers_B, "on black background B is closer");
    cse.check(white_prefers_A, "on white background A is closer (ranking reversal)");

    // The reference max rule must disagree with averaging the two backdrops.
    const double dA = color_distance(fn, fa), dB = color_distance(fn, fb);
    const double avgA = (black_only_sq(fn, fa) + white_only_sq(fn, fa)) / 2.0;
    const double avgB = (black_only_sq(fn, fb) + white_only_sq(fn, fb)) / 2.0;
    cse.check(dA < dB, "max-of-black/white rule selects A");
    cse.check(avgA > avgB, "black/white average would select B (different reference)");

    // Through the real public API, the needle must be remapped to A's entry.
    const std::vector<RGBA> fixed = {
        colA, colB,
        RGBA{200,30,30,255}, RGBA{30,200,30,255}, RGBA{30,30,200,255}, RGBA{240,240,30,255},
        RGBA{120,30,120,255}, RGBA{30,120,120,255},
    };
    const std::vector<RGBA> image = {
        needle, fixed[2], fixed[3], fixed[4], fixed[5], fixed[6], fixed[7], fixed[4], needle,
    };
    liq_attr* attr = liq_attr_create();
    cse.check(attr != nullptr, "attr created");
    liq_set_max_colors(attr, static_cast<int>(fixed.size()));
    liq_result* res = quantize_fixed_histogram(attr, fixed, image, gamma);
    cse.check(res != nullptr, "quantize with all-fixed palette");
    liq_image* img = liq_image_create_rgba(attr, image.data(), 3, 3, gamma);
    const auto idx = remap(res, img, image.size());
    const Palette pal = decode_palette(liq_get_palette(res), liq_get_output_gamma(res));
    assert_palette_is_subset_of_fixed(cse, pal, fixed);
    verify_indices_are_optimal(cse, "metric/reversal", image, 3, 3, gamma, idx, pal);
    cse.check(pal.bytes[idx[0]] == colA, "library remaps the needle to max-rule winner A");
    liq_result_destroy(res); liq_image_destroy(img); liq_attr_destroy(attr);
}

static void metric_alpha_edges(Case& cse) {
    ensure_artifacts();
    const double gamma = 0.45455;
    const RGBA transparent_red{200, 100, 50, 0};
    const RGBA transparent_blue{10, 20, 30, 0};
    const RGBA near_zero{120, 120, 120, 1};
    const RGBA full{120, 120, 120, 255};

    // Fully transparent colors with different RGB are identical once premultiplied.
    const Linear tr = encode(transparent_red, gamma), tb = encode(transparent_blue, gamma);
    cse.check(color_distance(tr, tb) == 0.0, "different RGB at alpha 0 have zero distance");
    // Near-zero vs full alpha must remain clearly separated.
    cse.check(color_distance(encode(near_zero, gamma), encode(full, gamma)) > 0.5,
              "alpha 1 vs alpha 255 stays far apart");

    const std::vector<RGBA> fixed = {
        transparent_red, transparent_blue, near_zero, full,
        RGBA{200,30,30,255}, RGBA{30,200,30,255}, RGBA{30,30,200,255}, RGBA{10,10,10,255},
    };
    const std::vector<RGBA> image = {
        transparent_red, transparent_blue, near_zero, full,
        fixed[4], fixed[5], fixed[6], fixed[7], near_zero,
    };
    liq_attr* attr = liq_attr_create();
    liq_set_max_colors(attr, static_cast<int>(fixed.size()));
    liq_result* res = quantize_fixed_histogram(attr, fixed, image, gamma);
    cse.check(res != nullptr, "quantize alpha-edge palette");
    liq_image* img = liq_image_create_rgba(attr, image.data(), 3, 3, gamma);
    const auto idx = remap(res, img, image.size());
    const liq_palette* lp = liq_get_palette(res);
    const Palette pal = decode_palette(lp, liq_get_output_gamma(res));
    bool has_transparent_entry = false;
    for (const RGBA& c : pal.bytes) if (c.a == 0) has_transparent_entry = true;
    cse.check(has_transparent_entry, "output palette exposes a fully transparent entry");
    verify_indices_are_optimal(cse, "metric/alpha_edges", image, 3, 3, gamma, idx, pal);
    liq_result_destroy(res); liq_image_destroy(img); liq_attr_destroy(attr);
}

static void metric_distinct_gammas(Case& cse) {
    ensure_artifacts();
    const std::vector<RGBA> fixed = fixed_sixteen();
    const std::vector<RGBA> image = rng_pixels(fixed, 6, 4);
    struct Pair { double in, out; };
    const Pair pairs[] = { {0.60, 0.45455}, {0.35, 0.55} };
    for (const Pair& p : pairs) {
        liq_attr* attr = liq_attr_create();
        liq_set_max_colors(attr, static_cast<int>(fixed.size()));
        liq_result* res = quantize_fixed_histogram(attr, fixed, image, p.in);
        cse.check(res != nullptr, "quantize at input gamma");
        liq_error ge = liq_set_output_gamma(res, p.out);
        cse.check(ge == LIQ_OK, "distinct output gamma accepted");
        liq_image* img = liq_image_create_rgba(attr, image.data(), 6, 4, p.in);
        const auto idx = remap(res, img, image.size());
        const double actual_out = liq_get_output_gamma(res);
        cse.check(std::fabs(actual_out - p.out) < 1e-12, "reported output gamma matches request");
        const Palette pal = decode_palette(liq_get_palette(res), actual_out);
        // Same byte must decode differently under the two gammas: proves gamma is honored.
        const Linear l_out = encode(RGBA{100, 100, 100, 255}, p.out);
        const Linear l_in = encode(RGBA{100, 100, 100, 255}, p.in);
        cse.check(color_distance(l_out, l_in) > 1e-4, "input vs output gamma change the linear decode");
        verify_indices_are_optimal(cse, "metric/distinct_gammas", image, 6, 4, p.in, idx, pal);
        liq_result_destroy(res); liq_image_destroy(img); liq_attr_destroy(attr);
    }
}

// ---- search group -----------------------------------------------------------

static void search_vp_tree(Case& cse) {
    ensure_artifacts();
    const double gamma = 0.45455;
    const int W = 9, H = 7, N = W * H;
    const std::vector<RGBA> fixed = rng_fixed_sixteen();
    const std::vector<RGBA> pixels = rng_pixels(fixed, W, H);
    cse.check(fixed.size() >= 16, "at least sixteen fixed colors form internal VP nodes");

    // Non-uniform material: duplicate colors must actually occur.
    bool has_duplicate = false;
    for (size_t i = 1; i < pixels.size(); ++i)
        if (pixels[i] == pixels[i - 1]) has_duplicate = true;
    cse.check(has_duplicate, "pixel stream contains duplicated (non-uniform) entries");

    liq_attr* attr = liq_attr_create();
    liq_set_max_colors(attr, static_cast<int>(fixed.size()));
    liq_result* res = quantize_fixed_histogram(attr, fixed, pixels, gamma);
    cse.check(res != nullptr, "quantize search palette");
    liq_image* img = liq_image_create_rgba(attr, pixels.data(), W, H, gamma);
    const auto idx = remap(res, img, N);
    const Palette pal = decode_palette(liq_get_palette(res), liq_get_output_gamma(res));
    assert_palette_is_subset_of_fixed(cse, pal, fixed);

    // Every pixel independently enumerated against every palette entry; the
    // library's VP-tree answer must be at the exhaustive minimum distance.
    verify_indices_are_optimal(cse, "search/vp_tree", pixels, W, H, gamma, idx, pal);

    // Adjacent-pixel hint coverage: the remapper passes the previous index as a
    // hint. Require both a correct hint (same optimal index) and a wrong hint
    // (different optimal index) to occur among horizontal neighbors.
    bool equal_adjacent = false, different_adjacent = false;
    for (int y = 0; y < H; ++y)
        for (int x = 1; x < W; ++x) {
            if (idx[y * W + x] == idx[y * W + x - 1]) equal_adjacent = true;
            else different_adjacent = true;
        }
    cse.check(equal_adjacent, "an adjacent pair shares an index (correct hint path)");
    cse.check(different_adjacent, "an adjacent pair changes index (wrong hint must fall through to tree search)");
    liq_result_destroy(res); liq_image_destroy(img); liq_attr_destroy(attr);
}

// ---- layout group -----------------------------------------------------------

struct CallbackState {
    const std::vector<RGBA>* data;
    int width;
    int calls = 0;
    std::vector<int> rows_requested;
};

static void layout_callback(liq_color row_out[], int row, int width, void* user_info) {
    CallbackState* st = static_cast<CallbackState*>(user_info);
    st->calls++;
    st->rows_requested.push_back(row);
    for (int x = 0; x < width; ++x) {
        const RGBA& c = (*st->data)[row * st->width + x];
        row_out[x] = liq_color{c.r, c.g, c.b, c.a};
    }
}

static void layout_three_inputs(Case& cse) {
    ensure_artifacts();
    const double gamma = 0.45455;
    const int W = 6, H = 4, N = W * H;
    const std::vector<RGBA> fixed = fixed_sixteen();
    const std::vector<RGBA> pixels = rng_pixels(fixed, W, H);

    // 1) contiguous RGBA array
    liq_attr* attr_a = liq_attr_create();
    liq_set_max_colors(attr_a, static_cast<int>(fixed.size()));
    liq_image* img_a = liq_image_create_rgba(attr_a, pixels.data(), W, H, gamma);
    liq_result* res_a = quantize_fixed_from_image(attr_a, fixed, img_a, gamma);
    cse.check(res_a != nullptr, "contiguous quantize");
    const auto idx_contig = remap(res_a, img_a, N);
    const Palette pal_contig = decode_palette(liq_get_palette(res_a), liq_get_output_gamma(res_a));
    assert_palette_is_subset_of_fixed(cse, pal_contig, fixed);
    verify_indices_are_optimal(cse, "layout/contiguous", pixels, W, H, gamma, idx_contig, pal_contig);

    // 2) row pointers with inter-row sentinel bytes that must survive untouched
    const int stride = W + 3;
    std::vector<unsigned char> backing(stride * H * 4, 0xCD);
    std::vector<void*> row_ptrs(H);
    for (int y = 0; y < H; ++y) {
        row_ptrs[y] = backing.data() + y * stride * 4;
        for (int x = 0; x < W; ++x) {
            const RGBA& c = pixels[y * W + x];
            unsigned char* p = backing.data() + y * stride * 4 + x * 4;
            p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = c.a;
        }
    }
    liq_attr* attr_b = liq_attr_create();
    liq_set_max_colors(attr_b, static_cast<int>(fixed.size()));
    liq_image* img_b = liq_image_create_rgba_rows(attr_b, row_ptrs.data(), W, H, gamma);
    liq_result* res_b = quantize_fixed_from_image(attr_b, fixed, img_b, gamma);
    cse.check(res_b != nullptr, "row-pointer quantize");
    const auto idx_rows = remap(res_b, img_b, N);
    const Palette pal_rows = decode_palette(liq_get_palette(res_b), liq_get_output_gamma(res_b));
    verify_indices_are_optimal(cse, "layout/row_pointers", pixels, W, H, gamma, idx_rows, pal_rows);
    bool sentinel_intact = true;
    for (int y = 0; y < H && sentinel_intact; ++y)
        for (int x = W; x < stride; ++x) {
            const unsigned char* p = backing.data() + y * stride * 4 + x * 4;
            for (int k = 0; k < 4; ++k) if (p[k] != 0xCD) sentinel_intact = false;
        }
    cse.check(sentinel_intact, "inter-row sentinel bytes never read or overwritten");

    // 3) custom row callback; do not assume one call or a fixed order
    CallbackState state{ &pixels, W, 0, {} };
    liq_attr* attr_c = liq_attr_create();
    liq_set_max_colors(attr_c, static_cast<int>(fixed.size()));
    liq_image* img_c = liq_image_create_custom(attr_c, layout_callback, &state, W, H, gamma);
    liq_result* res_c = quantize_fixed_from_image(attr_c, fixed, img_c, gamma);
    cse.check(res_c != nullptr, "callback quantize");
    const auto idx_cb = remap(res_c, img_c, N);
    const Palette pal_cb = decode_palette(liq_get_palette(res_c), liq_get_output_gamma(res_c));
    verify_indices_are_optimal(cse, "layout/callback", pixels, W, H, gamma, idx_cb, pal_cb);
    cse.check(state.calls >= H, "callback serves every row at least once (not assumed single-shot)");
    std::vector<int> served = state.rows_requested;
    std::sort(served.begin(), served.end());
    served.erase(std::unique(served.begin(), served.end()), served.end());
    cse.check(served.size() == static_cast<size_t>(H), "callback covered every row index");
    for (int r : served) cse.check(r >= 0 && r < H, "callback row index always in range");

    // All three layouts must decode to the same index image and the same palette.
    cse.check(idx_contig == idx_rows, "row pointers match contiguous indices");
    cse.check(idx_contig == idx_cb, "callback matches contiguous indices");
    cse.check(pal_contig.bytes == pal_rows.bytes, "row-pointer palette matches contiguous");
    cse.check(pal_contig.bytes == pal_cb.bytes, "callback palette matches contiguous");

    liq_result_destroy(res_a); liq_image_destroy(img_a); liq_attr_destroy(attr_a);
    liq_result_destroy(res_b); liq_image_destroy(img_b); liq_attr_destroy(attr_b);
    liq_result_destroy(res_c); liq_image_destroy(img_c); liq_attr_destroy(attr_c);
}

// ---- reuse group ------------------------------------------------------------

static std::vector<RGBA> image_a() {
    return {
        RGBA{12,198,33,255},  RGBA{197,13,44,255},  RGBA{28,42,207,255},  RGBA{238,237,12,255},
        RGBA{118,12,122,255}, RGBA{12,128,133,255},  RGBA{177,92,22,255},  RGBA{24,22,18,255},
        RGBA{227,229,232,255},RGBA{158,161,43,255}, RGBA{44,157,163,255},  RGBA{163,38,158,255},
        RGBA{93,177,63,127},  RGBA{57,93,177,127},  RGBA{198,202,197,63},  RGBA{5,5,5,2},
        RGBA{100,100,100,255},RGBA{70,30,200,255},  RGBA{200,70,30,255},   RGBA{30,200,70,255},
        RGBA{12,198,33,255},  RGBA{12,198,33,255},  RGBA{227,229,232,255}, RGBA{93,177,63,127},
    };
}

static std::vector<RGBA> image_b() {
    // Different dimensions and a distinctly different alpha distribution.
    return {
        RGBA{90,180,60,128},  RGBA{60,90,180,128},  RGBA{200,200,200,64}, RGBA{0,0,0,0},     RGBA{0,0,0,0},
        RGBA{11,199,31,255},  RGBA{201,11,41,255},  RGBA{31,41,209,255},  RGBA{241,241,11,255},RGBA{121,11,121,255},
        RGBA{93,177,63,40},   RGBA{57,93,177,90},   RGBA{198,202,197,16}, RGBA{8,8,8,4},     RGBA{230,230,230,255},
    };
}

static void reuse_interleaved(Case& cse) {
    ensure_artifacts();
    const double gamma = 0.45455;
    const std::vector<RGBA> fixed = fixed_sixteen();
    const std::vector<RGBA> A = image_a(); const int WA = 6, HA = 4;
    const std::vector<RGBA> B = image_b(); const int WB = 5, HB = 3;

    liq_attr* attr = liq_attr_create();
    liq_set_max_colors(attr, static_cast<int>(fixed.size()));
    liq_result* res = quantize_fixed_histogram(attr, fixed, A, gamma);
    cse.check(res != nullptr, "shared result quantized once");
    liq_image* imgA = liq_image_create_rgba(attr, A.data(), WA, HA, gamma);
    liq_image* imgB = liq_image_create_rgba(attr, B.data(), WB, HB, gamma);

    // Interleave A,B,A,B on one result. Snapshot the palette after every remap:
    // liq_get_palette returns storage that the next remap rewrites in place.
    const auto idxA1 = remap(res, imgA, A.size());
    const Palette pA1 = decode_palette(liq_get_palette(res), liq_get_output_gamma(res));
    const auto idxB1 = remap(res, imgB, B.size());
    const Palette pB1 = decode_palette(liq_get_palette(res), liq_get_output_gamma(res));
    const auto idxA2 = remap(res, imgA, A.size());
    const Palette pA2 = decode_palette(liq_get_palette(res), liq_get_output_gamma(res));
    const auto idxB2 = remap(res, imgB, B.size());
    const Palette pB2 = decode_palette(liq_get_palette(res), liq_get_output_gamma(res));

    // Each index image is decoded against the palette captured with that remap,
    // never a palette left over from the previous image.
    verify_indices_are_optimal(cse, "reuse/A1", A, WA, HA, gamma, idxA1, pA1);
    verify_indices_are_optimal(cse, "reuse/B1", B, WB, HB, gamma, idxB1, pB1);
    verify_indices_are_optimal(cse, "reuse/A2", A, WA, HA, gamma, idxA2, pA2);
    verify_indices_are_optimal(cse, "reuse/B2", B, WB, HB, gamma, idxB2, pB2);

    cse.check(idxA1 == idxA2, "image A indices deterministic across reuse");
    cse.check(idxB1 == idxB2, "image B indices deterministic across reuse");
    cse.check(pA1.bytes == pB1.bytes && pA1.bytes == pA2.bytes && pA1.bytes == pB2.bytes,
              "palette bytes identical for every remap of either image");

    // Independent, separately created objects must reach the same answer.
    liq_attr* attr2 = liq_attr_create();
    liq_set_max_colors(attr2, static_cast<int>(fixed.size()));
    liq_result* res2 = quantize_fixed_histogram(attr2, fixed, A, gamma);
    cse.check(res2 != nullptr, "independent result quantized");
    liq_image* imgB2 = liq_image_create_rgba(attr2, B.data(), WB, HB, gamma);
    const auto idxB_ind = remap(res2, imgB2, B.size());
    const Palette pBind = decode_palette(liq_get_palette(res2), liq_get_output_gamma(res2));
    cse.check(idxB_ind == idxB1, "independent objects produce the same B indices");
    cse.check(pBind.bytes == pB1.bytes, "independent objects produce the same B palette");
    verify_indices_are_optimal(cse, "reuse/independent", B, WB, HB, gamma, idxB_ind, pBind);

    liq_result_destroy(res); liq_image_destroy(imgA); liq_image_destroy(imgB); liq_attr_destroy(attr);
    liq_result_destroy(res2); liq_image_destroy(imgB2); liq_attr_destroy(attr2);
}

} // namespace liqtest

void register_task_tests(Registry& registry) {
    using namespace liqtest;
    registry.add("metric", "black_white_ranking_reversal", metric_reversal);
    registry.add("metric", "transparent_and_alpha_edges", metric_alpha_edges);
    registry.add("metric", "distinct_input_output_gammas", metric_distinct_gammas);
    registry.add("search", "vp_tree_exhaustive_distance", search_vp_tree);
    registry.add("layout", "three_input_layouts_match", layout_three_inputs);
    registry.add("reuse", "interleaved_result_two_images", reuse_interleaved);
}
