#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "task_support/check.h"
#include "scene.h"
#include "camera.h"
#include "renderer.h"
#include "beamfilm.h"
#include "../examples/beamfilm_scene.h"

namespace {

const double TOL = 1e-8;

bool near_eq(double a, double b, double tol = TOL) {
    return fabs(a - b) <= tol;
}

void write_obj(const std::string &path, const std::vector<Vec> &v,
               const std::vector<int> &f) {
    std::ofstream o(path.c_str());
    for (size_t i = 0; i < v.size(); ++i)
        o << "v " << v[i].x << " " << v[i].y << " " << v[i].z << "\n";
    for (size_t i = 0; i < f.size(); i += 3)
        o << "f " << f[i]+1 << " " << f[i+1]+1 << " " << f[i+2]+1 << "\n";
}

std::string tmp_dir() {
    const char *d = std::getenv("TMPDIR");
    return std::string(d ? d : "/tmp/") + "/beamtest-";
}

// ----------------------------- events group -------------------------------

int ev_slit() {
    std::string b = tmp_dir();
    const double w = 0.02;
    std::vector<Vec> v;
    v.push_back(Vec(-10,3,-10)); v.push_back(Vec(-w,3,-10));
    v.push_back(Vec(-w,3,10));   v.push_back(Vec(-10,3,10));
    v.push_back(Vec(w,3,-10));   v.push_back(Vec(10,3,-10));
    v.push_back(Vec(10,3,10));   v.push_back(Vec(w,3,10));
    write_obj(b+"slit.obj", v, std::vector<int>{0,1,2,0,2,3,4,5,6,4,6,7});
    Scene sc;
    Mesh m(Vec(), (b+"slit.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(1,1,1)));
    sc.add(&m);
    Camera cam(Vec(0,-5,0), Vec(0,0,0), 4, 4);
    BeamTracer tr(sc, cam);
    BeamFilm f = tr.render_frame(0);
    double emit=0, miss=0;
    for (size_t i=0;i<f.pixels.size();++i) {
        emit += f.pixels[i].emitter_area;
        miss += f.pixels[i].miss_area;
    }
    TASK_CHECK(near_eq(emit+miss, 16.0));
    // Projection at y=3: X = 2(x-2); world gap [-w,w] -> screen width w.
    std::vector<ScreenPoint> gap{
        ScreenPoint(2.0-w/2.0,0.0), ScreenPoint(2.0+w/2.0,0.0),
        ScreenPoint(2.0+w/2.0,4.0), ScreenPoint(2.0-w/2.0,4.0)};
    BeamSubResult q = tr.query(gap, 0);
    TASK_CHECK(near_eq(q.area, w*4.0));
    TASK_CHECK(near_eq(q.miss_area, w*4.0));
    TASK_CHECK(near_eq(miss, w*4.0));
    TASK_CHECK(near_eq(emit, 16.0-w*4.0));
    return 0;
}

int ev_swap() {
    std::string b = tmp_dir();
    // Two overlapping triangles on intersecting planes; depth equality line
    // x=20/9 runs through pixel x=2, swapping the winner inside the pixel.
    write_obj(b+"a.obj",
        std::vector<Vec>{Vec(-3,1,-3),Vec(3,1,-3),Vec(3,1,3)},
        std::vector<int>{0,1,2});
    write_obj(b+"b.obj",
        std::vector<Vec>{Vec(-2,4.5,-6),Vec(2,-1.5,-6),Vec(2,-1.5,6)},
        std::vector<int>{0,1,2});
    Scene s;
    Mesh ma(Vec(), (b+"a.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(7,0,0)));
    Mesh mb(Vec(), (b+"b.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(0,5,0)));
    s.add(&ma); s.add(&mb);
    Camera cam(Vec(0,-5,0), Vec(0,0,0), 4, 4);
    BeamTracer tr(s, cam);
    BeamFilm f = tr.render_frame(0);
    const BeamPixel &p = f.pixels[2*4+2];
    double red = p.by_bounce[0].x/7.0;
    double green = p.by_bounce[0].y/5.0;
    TASK_CHECK(red > 0.0 && green > 0.0);
    TASK_CHECK(near_eq(red+green, p.emitter_area));
    // Single-ray winners on either side of the analytic contour x=20/9.
    unsigned short xi[3]={1,2,3};
    double cx = 20.0/9.0;
    Ray rl = cam.get_ray_continuous(cx-0.1, 2.5);
    Ray rr = cam.get_ray_continuous(cx+0.1, 2.5);
    Vec vl = s.trace_ray(rl, 0, xi), vr = s.trace_ray(rr, 0, xi);
    TASK_CHECK(near_eq(vl.x,7.0) && near_eq(vl.y,0.0));
    TASK_CHECK(near_eq(vr.x,0.0) && near_eq(vr.y,5.0));
    return 0;
}

int ev_shared_edge_and_dup() {
    std::string b = tmp_dir();
    write_obj(b+"edge.obj",
        std::vector<Vec>{Vec(-10,3,-10),Vec(10,3,-10),Vec(10,3,10),Vec(-10,3,10)},
        std::vector<int>{0,1,2, 0,2,3});
    Scene s1;
    Mesh e(Vec(), (b+"edge.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(2,2,2)));
    s1.add(&e);
    Camera cam(Vec(0,-5,0), Vec(0,0,0), 4, 4);
    BeamFilm f1 = BeamTracer(s1, cam).render_frame(0);
    double area=0; Vec rgb;
    for (size_t i=0;i<f1.pixels.size();++i) {
        area += f1.pixels[i].emitter_area;
        rgb = rgb + f1.pixels[i].by_bounce[0];
    }
    TASK_CHECK(near_eq(area, 16.0));
    TASK_CHECK(near_eq(rgb.x, 32.0));

    // Exact duplicate faces: distinct original face order, single coverage.
    std::ofstream o((b+"dup.obj").c_str());
    o << "v -10 3 -10\nv 10 3 -10\nv 10 3 10\nv -10 3 10\n";
    o << "f 1 2 3\nf 1 3 4\nf 1 2 3\nf 1 3 4\n";
    o.close();
    Scene s2;
    Mesh d(Vec(), (b+"dup.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(3,3,3)));
    s2.add(&d);
    TASK_CHECK(d.get_triangles().size() == 4);
    TASK_CHECK(d.get_triangles()[0]->face == 0);
    TASK_CHECK(d.get_triangles()[2]->face == 2);
    BeamFilm f2 = BeamTracer(s2, cam).render_frame(0);
    double a2=0; Vec c2;
    for (size_t i=0;i<f2.pixels.size();++i) {
        a2 += f2.pixels[i].emitter_area;
        c2 = c2 + f2.pixels[i].by_bounce[0];
    }
    TASK_CHECK(near_eq(a2, 16.0));
    TASK_CHECK(near_eq(c2.x, 48.0));
    return 0;
}

int ev_tie_and_kd() {
    std::string b = tmp_dir();
    // Coincident full-frame quads in two meshes (different slots): the
    // smaller slot must win every equal-depth tie.
    write_obj(b+"c.obj",
        std::vector<Vec>{Vec(-10,3,-10),Vec(10,3,-10),Vec(10,3,10),Vec(-10,3,10)},
        std::vector<int>{0,1,2, 0,2,3});
    Scene s;
    Mesh m0(Vec(), (b+"c.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(4,0,0)));
    Mesh m1(Vec(), (b+"c.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(0,6,0)));
    s.add(&m0); s.add(&m1);
    TASK_CHECK(m0.get_slot() == 0 && m1.get_slot() == 1);
    Camera cam(Vec(0,-5,0), Vec(0,0,0), 4, 4);
    BeamFilm f = BeamTracer(s, cam).render_frame(0);
    Vec total; double covered=0, miss=0;
    for (size_t i=0;i<f.pixels.size();++i) {
        total = total + f.pixels[i].by_bounce[0];
        covered += f.pixels[i].emitter_area;
        miss += f.pixels[i].miss_area;
    }
    // The coincident quads cover the whole frame; slot 0 wins every tie.
    TASK_CHECK(near_eq(covered, 16.0));
    TASK_CHECK(near_eq(miss, 0.0));
    TASK_CHECK(near_eq(total.x/4.0, 16.0));
    TASK_CHECK(near_eq(total.y, 0.0)); // slot 1 never wins the tie
    // Single-ray intersection identity also reports the smaller slot.
    Ray rr0 = cam.get_ray_continuous(2.0, 2.0);
    ObjectIntersection is0 = s.intersect(rr0);
    TASK_CHECK(is0.hit && is0.slot == 0);

    // Real multi-leaf KD tree with a distant box; culling must not lose area.
    std::vector<Vec> v; std::vector<int> idx; int k=0;
    for (int gx=-2; gx<=2; ++gx) for (int gy=-2; gy<=2; ++gy) {
        double x0=gx*0.4, x1=x0+0.3, z0=gy*0.4, z1=z0+0.3;
        v.push_back(Vec(x0,3,z0)); v.push_back(Vec(x1,3,z0));
        v.push_back(Vec(x1,3,z1)); v.push_back(Vec(x0,3,z1));
        idx.push_back(k); idx.push_back(k+1); idx.push_back(k+2);
        idx.push_back(k); idx.push_back(k+2); idx.push_back(k+3);
        k += 4;
    }
    v.push_back(Vec(400,3,-5)); v.push_back(Vec(410,3,-5));
    v.push_back(Vec(410,3,5)); v.push_back(Vec(400,3,5));
    idx.push_back(k); idx.push_back(k+1); idx.push_back(k+2);
    idx.push_back(k); idx.push_back(k+2); idx.push_back(k+3);
    write_obj(b+"grid.obj", v, idx);
    Scene sg;
    Mesh gm(Vec(), (b+"grid.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(1,1,1)));
    sg.add(&gm);
    unsigned long long tree_nodes = kd_tree_node_count(gm.get_kd_root());
    TASK_CHECK(tree_nodes > 1); // genuinely several leaves
    BeamTracer tr(sg, cam);
    // The grid projects to a small region around the screen centre; query a
    // tight convex domain around it instead of a whole corner pixel.
    std::vector<ScreenPoint> one{ScreenPoint(1.4,1.4),ScreenPoint(2.6,1.4),
                                 ScreenPoint(2.6,2.6),ScreenPoint(1.4,2.6)};
    BeamSubResult q = tr.query(one, 0);
    // 52 triangles exist; a tight frustum touches only a few leaves.
    TASK_CHECK(q.candidate_faces > 0);
    TASK_CHECK(q.candidate_faces < 52);
    TASK_CHECK(q.clipped_faces == q.candidate_faces);
    TASK_CHECK(q.nodes_visited > 0 && q.nodes_visited < tree_nodes);
    // The distant box lives in its own leaves; the tight frustum must cull
    // them entirely. Verify with a two-mesh scene: nearby grid plus a far box.
    write_obj(b+"farbox.obj",
        std::vector<Vec>{Vec(400,3,-5),Vec(410,3,-5),Vec(410,3,5),Vec(400,3,5)},
        std::vector<int>{0,1,2,0,2,3});
    Scene sf;
    Mesh gm2(Vec(), (b+"grid.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(1,1,1)));
    Mesh fbm(Vec(), (b+"farbox.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(1,1,1)));
    sf.add(&gm2); sf.add(&fbm);
    unsigned long long total_faces2 =
        gm2.get_triangles().size() + fbm.get_triangles().size();
    BeamSubResult qf = BeamTracer(sf, cam).query(one, 0);
    TASK_CHECK(qf.candidate_faces > 0);
    TASK_CHECK(qf.candidate_faces < total_faces2); // far box faces excluded
    // No area is lost despite the conservative traversal.
    BeamFilm ff = BeamTracer(sf, cam).render_frame(0);
    double frame_covered2=0;
    for (size_t i=0;i<ff.pixels.size();++i)
        frame_covered2 += ff.pixels[i].emitter_area + ff.pixels[i].miss_area;
    TASK_CHECK(near_eq(frame_covered2, 16.0));
    return 0;
}

int ev_conservative_frustum() {
    std::string b = tmp_dir();
    // Full-frame mirror at y=-2 and a parallel emitter wall strictly behind
    // it at y=-7. The virtual eye (y=1) lies inside every scene bounding box
    // and the wall straddles deeper segment start planes, exercising the
    // conservative "eye inside box" and near-plane culling paths.
    write_obj(b+"mir.obj",
        std::vector<Vec>{Vec(-20,-2,-20),Vec(20,-2,-20),Vec(20,-2,20),Vec(-20,-2,20)},
        std::vector<int>{0,1,2,0,2,3});
    write_obj(b+"wall.obj",
        std::vector<Vec>{Vec(-200,-7,-200),Vec(200,-7,-200),Vec(200,-7,200),Vec(-200,-7,200)},
        std::vector<int>{0,1,2,0,2,3});
    Camera cam(Vec(0,-5,0), Vec(0,0,0), 4, 4);

    Scene s;
    Mesh mi(Vec(), (b+"mir.obj").c_str(), Material(IDEAL_MIRROR,Vec(0.5,0.5,0.5),Vec()));
    Mesh em(Vec(), (b+"wall.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(1,2,3)));
    s.add(&mi); s.add(&em);
    BeamFilm f = BeamTracer(s, cam).render_frame(1);
    double b0=0, b1=0, miss=0, trunc=0;
    for (size_t i=0;i<f.pixels.size();++i) {
        b0 += f.pixels[i].emitter_by_bounce[0];
        b1 += f.pixels[i].emitter_by_bounce[1];
        miss += f.pixels[i].miss_area;
        trunc += f.pixels[i].truncated_area;
    }
    TASK_CHECK(near_eq(b0, 0.0));
    TASK_CHECK(near_eq(b1, 16.0));
    TASK_CHECK(near_eq(miss+trunc, 0.0));

    // Pointwise cross-check at interior points of every pixel.
    unsigned short xi[3]={5,5,5};
    double worst=0.0;
    for (int yy=0; yy<4; ++yy) for (int xx=0; xx<4; ++xx) {
        for (int r=0; r<4; ++r) {
            double sx = xx + 0.2 + 0.2*r;
            double sy = yy + 0.2 + 0.2*r;
            Ray ray = cam.get_ray_continuous(sx, sy);
            Vec tv = s.trace_ray(ray, 0, xi);
            worst = std::max(worst, fabs(tv.x-0.5)+fabs(tv.y-1.0)+fabs(tv.z-1.5));
        }
    }
    TASK_CHECK(near_eq(worst, 0.0, 1e-9));

    // A fully coplanar emitter on the mirror must not ghost into any
    // segment: strict-side near-plane clipping rejects it.
    Scene sg;
    Mesh mig(Vec(), (b+"mir.obj").c_str(), Material(IDEAL_MIRROR,Vec(0.5,0.5,0.5),Vec()));
    Mesh emg(Vec(), (b+"mir.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(9,9,9)));
    sg.add(&mig); sg.add(&emg);
    BeamFilm fg = BeamTracer(sg, cam).render_frame(1);
    double g=0;
    for (size_t i=0;i<fg.pixels.size();++i)
        g += fg.pixels[i].emitter_by_bounce[0] + fg.pixels[i].emitter_by_bounce[1];
    TASK_CHECK(near_eq(g, 0.0));

    // A quad ending exactly on the departure plane and extending in front of
    // the mirror cannot alter the reflected result, because its front part is
    // clipped away and its on-plane edge contributes no area.
    write_obj(b+"poke.obj",
        std::vector<Vec>{Vec(-0.3,-1.5,-0.3),Vec(0.3,-1.5,0.3),
                         Vec(0.3,-2.0,0.3),Vec(-0.3,-2.0,-0.3)},
        std::vector<int>{0,1,2,0,2,3});
    Scene sp;
    Mesh mip(Vec(), (b+"mir.obj").c_str(), Material(IDEAL_MIRROR,Vec(0.5,0.5,0.5),Vec()));
    Mesh emp(Vec(), (b+"wall.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(1,2,3)));
    Mesh poke(Vec(), (b+"poke.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(7,1,1)));
    sp.add(&mip); sp.add(&emp); sp.add(&poke);
    BeamFilm fp = BeamTracer(sp, cam).render_frame(1);
    double pb1=0, part=0;
    for (size_t i=0;i<fp.pixels.size();++i) {
        pb1 += fp.pixels[i].emitter_by_bounce[1];
        part += fp.pixels[i].emitter_area + fp.pixels[i].miss_area
              + fp.pixels[i].truncated_area;
    }
    TASK_CHECK(near_eq(pb1, 16.0));
    TASK_CHECK(near_eq(part, 16.0));
    return 0;
}


// --------------------------- transport group ------------------------------

int tr_one_bounce() {
    std::string b = tmp_dir();
    write_obj(b+"front.obj",
        std::vector<Vec>{Vec(-5,3,-5),Vec(5,3,-5),Vec(5,3,5),Vec(-5,3,5)},
        std::vector<int>{0,1,2,0,2,3});
    write_obj(b+"mir.obj",
        std::vector<Vec>{Vec(-0.375,-2,-0.375),Vec(0.375,-2,-0.375),
                         Vec(0.375,-2,0.375),Vec(-0.375,-2,0.375)},
        std::vector<int>{0,1,2,0,2,3});
    write_obj(b+"rear.obj",
        std::vector<Vec>{Vec(-5,-7,-5),Vec(5,-7,-5),Vec(5,-7,5),Vec(-5,-7,5)},
        std::vector<int>{0,1,2,0,2,3});
    Scene s;
    Mesh fm(Vec(), (b+"front.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(0.1,0.2,0.3)));
    Mesh rm(Vec(), (b+"rear.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(1,2,3)));
    Mesh mm(Vec(), (b+"mir.obj").c_str(), Material(IDEAL_MIRROR,Vec(0.5,0.25,0.125),Vec()));
    s.add(&fm); s.add(&rm); s.add(&mm);
    Camera cam(Vec(0,-5,0), Vec(0,0,0), 4, 4);
    BeamTracer tracer(s, cam);
    BeamFilm f = tracer.render_frame(2);
    Vec b0, b1; double ea=0, miss=0, trunc=0;
    for (size_t i=0;i<f.pixels.size();++i) {
        b0 = b0 + f.pixels[i].by_bounce[0];
        b1 = b1 + f.pixels[i].by_bounce[1];
        ea += f.pixels[i].emitter_area;
        miss += f.pixels[i].miss_area;
        trunc += f.pixels[i].truncated_area;
    }
    TASK_CHECK(near_eq(ea, 16.0) && near_eq(miss,0.0) && near_eq(trunc,0.0));
    TASK_CHECK(near_eq(b0.x, 1.5) && near_eq(b0.y,3.0) && near_eq(b0.z,4.5));
    TASK_CHECK(near_eq(b1.x,0.5) && near_eq(b1.y,0.5) && near_eq(b1.z,0.375));
    const BeamPixel &p = f.pixels[2*4+2];
    TASK_CHECK(near_eq(p.emitter_by_bounce[0],0.75));
    TASK_CHECK(near_eq(p.emitter_by_bounce[1],0.25));
    // D=0 truncates exactly the mirror square.
    BeamFilm f0 = tracer.render_frame(0);
    double t0=0;
    for (size_t i=0;i<f0.pixels.size();++i) t0 += f0.pixels[i].truncated_area;
    TASK_CHECK(near_eq(t0, 1.0));
    return 0;
}

int tr_slanted() {
    std::string b = tmp_dir();
    double th = 20.0*M_PI/180.0, sn=sin(th), cs=cos(th);
    Vec n(sn,cs,0), u(0,0,1), t = n.cross(u), p0(0,-2,0);
    double L=50.0;
    write_obj(b+"slant.obj",
        std::vector<Vec>{p0+t*(-L)+u*(-L), p0+t*(L)+u*(-L),
                         p0+t*(L)+u*(L), p0+t*(-L)+u*(L)},
        std::vector<int>{0,1,2,0,2,3});
    write_obj(b+"rear.obj",
        std::vector<Vec>{Vec(-300,-30,-300),Vec(300,-30,-300),
                         Vec(300,-30,300),Vec(-300,-30,300)},
        std::vector<int>{0,1,2,0,2,3});
    Scene s;
    Mesh rear(Vec(), (b+"rear.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(2,3,4)));
    Mesh mir(Vec(), (b+"slant.obj").c_str(), Material(IDEAL_MIRROR,Vec(0.7,0.6,0.5),Vec()));
    s.add(&rear); s.add(&mir);
    Camera cam(Vec(0,-5,0), Vec(0,0,0), 4, 4);
    BeamTracer tr(s, cam);
    BeamFilm f = tr.render_frame(2);
    double ea=0; Vec b1;
    for (size_t i=0;i<f.pixels.size();++i) {
        ea += f.pixels[i].emitter_area;
        b1 = b1 + f.pixels[i].by_bounce[1];
    }
    TASK_CHECK(near_eq(ea, 16.0));
    TASK_CHECK(near_eq(b1.x,16.0*0.7*2));
    TASK_CHECK(near_eq(b1.y,16.0*0.6*3));
    TASK_CHECK(near_eq(b1.z,16.0*0.5*4));
    unsigned short xi[3]={3,4,5};
    double worst=0.0;
    for (int r=0;r<25;++r) {
        Ray ray = cam.get_ray_continuous(0.23+3.5*r/24.0, 0.37+3.1*((r*11)%23)/23.0);
        Vec tv = s.trace_ray(ray, 0, xi);
        worst = std::max(worst, fabs(tv.x-0.7*2));
        worst = std::max(worst, fabs(tv.y-0.6*3));
        worst = std::max(worst, fabs(tv.z-0.5*4));
    }
    TASK_CHECK(near_eq(worst, 0.0, 1e-9));
    return 0;
}

int tr_two_bounce() {
    std::string b = tmp_dir();
    write_obj(b+"a.obj",
        std::vector<Vec>{Vec(-0.375,-2,-0.375),Vec(0.375,-2,-0.375),
                         Vec(0.375,-2,0.375),Vec(-0.375,-2,0.375)},
        std::vector<int>{0,1,2,0,2,3});
    Vec pb(0,-12,0), ux(1,0,0), ut(0,-1,1);
    double L=10.0;
    write_obj(b+"b.obj",
        std::vector<Vec>{pb+ux*(-L)+ut*(-L), pb+ux*(L)+ut*(-L),
                         pb+ux*(L)+ut*(L), pb+ux*(-L)+ut*(L)},
        std::vector<int>{0,1,2,0,2,3});
    write_obj(b+"r.obj",
        std::vector<Vec>{Vec(-30,-40,10),Vec(30,-40,10),Vec(30,30,10),Vec(-30,30,10)},
        std::vector<int>{0,1,2,0,2,3});
    Scene s;
    Mesh A(Vec(), (b+"a.obj").c_str(), Material(IDEAL_MIRROR,Vec(0.5,0.5,0.5),Vec()));
    Mesh B(Vec(), (b+"b.obj").c_str(), Material(IDEAL_MIRROR,Vec(0.25,0.5,0.75),Vec()));
    Mesh R(Vec(), (b+"r.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(1,2,4)));
    s.add(&A); s.add(&B); s.add(&R);
    Camera cam(Vec(0,-5,0), Vec(0,0,0), 4, 4);
    BeamTracer tr(s, cam);
    BeamFilm f = tr.render_frame(2);
    double a2=0, trunc=0; Vec c2;
    for (size_t i=0;i<f.pixels.size();++i) {
        a2 += f.pixels[i].emitter_by_bounce[2];
        trunc += f.pixels[i].truncated_area;
        c2 = c2 + f.pixels[i].by_bounce[2];
    }
    TASK_CHECK(near_eq(a2, 1.0));
    TASK_CHECK(near_eq(trunc, 0.0));
    TASK_CHECK(near_eq(c2.x,0.125) && near_eq(c2.y,0.5) && near_eq(c2.z,1.5));
    std::vector<ScreenPoint> sq{ScreenPoint(1.5,1.5),ScreenPoint(2.5,1.5),
                                ScreenPoint(2.5,2.5),ScreenPoint(1.5,2.5)};
    BeamSubResult q = tr.query(sq, 2);
    TASK_CHECK(near_eq(q.emitter_by_bounce[2],1.0));
    unsigned short xi[3]={9,9,9};
    double worst=0.0;
    for (int r=0;r<20;++r) {
        Ray ray = cam.get_ray_continuous(1.55+0.9*r/19.0, 1.55+0.9*((r*7)%19)/19.0);
        Vec tv = s.trace_ray(ray, 0, xi);
        Vec dens(q.integral[2].x, q.integral[2].y, q.integral[2].z);
        worst = std::max(worst, fabs(tv.x-dens.x));
        worst = std::max(worst, fabs(tv.y-dens.y));
        worst = std::max(worst, fabs(tv.z-dens.z));
    }
    TASK_CHECK(near_eq(worst, 0.0, 1e-9));
    BeamFilm f1 = tr.render_frame(1);
    double t1=0;
    for (size_t i=0;i<f1.pixels.size();++i) t1 += f1.pixels[i].truncated_area;
    TASK_CHECK(near_eq(t1, 1.0));
    return 0;
}

int tr_mutual_mirrors() {
    std::string b = tmp_dir();
    write_obj(b+"p1.obj",
        std::vector<Vec>{Vec(-100,-2,-100),Vec(100,-2,-100),Vec(100,-2,100),Vec(-100,-2,100)},
        std::vector<int>{0,1,2,0,2,3});
    write_obj(b+"p2.obj",
        std::vector<Vec>{Vec(-100,-12,-100),Vec(100,-12,-100),Vec(100,-12,100),Vec(-100,-12,100)},
        std::vector<int>{0,1,2,0,2,3});
    Scene s;
    Mesh p1(Vec(), (b+"p1.obj").c_str(), Material(IDEAL_MIRROR,Vec(1,1,1),Vec()));
    Mesh p2(Vec(), (b+"p2.obj").c_str(), Material(IDEAL_MIRROR,Vec(1,1,1),Vec()));
    s.add(&p1); s.add(&p2);
    Camera cam(Vec(0,-5,0), Vec(0,0,0), 4, 4);
    BeamTracer tr(s, cam);
    for (int D=0; D<=4; ++D) {
        BeamFilm f = tr.render_frame(D);
        double miss=0, trunc=0, emit=0;
        for (size_t i=0;i<f.pixels.size();++i) {
            miss += f.pixels[i].miss_area;
            trunc += f.pixels[i].truncated_area;
            emit += f.pixels[i].emitter_area;
        }
        TASK_CHECK(near_eq(emit,0.0));
        TASK_CHECK(near_eq(miss,0.0));
        TASK_CHECK(near_eq(trunc,16.0));
    }
    return 0;
}

int tr_double_sided_mirror() {
    std::string b = tmp_dir();
    // Full-frame square whose triangles use reversed winding, so the stored
    // geometric normal points away from the camera. Reflection must still
    // work from the back face.
    write_obj(b+"mir.obj",
        std::vector<Vec>{Vec(-20,-2,-20),Vec(20,-2,-20),Vec(20,-2,20),Vec(-20,-2,20)},
        std::vector<int>{2,1,0, 3,2,0});
    write_obj(b+"rear.obj",
        std::vector<Vec>{Vec(-200,-7,-200),Vec(200,-7,-200),
                         Vec(200,-7,200),Vec(-200,-7,200)},
        std::vector<int>{0,1,2,0,2,3});
    Scene s;
    Mesh mi(Vec(), (b+"mir.obj").c_str(), Material(IDEAL_MIRROR,Vec(0.5,0.5,0.5),Vec()));
    Mesh em(Vec(), (b+"rear.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(1,2,3)));
    s.add(&mi); s.add(&em);
    Camera cam(Vec(0,-5,0), Vec(0,0,0), 4, 4);
    BeamFilm f = BeamTracer(s, cam).render_frame(1);
    double b0=0, b1=0;
    for (size_t i=0;i<f.pixels.size();++i) {
        b0 += f.pixels[i].emitter_by_bounce[0];
        b1 += f.pixels[i].emitter_by_bounce[1];
    }
    TASK_CHECK(near_eq(b0, 0.0));
    TASK_CHECK(near_eq(b1, 16.0));
    unsigned short xi[3]={1,1,1};
    Ray r = cam.get_ray_continuous(2.0, 2.0);
    Vec tv = s.trace_ray(r, 0, xi);
    TASK_CHECK(near_eq(tv.x,0.5) && near_eq(tv.y,1.0) && near_eq(tv.z,1.5));
    return 0;
}

// ------------------------------- film group --------------------------------

int fm_subdomain_additivity() {
    std::string b = tmp_dir();
    write_obj(b+"f.obj",
        std::vector<Vec>{Vec(-5,3,-5),Vec(5,3,-5),Vec(5,3,5),Vec(-5,3,5)},
        std::vector<int>{0,1,2,0,2,3});
    write_obj(b+"m.obj",
        std::vector<Vec>{Vec(-0.375,-2,-0.375),Vec(0.375,-2,-0.375),
                         Vec(0.375,-2,0.375),Vec(-0.375,-2,0.375)},
        std::vector<int>{0,1,2,0,2,3});
    write_obj(b+"r.obj",
        std::vector<Vec>{Vec(-5,-7,-5),Vec(5,-7,-5),Vec(5,-7,5),Vec(-5,-7,5)},
        std::vector<int>{0,1,2,0,2,3});
    Scene s;
    Mesh f(Vec(), (b+"f.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(1,2,3)));
    Mesh r(Vec(), (b+"r.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(2,1,0.5)));
    Mesh m(Vec(), (b+"m.obj").c_str(), Material(IDEAL_MIRROR,Vec(0.5,0.5,0.5),Vec()));
    s.add(&f); s.add(&r); s.add(&m);
    Camera cam(Vec(0,-5,0), Vec(0,0,0), 4, 4);
    BeamTracer tr(s, cam);
    std::vector<ScreenPoint> whole{ScreenPoint(0,0),ScreenPoint(4,0),ScreenPoint(4,4),ScreenPoint(0,4)};
    std::vector<ScreenPoint> q1{ScreenPoint(0,0),ScreenPoint(2,0),ScreenPoint(2,4),ScreenPoint(0,4)};
    std::vector<ScreenPoint> q2{ScreenPoint(2,0),ScreenPoint(4,0),ScreenPoint(4,4),ScreenPoint(2,4)};
    BeamSubResult rw = tr.query(whole, 2);
    BeamSubResult r1 = tr.query(q1, 2);
    BeamSubResult r2 = tr.query(q2, 2);
    TASK_CHECK(near_eq(rw.area,16.0));
    TASK_CHECK(near_eq(r1.area+r2.area, rw.area));
    for (int k=0;k<=2;++k) for (int c=0;c<3;++c) {
        double wv = c==0?rw.integral[k].x:c==1?rw.integral[k].y:rw.integral[k].z;
        double v1 = c==0?r1.integral[k].x:c==1?r1.integral[k].y:r1.integral[k].z;
        double v2 = c==0?r2.integral[k].x:c==1?r2.integral[k].y:r2.integral[k].z;
        TASK_CHECK(near_eq(v1+v2, wv));
    }
    TASK_CHECK(near_eq(r1.miss_area+r2.miss_area, rw.miss_area));
    TASK_CHECK(near_eq(r1.truncated_area+r2.truncated_area, rw.truncated_area));
    return 0;
}

int fm_triangulation_invariance() {
    std::string b = tmp_dir();
    // Same square, two diagonal triangulations and a centroid fan.
    write_obj(b+"t1.obj",
        std::vector<Vec>{Vec(-10,3,-10),Vec(10,3,-10),Vec(10,3,10),Vec(-10,3,10)},
        std::vector<int>{0,1,2, 0,2,3});
    write_obj(b+"t2.obj",
        std::vector<Vec>{Vec(-10,3,-10),Vec(10,3,-10),Vec(10,3,10),Vec(-10,3,10)},
        std::vector<int>{0,1,3, 1,2,3});
    write_obj(b+"t3.obj",
        std::vector<Vec>{Vec(-10,3,-10),Vec(10,3,-10),Vec(10,3,10),Vec(-10,3,10),Vec(0,3,0)},
        std::vector<int>{0,1,4, 1,2,4, 2,3,4, 3,0,4});
    Camera cam(Vec(0,-5,0), Vec(0,0,0), 4, 4);
    BeamFilm films[3];
    for (int q=0;q<3;++q) {
        Scene s;
        Mesh mm(Vec(), (b+ (q==0?"t1":q==1?"t2":"t3") +".obj").c_str(),
                Material(EMIT,Vec(1,1,1),Vec(1.5,2.5,3.5)));
        s.add(&mm);
        films[q] = BeamTracer(s, cam).render_frame(0);
    }
    for (int i=0;i<16;++i) {
        for (int q=1;q<3;++q) {
            TASK_CHECK(near_eq(films[0].pixels[i].by_bounce[0].x,
                               films[q].pixels[i].by_bounce[0].x));
            TASK_CHECK(near_eq(films[0].pixels[i].emitter_area,
                               films[q].pixels[i].emitter_area));
        }
    }
    return 0;
}

int fm_channels_and_identities() {
    std::string b = tmp_dir();
    write_obj(b+"f.obj",
        std::vector<Vec>{Vec(-5,3,-5),Vec(5,3,-5),Vec(5,3,5),Vec(-5,3,5)},
        std::vector<int>{0,1,2,0,2,3});
    write_obj(b+"m.obj",
        std::vector<Vec>{Vec(-0.375,-2,-0.375),Vec(0.375,-2,-0.375),
                         Vec(0.375,-2,0.375),Vec(-0.375,-2,0.375)},
        std::vector<int>{0,1,2,0,2,3});
    write_obj(b+"r.obj",
        std::vector<Vec>{Vec(-5,-7,-5),Vec(5,-7,-5),Vec(5,-7,5),Vec(-5,-7,5)},
        std::vector<int>{0,1,2,0,2,3});
    Scene s;
    Mesh f(Vec(), (b+"f.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(1,1,1)));
    Mesh r(Vec(), (b+"r.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(1,1,1)));
    Mesh m(Vec(), (b+"m.obj").c_str(), Material(IDEAL_MIRROR,Vec(0.5,0.5,0.5),Vec()));
    s.add(&f); s.add(&r); s.add(&m);
    Camera cam(Vec(0,-5,0), Vec(0,0,0), 4, 4);
    BeamTracer tr(s, cam);
    BeamFilm film = tr.render_frame(2);
    TASK_CHECK(film.depth == 2 && (int)film.pixels[0].by_bounce.size() == 3);
    for (size_t i=0;i<film.pixels.size();++i) {
        const BeamPixel &p = film.pixels[i];
        Vec sum;
        double e=0;
        for (int k=0;k<=2;++k) {
            sum = sum + p.by_bounce[k];
            e += p.emitter_by_bounce[k];
        }
        TASK_CHECK(near_eq(sum.x,p.beauty.x,1e-12));
        TASK_CHECK(near_eq(sum.y,p.beauty.y,1e-12));
        TASK_CHECK(near_eq(sum.z,p.beauty.z,1e-12));
        TASK_CHECK(near_eq(e,p.emitter_area,1e-12));
        TASK_CHECK(near_eq(p.emitter_area+p.miss_area+p.truncated_area, 1.0));
        // Black emission still partitions the full area.
    }
    // Black emitter: radiance zero everywhere but areas still sum to one.
    write_obj(b+"black.obj",
        std::vector<Vec>{Vec(-10,3,-10),Vec(10,3,-10),Vec(10,3,10),Vec(-10,3,10)},
        std::vector<int>{0,1,2,0,2,3});
    Scene sb;
    Mesh bm(Vec(), (b+"black.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(0,0,0)));
    sb.add(&bm);
    BeamFilm fb = BeamTracer(sb, cam).render_frame(1);
    for (size_t i=0;i<fb.pixels.size();++i) {
        TASK_CHECK(near_eq(fb.pixels[i].emitter_area, 1.0));
        TASK_CHECK(near_eq(fb.pixels[i].beauty.x, 0.0));
    }
    // Zero reflectance mirror: with budget D=0 the first mirror hit still
    // counts the full truncated area; with budget left it reflects black
    // weight, so the reached rear emitter contributes area but zero radiance.
    Scene sz;
    Mesh zf(Vec(), (b+"f.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(1,1,1)));
    Mesh zr2m(Vec(), (b+"r.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(1,1,1)));
    Mesh zm(Vec(), (b+"m.obj").c_str(), Material(IDEAL_MIRROR,Vec(0,0,0),Vec()));
    sz.add(&zf); sz.add(&zr2m); sz.add(&zm);
    BeamFilm fz0 = BeamTracer(sz, cam).render_frame(0);
    double trunc0=0;
    for (size_t i=0;i<fz0.pixels.size();++i) trunc0 += fz0.pixels[i].truncated_area;
    TASK_CHECK(near_eq(trunc0, 1.0));
    BeamFilm fz = BeamTracer(sz, cam).render_frame(2);
    double trunc=0, black_area=0, black_rgb=0;
    for (size_t i=0;i<fz.pixels.size();++i) {
        trunc += fz.pixels[i].truncated_area;
        black_area += fz.pixels[i].emitter_by_bounce[1];
        black_rgb += fz.pixels[i].by_bounce[1].x + fz.pixels[i].by_bounce[1].y
                   + fz.pixels[i].by_bounce[1].z;
    }
    TASK_CHECK(near_eq(trunc, 0.0));
    TASK_CHECK(near_eq(black_area, 1.0)); // area is still integrated
    TASK_CHECK(near_eq(black_rgb, 0.0));  // ...but the colour is black
    return 0;
}

int fm_near_parallel_and_nonfinite() {
    std::string b = tmp_dir();
    write_obj(b+"f.obj",
        std::vector<Vec>{Vec(-5,3,-5),Vec(5,3,-5),Vec(5,3,5),Vec(-5,3,5)},
        std::vector<int>{0,1,2,0,2,3});
    Scene s;
    Mesh m(Vec(), (b+"f.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(1,2,3)));
    s.add(&m);
    Camera cam(Vec(0,-5,0), Vec(0,0,0), 4, 4);
    BeamTracer tr(s, cam);

    // A thin sliver domain: adjacent polygon edges are nearly parallel, so
    // some swept frustum planes degenerate. They must be skipped, not made
    // up, and the area must still integrate exactly (no lost region).
    double e = 1e-7;
    std::vector<ScreenPoint> sliver{
        ScreenPoint(0.3,0.3), ScreenPoint(3.7,0.3),
        ScreenPoint(3.7+e,3.7), ScreenPoint(0.3+e,3.7)};
    BeamSubResult q = tr.query(sliver, 0);
    double want = 0.0;
    {
        // Shoelace area computed independently.
        double a=0.0; size_t n=sliver.size();
        for (size_t i=0;i<n;++i) {
            const ScreenPoint &p=sliver[i], &r=sliver[(i+1)%n];
            a += p.x*r.y - r.x*p.y;
        }
        want = 0.5*fabs(a);
    }
    TASK_CHECK(near_eq(q.area, want, 1e-14));
    TASK_CHECK(near_eq(q.emitter_area + q.miss_area + q.truncated_area, want, 1e-9));
    TASK_CHECK(q.nodes_visited > 0);
    TASK_CHECK(q.candidate_faces > 0);

    // Non-finite screen coordinates are rejected.
    bool threw=false;
    try {
        std::vector<ScreenPoint> bad{ScreenPoint(0,0),ScreenPoint(1,0),
            ScreenPoint(std::numeric_limits<double>::infinity(),1),ScreenPoint(0,1)};
        tr.query(bad, 0);
    } catch (const std::invalid_argument&) { threw=true; }
    TASK_CHECK(threw);

    // Non-finite material data is rejected before rendering.
    write_obj(b+"inf.obj",
        std::vector<Vec>{Vec(-5,3,-5),Vec(5,3,-5),Vec(5,3,5)},
        std::vector<int>{0,1,2});
    Scene si;
    Mesh im(Vec(), (b+"inf.obj").c_str(),
            Material(EMIT, Vec(1,1,1),
                     Vec(std::numeric_limits<double>::quiet_NaN(),1.0,1.0)));
    si.add(&im);
    threw=false;
    try { BeamTracer(si, cam).render_frame(1); }
    catch (const std::invalid_argument&) { threw=true; }
    TASK_CHECK(threw);

    // Negative emission is rejected.
    Scene sn;
    Mesh nm2(Vec(), (b+"inf.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(-0.1,1,1)));
    sn.add(&nm2);
    threw=false;
    try { BeamTracer(sn, cam).render_frame(1); }
    catch (const std::invalid_argument&) { threw=true; }
    TASK_CHECK(threw);
    return 0;
}

int fm_invalid_inputs_and_state() {
    std::string b = tmp_dir();
    write_obj(b+"f.obj",
        std::vector<Vec>{Vec(-5,3,-5),Vec(5,3,-5),Vec(5,3,5),Vec(-5,3,5)},
        std::vector<int>{0,1,2,0,2,3});
    Scene s;
    Mesh m(Vec(), (b+"f.obj").c_str(), Material(EMIT,Vec(1,1,1),Vec(1,1,1)));
    s.add(&m);
    Camera cam(Vec(0,-5,0), Vec(0,0,0), 4, 4);
    BeamTracer tr(s, cam);
    bool threw=false;
    try { tr.render_frame(5); } catch (const std::invalid_argument&) { threw=true; }
    TASK_CHECK(threw);
    threw=false;
    try { tr.render_frame(-1); } catch (const std::invalid_argument&) { threw=true; }
    TASK_CHECK(threw);
    threw=false;
    try { std::vector<ScreenPoint> bad{ScreenPoint(0,0),ScreenPoint(1,0)};
          tr.query(bad, 2); } catch (const std::invalid_argument&) { threw=true; }
    TASK_CHECK(threw);
    threw=false;
    try { std::vector<ScreenPoint> bad{ScreenPoint(0,0),ScreenPoint(2,0),
                                       ScreenPoint(0,2),ScreenPoint(2,2)}; // bowtie
          tr.query(bad, 2); } catch (const std::invalid_argument&) { threw=true; }
    TASK_CHECK(threw);
    threw=false;
    try { std::vector<ScreenPoint> bad{ScreenPoint(0,0),ScreenPoint(1,0),
                                       ScreenPoint(1,1),ScreenPoint(0,1)};
          tr.query(bad, 9); } catch (const std::invalid_argument&) { threw=true; }
    TASK_CHECK(threw);
    // Sphere / DIFF / SPEC / bad colours are rejected before rendering.
    Scene ss; Sphere sp(Vec(),1.0,Material(EMIT,Vec(1,1,1),Vec(1,1,1)));
    ss.add(&sp);
    threw=false;
    try { BeamTracer(ss,cam).render_frame(1); } catch (const std::invalid_argument&) { threw=true; }
    TASK_CHECK(threw);
    Scene sd;
    Mesh dm(Vec(), (b+"f.obj").c_str(), Material(DIFF,Vec(1,1,1),Vec()));
    sd.add(&dm);
    threw=false;
    try { BeamTracer(sd,cam).render_frame(1); } catch (const std::invalid_argument&) { threw=true; }
    TASK_CHECK(threw);
    Scene sk;
    Mesh km(Vec(), (b+"f.obj").c_str(), Material(SPEC,Vec(1,1,1),Vec()));
    sk.add(&km);
    threw=false;
    try { BeamTracer(sk,cam).render_frame(1); } catch (const std::invalid_argument&) { threw=true; }
    TASK_CHECK(threw);
    Scene sr;
    Mesh rm2(Vec(), (b+"f.obj").c_str(),
             Material(IDEAL_MIRROR,Vec(1.5,0,0),Vec()));
    sr.add(&rm2);
    threw=false;
    try { BeamTracer(sr,cam).render_frame(1); } catch (const std::invalid_argument&) { threw=true; }
    TASK_CHECK(threw);
    // Failed render preserves the previous film; two tracers stay independent.
    BeamFilm good = tr.render_frame(1);
    int w_before = good.width;
    threw=false;
    try { tr.render_frame(9); } catch (const std::invalid_argument&) { threw=true; }
    TASK_CHECK(threw && tr.has_film() && tr.last_film().width == w_before);
    BeamTracer other(s, cam);
    TASK_CHECK(!other.has_film());
    // Bad camera dimensions.
    Camera bad_cam(Vec(0,-5,0), Vec(0,0,0), 0, 4);
    threw=false;
    try { BeamTracer(s,bad_cam).render_frame(1); } catch (const std::invalid_argument&) { threw=true; }
    TASK_CHECK(threw);
    Camera bad_cam2(Vec(0,-5,0), Vec(0,0,0), 4, -1);
    threw=false;
    try { BeamTracer(s,bad_cam2).render_frame(1); } catch (const std::invalid_argument&) { threw=true; }
    TASK_CHECK(threw);

    // Renderer instances keep independent beam state; a failed render_beam
    // leaves the previously stored film untouched.
    Renderer r1(&s, &cam);
    const BeamFilm &rf1 = r1.render_beam(1);
    Renderer r2(&s, &cam);
    TASK_CHECK(!r2.has_beam_film());
    const BeamFilm &rf2 = r2.render_beam(2);
    TASK_CHECK(r1.beam_film().depth == 1 && rf1.depth == 1);
    TASK_CHECK(r2.beam_film().depth == 2 && rf2.depth == 2);
    Scene bad_scene; Sphere sp2(Vec(), 1.0, Material(EMIT,Vec(1,1,1),Vec(1,1,1)));
    bad_scene.add(&sp2);
    Renderer r3(&bad_scene, &cam);
    threw=false;
    try { r3.render_beam(1); } catch (const std::invalid_argument&) { threw=true; }
    TASK_CHECK(threw && !r3.has_beam_film());
    return 0;
}

int fm_continuous_and_example_files() {
    // Integer continuous rays match get_ray(x,y,false,...) bit-for-bit.
    Camera cam(Vec(0,-5,0), Vec(0,0,0), 5, 3);
    unsigned short xi[3]={1,2,3};
    for (int y=0;y<3;++y) for (int x=0;x<5;++x) {
        Ray a = cam.get_ray(x,y,false,xi);
        Ray b = cam.get_ray_continuous((double)x,(double)y);
        TASK_CHECK(near_eq(a.direction.x,b.direction.x,0.0));
        TASK_CHECK(near_eq(a.direction.y,b.direction.y,0.0));
        TASK_CHECK(near_eq(a.direction.z,b.direction.z,0.0));
    }

    // Rebuild the exact example scene in memory and compare against the files
    // written by examples/beamfilm.cpp in this run. The in-memory scene uses
    // its own scratch directory; the on-disk files live in out_dir.
    const std::string out_dir = ".task-build/beamfilm-output";
    const std::string scratch = tmp_dir() + "example-scratch/";
    #ifdef _WIN32
    std::string mkdir_cmd = "if not exist " + scratch + " mkdir " + scratch;
    #else
    std::string mkdir_cmd = "mkdir -p " + scratch;
    #endif
    int rc = std::system(mkdir_cmd.c_str());
    (void)rc;
    Scene scene; BeamExampleConfig cfg;
    Camera camera = build_beam_example_scene(scratch, scene, cfg);
    Renderer renderer(&scene, &camera);
    const BeamFilm &film = renderer.render_beam(cfg.depth);

    // PPM exists and has the expected header/dimensions and byte count.
    std::ifstream ppm((out_dir+"/image.ppm").c_str(), std::ios::binary);
    TASK_CHECK(ppm.good());
    std::string magic; int pw, ph, maxv;
    ppm >> magic >> pw >> ph >> maxv;
    TASK_CHECK(magic == "P6" && pw == film.width && ph == film.height && maxv == 255);
    ppm.get(); // single whitespace after max value
    std::vector<unsigned char> rgb((size_t)pw*ph*3);
    ppm.read(reinterpret_cast<char*>(&rgb[0]), (std::streamsize)rgb.size());
    TASK_CHECK((size_t)ppm.gcount() == rgb.size());

    // CSV: one row per pixel per bounce, exact round trip of all channels.
    std::ifstream csv((out_dir+"/channels.csv").c_str());
    TASK_CHECK(csv.good());
    size_t rows = 0;
    std::string line;
    while (std::getline(csv, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string tok[9];
        for (int i=0;i<9;++i) std::getline(ss, tok[i], ',');
        int y=atoi(tok[0].c_str()), x=atoi(tok[1].c_str()), k=atoi(tok[2].c_str());
        double r=atof(tok[3].c_str()), g=atof(tok[4].c_str()), bb=atof(tok[5].c_str());
        double ea=atof(tok[6].c_str()), ma=atof(tok[7].c_str()), ta=atof(tok[8].c_str());
        TASK_CHECK(y>=0 && y<film.height && x>=0 && x<film.width);
        TASK_CHECK(k>=0 && k<=film.depth);
        const BeamPixel &p = film.pixels[(size_t)y*film.width+x];
        TASK_CHECK(near_eq(r,p.by_bounce[k].x,1e-15));
        TASK_CHECK(near_eq(g,p.by_bounce[k].y,1e-15));
        TASK_CHECK(near_eq(bb,p.by_bounce[k].z,1e-15));
        if (k == 0) {
            TASK_CHECK(near_eq(ea,p.emitter_area,1e-15));
            TASK_CHECK(near_eq(ma,p.miss_area,1e-15));
            TASK_CHECK(near_eq(ta,p.truncated_area,1e-15));
        } else {
            TASK_CHECK(near_eq(ea,0.0) && near_eq(ma,0.0) && near_eq(ta,0.0));
        }
        ++rows;
    }
    TASK_CHECK(rows == (size_t)film.width*film.height*(film.depth+1));

    // The example must really exercise every feature: slit miss, off-view
    // one-bounce emission and a genuine two-bounce channel.
    double miss=0, b1=0, b2=0;
    for (size_t i=0;i<film.pixels.size();++i) {
        miss += film.pixels[i].miss_area;
        b1 += film.pixels[i].emitter_by_bounce[1];
        b2 += film.pixels[i].emitter_by_bounce[2];
    }
    TASK_CHECK(miss > 0.0);
    TASK_CHECK(b1 > 0.0);
    TASK_CHECK(b2 > 0.0);
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"events", "fine_slit", ev_slit},
    {"events", "depth_swap", ev_swap},
    {"events", "shared_edge_and_duplicates", ev_shared_edge_and_dup},
    {"events", "tie_order_and_kd_culling", ev_tie_and_kd},
    {"events", "conservative_frustum", ev_conservative_frustum},
    {"transport", "one_bounce_handcalc", tr_one_bounce},
    {"transport", "slanted_mirror", tr_slanted},
    {"transport", "two_reflections", tr_two_bounce},
    {"transport", "mutual_mirrors", tr_mutual_mirrors},
    {"transport", "double_sided_mirror", tr_double_sided_mirror},
    {"film", "subdomain_additivity", fm_subdomain_additivity},
    {"film", "near_parallel_and_nonfinite", fm_near_parallel_and_nonfinite},
    {"film", "triangulation_invariance", fm_triangulation_invariance},
    {"film", "channel_identities", fm_channels_and_identities},
    {"film", "invalid_inputs_and_state", fm_invalid_inputs_and_state},
    {"film", "continuous_rays_and_example_files", fm_continuous_and_example_files},
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
