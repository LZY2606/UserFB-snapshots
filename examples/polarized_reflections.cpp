#include "polarized_renderer.h"

#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr int PANEL_W = 180;
constexpr int PANEL_H = 135;
constexpr int TOTAL_W = 3 * PANEL_W;

struct LinearPixel {
    double rgb[3] = {0.0, 0.0, 0.0};
};

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

double clamp01(double x) { return std::max(0.0, std::min(1.0, x)); }

void write_ppm(const std::string& path,
               const std::array<std::vector<LinearPixel>, 3>& panels) {
    std::ofstream out(path, std::ios::binary);
    require(static_cast<bool>(out), "cannot open output PPM: " + path);
    out << "P6\n" << TOTAL_W << ' ' << PANEL_H << "\n255\n";

    constexpr double exposure = 1.35;
    constexpr double inv_gamma = 1.0 / 2.2;
    for (int y = 0; y < PANEL_H; ++y) {
        for (int panel = 0; panel < 3; ++panel) {
            for (int x = 0; x < PANEL_W; ++x) {
                const LinearPixel& p = panels[panel][y * PANEL_W + x];
                for (int ch = 0; ch < 3; ++ch) {
                    double mapped = std::pow(clamp01(exposure * p.rgb[ch]),
                                             inv_gamma);
                    int byte = std::max(0, std::min(255,
                        static_cast<int>(std::lround(255.0 * mapped))));
                    out.put(static_cast<char>(byte));
                }
            }
        }
    }
    out.close();
    require(static_cast<bool>(out), "failed while writing PPM: " + path);
}

} // namespace

int main(int argc, char** argv) {
    std::string path = argc > 1 ? argv[1] : "polarized_reflections.ppm";

    PolarizedRenderer renderer;
    renderer.camera_origin = {0.0f, 0.0f, 0.0f};
    renderer.fov = 1.0f;
    renderer.max_reflections = 3;
    renderer.spheres = {
        {{-1.4f, -0.2f,  -7.0f}, 1.6f, 1.0, 1.50},
        {{ 1.5f, -0.4f,  -8.0f}, 2.0f, 1.0, 1.45},
        {{ 0.1f,  1.2f, -10.0f}, 1.2f, 1.0, 2.00}
    };

    // Strong, physically-cone-valid polarized background.  It is interpreted
    // in the published world projection basis for each background k.
    renderer.background[0] = {1.10,  0.55,  0.25,  0.05};
    renderer.background[1] = {1.00,  0.20,  0.60, -0.10};
    renderer.background[2] = {1.20, -0.50,  0.20,  0.30};

    const std::array<double, 3> angles = {0.0, PI / 3.0, 2.0 * PI / 3.0};
    std::array<std::vector<LinearPixel>, 3> panels;
    for (auto& panel : panels) panel.resize(PANEL_W * PANEL_H);

    for (int y = 0; y < PANEL_H; ++y) {
        for (int x = 0; x < PANEL_W; ++x) {
            vec3 D = renderer.camera_ray(x, y, PANEL_W, PANEL_H);
            pol::Vec3 camera_e1 = polarized_camera_reference(D, renderer.roll);
            pol::PolarizationState state = renderer.trace(D, &camera_e1);

            for (int a = 0; a < 3; ++a) {
                auto intensity = pol::analyzer_intensities(state.stokes,
                                                           angles[a]);
                LinearPixel& pixel = panels[a][y * PANEL_W + x];
                for (int ch = 0; ch < 3; ++ch) pixel.rgb[ch] = intensity[ch];
            }
        }
    }

    // Representative pixels: two dielectric sphere reflections, one upper
    // sphere/grazing region, and two background rays.
    const std::array<std::pair<int, int>, 5> probes = {{
        {65, 70}, {113, 73}, {91, 52}, {10, 12}, {170, 118}
    }};

    std::cout << "Representative linear analyzer intensities (R,G,B)\n";
    for (auto [x, y] : probes) {
        require(x >= 0 && x < PANEL_W && y >= 0 && y < PANEL_H,
                "probe coordinate out of range");
        vec3 D = renderer.camera_ray(x, y, PANEL_W, PANEL_H);
        pol::Vec3 camera_e1 = polarized_camera_reference(D, renderer.roll);
        PolarizedTraceResult trace = renderer.trace_with_count(D, &camera_e1);

        for (int a = 0; a < 3; ++a) {
            auto direct = pol::analyzer_intensities(trace.state.stokes,
                                                    angles[a]);
            const LinearPixel& stored = panels[a][y * PANEL_W + x];
            for (int ch = 0; ch < 3; ++ch) {
                require(std::isfinite(direct[ch]) && direct[ch] >= 0.0,
                        "linear intensity must be finite and non-negative");
                require(std::abs(direct[ch] - stored.rgb[ch]) < 1e-12,
                        "stored panel intensity disagrees with fresh trace");
            }
            std::cout << "px(" << x << ',' << y << ") theta="
                      << static_cast<int>(angles[a] * 180.0 / PI) << "  "
                      << direct[0] << ' ' << direct[1] << ' ' << direct[2]
                      << " events=" << trace.reflection_events << '\n';
        }
    }

    // Anchor checks for the same probes.  These are linear pre-gamma values,
    // regenerated from the deterministic scene above.  The broad tolerances
    // catch frame/sign or recursion regressions without being fragile to
    // harmless compiler optimization differences.
    struct Anchor { int x, y; int angle; std::array<double, 3> rgb; };
    const std::array<Anchor, 6> anchors = {{
        {65, 70, 0,   {0.0330, 0.0240, 0.0140}},
        {65, 70, 60,  {0.0122, 0.0076, 0.0255}},
        {113, 73, 120,{0.0175, 0.0239, 0.0274}},
        {91, 52, 0,   {0.0611, 0.0440, 0.0258}},
        {10, 12, 0,   {0.825, 0.600, 0.350}},
        {170, 118, 60, {0.521, 0.710, 0.812}}
    }};
    for (const Anchor& anchor : anchors) {
        int ai = anchor.angle == 0 ? 0 : (anchor.angle == 60 ? 1 : 2);
        const LinearPixel& p = panels[ai][anchor.y * PANEL_W + anchor.x];
        for (int ch = 0; ch < 3; ++ch) {
            double tol = 0.18 * std::max(0.02, anchor.rgb[ch]);
            require(std::abs(p.rgb[ch] - anchor.rgb[ch]) <= tol,
                    "representative pixel anchor changed beyond tolerance");
        }
    }

    // The panels must not be three identical scalar-dimming copies: rotating
    // the analyzer is expected to change at least one representative pixel.
    int differing_probes = 0;
    for (auto [x, y] : probes) {
        bool differs = false;
        for (int ch = 0; ch < 3; ++ch) {
            double v0 = panels[0][y * PANEL_W + x].rgb[ch];
            double v1 = panels[1][y * PANEL_W + x].rgb[ch];
            double v2 = panels[2][y * PANEL_W + x].rgb[ch];
            differs |= std::abs(v0 - v1) > 1e-4 || std::abs(v1 - v2) > 1e-4;
        }
        differing_probes += differs;
    }
    require(differing_probes >= 3,
            "analyzer rotation should change several representative pixels");

    write_ppm(path, panels);
    std::cout << "wrote " << path << " (" << TOTAL_W << 'x' << PANEL_H
              << ", analyzer angles 0/60/120 degrees)\n";
    return 0;
}
