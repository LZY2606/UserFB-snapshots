// Windowless analytic beam-film example.
//
// Usage:
//   beamfilm <output-directory>
//
// Generates an OBJ-only scene (fine-slit slanted mirror, emitters outside
// the direct camera view and a two-reflection path), renders it with the
// analytic beam film through the normal Renderer entry point, and writes:
//   <output-directory>/image.ppm     beauty image (P6)
//   <output-directory>/channels.csv  per-pixel/per-bounce channel table
//
// No windowing, sampling or fixed-resolution intermediate buffers are used.

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "scene.h"
#include "camera.h"
#include "renderer.h"
#include "beamfilm.h"
#include "beamfilm_scene.h"

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <output-directory>\n";
        return 2;
    }
    const std::string dir = argv[1];

    Scene scene;
    BeamExampleConfig cfg;
    Camera camera = build_beam_example_scene(dir, scene, cfg);

    Renderer renderer(&scene, &camera);
    const BeamFilm &film = renderer.render_beam(cfg.depth);

    const std::string ppm_path = dir + "/image.ppm";
    const std::string csv_path = dir + "/channels.csv";
    beamfilm_write_ppm(film, ppm_path);
    beamfilm_write_csv(film, csv_path);

    double emit = 0.0, miss = 0.0, trunc = 0.0;
    for (size_t i = 0; i < film.pixels.size(); ++i) {
        emit += film.pixels[i].emitter_area;
        miss += film.pixels[i].miss_area;
        trunc += film.pixels[i].truncated_area;
    }
    std::cout << "beamfilm " << film.width << "x" << film.height
              << " D=" << film.depth << "\n"
              << "emitter_area=" << emit << " miss_area=" << miss
              << " truncated_area=" << trunc << "\n"
              << "kd nodes_visited=" << film.nodes_visited
              << " candidate_faces=" << film.candidate_faces
              << " clipped_faces=" << film.clipped_faces << "\n"
              << "wrote " << ppm_path << "\n"
              << "wrote " << csv_path << "\n";
    return 0;
}
