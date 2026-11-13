// Polarized specular reflection demo.
//
// Renders one sphere scene three times side by side through an ideal linear
// analyzer at three orientations.  The full Stokes state is traced once per
// camera ray; only the analyzer angle differs between the panels, so turning
// the analyzer reveals how the reflections reshaped Q and U rather than a
// scalar RGB multiplier.  The program also verifies the linear intensity
// invariants at representative pixels before writing the PPM.
//
// Usage: polarized_reflections <output.ppm>

#include "polarization.h"
#include "polarized_renderer.h"
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using pol::PolarState;
using pol_render::CameraRay;
using pol_render::PolarScene;
using pol_render::PolarSphere;

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr int WIDTH = 480;
constexpr int HEIGHT = 360;
constexpr double FOV = 1.05;
const double ANGLES[3] = {0.0, PI / 4.0, PI / 2.0};

PolarScene build_scene() {
    PolarScene scene;
    scene.max_reflections = 6;

    // Two glass spheres with distinct outside/inside indices; reflection
    // never crosses an interface, so each sphere carries both indices and the
    // hit side decides which one is n1.
    PolarSphere a;
    a.sphere.center = {-1.2f, -0.6f, -10.0f};
    a.sphere.radius = 2.0f;
    a.n_outside = 1.0;
    a.n_inside = 1.5;
    PolarSphere b;
    b.sphere.center = {2.2f, 0.8f, -13.0f};
    b.sphere.radius = 2.6f;
    b.n_outside = 1.0;
    b.n_inside = 1.33;
    scene.spheres.push_back(a);
    scene.spheres.push_back(b);

    // Partially polarized background, identical definition per direction and
    // interpreted in the deterministic world-projected reference frame.
    for (int ch = 0; ch < 3; ++ch) {
        double base = 0.55 - 0.05 * ch;
        scene.background.stokes[ch] = {base, 0.25 * base, 0.10 * base, 0.0};
    }
    return scene;
}

CameraRay camera_ray(int px, int py) {
    double dx = (px + 0.5) - WIDTH / 2.0;
    double dy = -(py + 0.5) + HEIGHT / 2.0; // flip the image vertically
    double dz = -HEIGHT / (2.0 * std::tan(FOV / 2.0));
    return CameraRay{{0, 0, 0}, dvec3{dx, dy, dz}.normalized()};
}

std::uint8_t to_byte(double v) {
    if (v < 0.0) v = 0.0;
    v = std::pow(v, 1.0 / 2.2); // linear light -> display gamma
    if (v > 1.0) v = 1.0;
    return static_cast<std::uint8_t>(255.0 * v);
}

void require(bool ok, const std::string& msg) {
    if (!ok) throw std::runtime_error(msg);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <output.ppm>\n";
        return 2;
    }

    PolarScene scene = build_scene();

    // One trace per pixel; analyzer panels share the identical Stokes state.
    std::vector<PolarState> states(WIDTH * HEIGHT);
    for (int py = 0; py < HEIGHT; ++py)
        for (int px = 0; px < WIDTH; ++px)
            states[py * WIDTH + px] = scene.trace(camera_ray(px, py));

    // Representative-pixel checks on the linear (pre-tonemap) intensities.
    const int probes[][2] = {{4, 4}, {WIDTH / 2, HEIGHT / 2}, {WIDTH - 5, HEIGHT - 5}};
    for (const auto& q : probes) {
        const PolarState& st = states[q[1] * WIDTH + q[0]];
        for (int ch = 0; ch < 3; ++ch) {
            const auto& s = st.s[ch];
            double i0 = pol::analyzer_intensity(s, ANGLES[0]);
            double i90 = pol::analyzer_intensity(s, ANGLES[2]);
            // Orthogonal analyzers partition the total intensity.
            require(std::abs((i0 + i90) - s[pol::I]) < 1e-10,
                    "orthogonal analyzer intensities must sum to I");
            // Analyzer never creates energy.
            require(i0 >= -1e-12 && i0 <= s[pol::I] + 1e-12, "analyzer is passive");
            // The full-state entry point and the scalar formula agree.
            require(std::abs(pol::linear_analyzer(s, ANGLES[1])[pol::I] -
                             pol::analyzer_intensity(s, ANGLES[1])) < 1e-12,
                    "analyzer state and scalar intensity disagree");
            // Repeated traces are frame-independent: rerun the same ray.
            PolarState again = scene.trace(camera_ray(q[0], q[1]));
            require(std::abs(again.s[ch][pol::I] - s[pol::I]) < 1e-12,
                    "repeated trace must be deterministic");
        }
    }
    // A corner ray misses every sphere and returns the background unchanged.
    const PolarState& corner = states[0];
    for (int ch = 0; ch < 3; ++ch)
        require(std::abs(corner.s[ch][pol::I] - scene.background.stokes[ch][pol::I]) < 1e-12,
                "miss ray returns background intensity");

    std::ofstream out(argv[1], std::ios::binary);
    if (!out) throw std::runtime_error(std::string("cannot open ") + argv[1]);
    out << "P6\n" << (3 * WIDTH) << " " << HEIGHT << "\n255\n";
    for (int py = 0; py < HEIGHT; ++py) {
        for (int px = 0; px < WIDTH; ++px) {
            const PolarState& st = states[py * WIDTH + px];
            for (int panel = 0; panel < 3; ++panel) {
                std::uint8_t rgb[3];
                for (int ch = 0; ch < 3; ++ch)
                    rgb[ch] = to_byte(pol::analyzer_intensity(st.s[ch], ANGLES[panel]));
                out.write(reinterpret_cast<char*>(rgb), 3);
            }
        }
    }
    require(out.good(), "failed while writing PPM");

    std::cout << "wrote " << argv[1] << " (" << (3 * WIDTH) << "x" << HEIGHT
              << "), analyzer angles 0/45/90 deg\n";
    std::cout << "center pixel linear I per channel (0deg):";
    const PolarState& center = states[(HEIGHT / 2) * WIDTH + WIDTH / 2];
    for (int ch = 0; ch < 3; ++ch)
        std::cout << " " << pol::analyzer_intensity(center.s[ch], 0.0);
    std::cout << "\n";
    return 0;
}
