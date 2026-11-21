#ifndef BEAMFILM_SCENE_H
#define BEAMFILM_SCENE_H

// Shared, header-only definition of the windowless beam-film example scene.
// Both examples/beamfilm.cpp and task_tests/beamfilm.cpp include it so the
// files on disk are always checked against the exact in-memory configuration
// that produced them. C++11 only, no extra translation unit required.

#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include "vector.h"
#include "material.h"
#include "objects.h"
#include "scene.h"
#include "camera.h"

struct BeamExampleConfig {
    int width;
    int height;
    int depth;
    Vec eye;
    Vec target;

    // Material constants, kept here so the tests can assert exact values.
    static Vec front_emission() { return Vec(0.10, 0.15, 0.20); }
    static Vec rear_emission()  { return Vec(1.20, 0.40, 0.10); }
    static Vec far_emission()   { return Vec(0.20, 0.80, 1.40); }
    static Vec mirror_rho()     { return Vec(0.80, 0.70, 0.60); }
    static Vec second_rho()     { return Vec(0.50, 0.50, 0.50); }
};

namespace beam_example_detail {

inline void write_obj(const std::string &path, const std::vector<Vec> &verts,
                      const std::vector<int> &faces) {
    std::ofstream out(path.c_str());
    for (size_t i = 0; i < verts.size(); ++i) {
        out << "v " << verts[i].x << " " << verts[i].y << " " << verts[i].z << "\n";
    }
    for (size_t i = 0; i < faces.size(); i += 3) {
        out << "f " << faces[i]+1 << " " << faces[i+1]+1 << " " << faces[i+2]+1 << "\n";
    }
}

} // namespace beam_example_detail

// Writes the OBJ files into dir and builds the scene/camera. The scene has:
//   * a dim front emitter wall with a narrow gap aligned to the mirror slit,
//     so rays through the fine slit escape the scene (miss area);
//   * a slanted mirror split by a fine slit whose edge is visible in the
//     film at sub-pixel width;
//   * a bright emitter behind the camera, visible only through the mirror;
//   * a small perpendicular mirror feeding a 45 degree second mirror and a
//     side emitter, producing a genuine two-reflection contribution.
inline Camera build_beam_example_scene(const std::string &dir, Scene &scene,
                                       BeamExampleConfig &cfg) {
    using beam_example_detail::write_obj;
    cfg.width = 6;
    cfg.height = 6;
    cfg.depth = 2;
    cfg.eye = Vec(0, -5, 0);
    cfg.target = Vec(0, 0, 0);

    const std::string front_l = dir + "/example_front_l.obj";
    const std::string front_r = dir + "/example_front_r.obj";
    const std::string slit_a = dir + "/example_slit_a.obj";
    const std::string slit_b = dir + "/example_slit_b.obj";
    const std::string rear = dir + "/example_rear.obj";
    const std::string mir_a = dir + "/example_mirror_a.obj";
    const std::string mir_b = dir + "/example_mirror_b.obj";
    const std::string side = dir + "/example_side.obj";

    // Slanted mirror plane through (0,-2,0), normal tilted 15 degrees about z.
    double th = 15.0 * M_PI / 180.0;
    Vec nm(sin(th), cos(th), 0.0);
    Vec uz(0, 0, 1);
    Vec tt = nm.cross(uz); // unit in-plane direction, x component cos(th)
    Vec p0(0, -2, 0);
    const double gap = 0.03; // fine slit measured along the in-plane t axis
    const double Lz = 30.0;
    // Left piece: t in [-30, -gap/2], right piece: t in [gap/2, 30].
    std::vector<Vec> la;
    la.push_back(p0 + tt*(-30.0) + uz*(-Lz));
    la.push_back(p0 + tt*(-gap/2.0) + uz*(-Lz));
    la.push_back(p0 + tt*(-gap/2.0) + uz*( Lz));
    la.push_back(p0 + tt*(-30.0) + uz*( Lz));
    write_obj(slit_a, la, std::vector<int>{0,1,2, 0,2,3});
    std::vector<Vec> lb;
    lb.push_back(p0 + tt*( gap/2.0) + uz*(-Lz));
    lb.push_back(p0 + tt*( 30.0) + uz*(-Lz));
    lb.push_back(p0 + tt*( 30.0) + uz*( Lz));
    lb.push_back(p0 + tt*( gap/2.0) + uz*( Lz));
    write_obj(slit_b, lb, std::vector<int>{0,1,2, 0,2,3});

    // Front emitter wall at y=4 with a gap that projects to the same screen
    // band as the mirror slit. Screen projection: x = 2 + X*4/(5-yp),
    // i.e. factor 4/3 at y=-2 and factor 2 at y=4.
    double x_mirror_edge = fabs(tt.x) * (gap/2.0);
    double x_cut = x_mirror_edge * (4.0/3.0) / 2.0;
    std::vector<Vec> fl;
    fl.push_back(Vec(-20,4,-20)); fl.push_back(Vec(-x_cut,4,-20));
    fl.push_back(Vec(-x_cut,4,20)); fl.push_back(Vec(-20,4,20));
    write_obj(front_l, fl, std::vector<int>{0,1,2, 0,2,3});
    std::vector<Vec> fr;
    fr.push_back(Vec(x_cut,4,-20)); fr.push_back(Vec(20,4,-20));
    fr.push_back(Vec(20,4,20)); fr.push_back(Vec(x_cut,4,20));
    write_obj(front_r, fr, std::vector<int>{0,1,2, 0,2,3});

    // Rear emitter on the camera side of the mirror, outside the direct view.
    write_obj(rear,
        std::vector<Vec>{Vec(-40,-9,-40),Vec(40,-9,-40),Vec(40,-9,40),Vec(-40,-9,40)},
        std::vector<int>{0,1,2, 0,2,3});

    // Small perpendicular mirror at y=-2 with a one-unit screen footprint in
    // a top-left corner, away from the central slit band.
    write_obj(mir_a,
        std::vector<Vec>{Vec(-1.875,-2,-1.875),Vec(-1.125,-2,-1.875),
                         Vec(-1.125,-2,-1.125),Vec(-1.875,-2,-1.125)},
        std::vector<int>{0,1,2, 0,2,3});
    // 45 degree mirror through (0,-12,0): turns a -y bundle towards +z.
    Vec pb(0, -12, 0), ux(1, 0, 0), ut(0, -1, 1);
    double L = 20.0;
    std::vector<Vec> mb;
    mb.push_back(pb + ux*(-L) + ut*(-L));
    mb.push_back(pb + ux*( L) + ut*(-L));
    mb.push_back(pb + ux*( L) + ut*( L));
    mb.push_back(pb + ux*(-L) + ut*( L));
    write_obj(mir_b, mb, std::vector<int>{0,1,2, 0,2,3});
    // Side emitter wall reached only after the two reflections.
    write_obj(side,
        std::vector<Vec>{Vec(-40,-60,10),Vec(40,-60,10),Vec(40,40,10),Vec(-40,40,10)},
        std::vector<int>{0,1,2, 0,2,3});

    // Slot order is part of the identity contract; tests rely on it.
    Mesh *front_l_mesh = new Mesh(Vec(), front_l.c_str(),
        Material(EMIT, Vec(1,1,1), BeamExampleConfig::front_emission()));
    Mesh *front_r_mesh = new Mesh(Vec(), front_r.c_str(),
        Material(EMIT, Vec(1,1,1), BeamExampleConfig::front_emission()));
    Mesh *rear_mesh = new Mesh(Vec(), rear.c_str(),
        Material(EMIT, Vec(1,1,1), BeamExampleConfig::rear_emission()));
    Mesh *side_mesh = new Mesh(Vec(), side.c_str(),
        Material(EMIT, Vec(1,1,1), BeamExampleConfig::far_emission()));
    Mesh *slit_a_mesh = new Mesh(Vec(), slit_a.c_str(),
        Material(IDEAL_MIRROR, BeamExampleConfig::mirror_rho(), Vec()));
    Mesh *slit_b_mesh = new Mesh(Vec(), slit_b.c_str(),
        Material(IDEAL_MIRROR, BeamExampleConfig::mirror_rho(), Vec()));
    Mesh *mirror_a = new Mesh(Vec(), mir_a.c_str(),
        Material(IDEAL_MIRROR, BeamExampleConfig::mirror_rho(), Vec()));
    Mesh *mirror_b = new Mesh(Vec(), mir_b.c_str(),
        Material(IDEAL_MIRROR, BeamExampleConfig::second_rho(), Vec()));

    scene.add(front_l_mesh);  // slot 0
    scene.add(front_r_mesh);  // slot 1
    scene.add(rear_mesh);     // slot 2
    scene.add(side_mesh);     // slot 3
    scene.add(slit_a_mesh);   // slot 4
    scene.add(slit_b_mesh);   // slot 5
    scene.add(mirror_a);      // slot 6
    scene.add(mirror_b);      // slot 7

    return Camera(cfg.eye, cfg.target, cfg.width, cfg.height);
}

#endif // BEAMFILM_SCENE_H
