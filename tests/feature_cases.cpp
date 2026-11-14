#include <cmath>
#include <fstream>
#include <string>
#include <vector>
#include "../task_support/check.h"
#include "our_gl.h"
#include "model.h"
#include "feature_edges.h"
#include "tgaimage.h"

extern mat<4,4> ModelView, Perspective;
extern std::vector<double> zbuffer;

namespace {

constexpr int W = 64, H = 64;

void write_file(const std::string &path, const std::string &text) {
    std::ofstream out(path);
    out << text;
}

void setup_camera(double ex, double ey, double ez) {
    const vec3 eye{ex, ey, ez};
    lookat(eye, {0., 0., 0.}, {0., 1., 0.});
    init_perspective(norm(eye));
    init_viewport(0, 0, W, H);
    init_zbuffer(W, H);
}

struct FlatShader : IShader {
    bool drop;
    TGAColor col;
    FlatShader(bool d, TGAColor c) : drop(d), col(c) {}
    std::pair<bool,TGAColor> fragment(const vec3) const override { return {drop, col}; }
};

struct TexturedShader : IShader {
    const Model &model;
    bool drop;
    TexturedShader(const Model &m, bool d) : model(m), drop(d) {}
    std::pair<bool,TGAColor> fragment(const vec3) const override {
        return {drop, sample2D(model.diffuse(), {0.5, 0.5})};
    }
};

bool same_color(const TGAColor &a, const TGAColor &b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

const TGAColor BG{{255, 255, 255, 255}};
const TGAColor BLUE{{255, 0, 0, 255}};
const TGAColor GREEN{{0, 255, 0, 255}};
const TGAColor RED{{0, 0, 255, 255}};
const TGAColor BLACK{{0, 0, 0, 255}};

std::pair<int,int> key(int a, int b) { return a < b ? std::make_pair(a, b) : std::make_pair(b, a); }

const FeatureEdge* find_static(const FeatureEdgeSet &s, int a, int b) {
    const auto k = key(a, b);
    for (const FeatureEdge &e : s.edges)
        if (e.a == k.first && e.b == k.second) return &e;
    return nullptr;
}

const EdgeAdjacency* find_adj(const FeatureEdgeSet &s, int a, int b) {
    const auto k = key(a, b);
    for (const EdgeAdjacency &e : s.adjacency)
        if (e.a == k.first && e.b == k.second) return &e;
    return nullptr;
}

bool has_face(const FeatureEdge &e, int face) {
    for (int f : e.faces) if (f == face) return true;
    return false;
}

// A planar quad triangulated into two triangles. Every corner gets its own UV
// index, so UV seams split the corners while the positions stay shared.
const char *kPlaneSplitUV =
    "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
    "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvt .2 .2\nvt .8 .8\n"
    "vn 0 0 1\n"
    "f 1/1/1 2/2/1 3/3/1\n"
    "f 1/4/1 3/5/1 4/6/1\n";

// Two coplanar triangles: vertex 4 has the same coordinates as vertex 1 but a
// distinct OBJ position index, so nothing may be welded.
const char *kDuplicatePositions =
    "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 0\n"
    "vt 0 0\nvt 1 0\nvt 0 1\nvt .5 .5\nvt .7 .7\nvt .3 .3\n"
    "vn 0 0 1\n"
    "f 1/1/1 2/2/1 3/3/1\n"
    "f 4/4/1 2/5/1 3/6/1\n";

// Two triangles sharing edge (1,2), geometric normals 90 degrees apart.
const char *kFolded =
    "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 -1\n"
    "vt 0 0\nvt 1 0\nvt 0 1\nvt .1 .1\nvt .9 .1\nvt .5 .9\n"
    "vn 0 0 1\n"
    "f 1/1/1 2/2/1 3/3/1\n"
    "f 1/4/1 2/5/1 4/6/1\n";

// A valid triangle followed by a zero-area triangle.
const char *kZeroArea =
    "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
    "vt 0 0\nvt 1 0\nvt 0 1\nvt .5 .5\nvt .5 .5\n"
    "vn 0 0 1\n"
    "f 1/1/1 2/2/1 3/3/1\n"
    "f 2/4/1 3/5/1 3/5/1\n";

// Three valid triangles sharing the same position edge (1,2).
const char *kThreeNeighbors =
    "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\nv 0 -1 0\n"
    "vt 0 0\nvt 1 0\nvt 0 1\nvt .1 .1\nvt .9 .1\nvt .5 .9\nvt .2 .2\nvt .8 .2\nvt .5 .5\n"
    "vn 0 0 1\n"
    "f 1/1/1 2/2/1 3/3/1\n"
    "f 1/4/1 2/5/1 4/6/1\n"
    "f 1/7/1 2/8/1 5/9/1\n";

// Convex 90 degree corner: face normals are (0,0,-1) and (-1,0,0).
const char *kCorner =
    "v 0 0 0\nv 0 1 0\nv -1 0 0\nv 0 0 -1\n"
    "vt 0 0\nvt 1 0\nvt 0 1\nvt .5 .5\nvt .5 .5\nvt .5 .5\n"
    "vn 0 0 1\n"
    "f 1/1/1 3/2/1 2/3/1\n"
    "f 1/4/1 2/5/1 4/6/1\n";

int topology_plane_split_uv() {
    write_file(".task-build/t_plane.obj", kPlaneSplitUV);
    Model model(".task-build/t_plane.obj");
    FeatureEdgeSet s = classify_feature_edges(model, 30.);
    TASK_CHECK(model.nfaces() == 2);
    TASK_CHECK(s.adjacency.size() == 5);          // four borders + one triangulation diagonal
    TASK_CHECK(s.edges.size() == 4);              // the diagonal must not show
    TASK_CHECK(find_static(s, 0, 2) == nullptr);  // internal diagonal absent from the line set
    const EdgeAdjacency *diag = find_adj(s, 0, 2);
    TASK_CHECK(diag && diag->faces.size() == 2);  // adjacency still knows both owning faces
    TASK_CHECK(s.valid_face[0] && s.valid_face[1]);
    TASK_CHECK(std::abs(s.face_normal[0].z - 1.) < 1e-12);
    TASK_CHECK(std::abs(s.face_normal[1].z - 1.) < 1e-12);
    const FeatureEdge *border = find_static(s, 1, 2); // original endpoint indices and face number
    TASK_CHECK(border && border->kind == FEATURE_BOUNDARY && border->faces.size() == 1 &&
               has_face(*border, 0));
    return 0;
}

int topology_duplicate_positions_not_welded() {
    write_file(".task-build/t_dup.obj", kDuplicatePositions);
    Model model(".task-build/t_dup.obj");
    FeatureEdgeSet s = classify_feature_edges(model, 30.);
    TASK_CHECK(s.adjacency.size() == 5);          // coincident indices 0 and 3 stay separate
    const EdgeAdjacency *e01 = find_adj(s, 0, 1);
    const EdgeAdjacency *e13 = find_adj(s, 1, 3);
    TASK_CHECK(e01 && e01->faces.size() == 1);    // two geometrically identical edges
    TASK_CHECK(e13 && e13->faces.size() == 1);    // are not merged into one
    const EdgeAdjacency *shared = find_adj(s, 1, 2);
    TASK_CHECK(shared && shared->faces.size() == 2);
    TASK_CHECK(find_static(s, 1, 2) == nullptr);  // coplanar shared edge is no crease
    TASK_CHECK(s.edges.size() == 4);              // four boundary edges, no diagonal/crease
    return 0;
}

int topology_folded_crease() {
    write_file(".task-build/t_fold.obj", kFolded);
    Model model(".task-build/t_fold.obj");
    FeatureEdgeSet loose = classify_feature_edges(model, 30.);
    const FeatureEdge *crease = find_static(loose, 0, 1);
    TASK_CHECK(crease && crease->kind == FEATURE_CREASE);
    TASK_CHECK(crease->faces.size() == 2 && has_face(*crease, 0) && has_face(*crease, 1));
    TASK_CHECK(loose.edges.size() == 5);          // one crease plus four boundaries
    FeatureEdgeSet tight = classify_feature_edges(model, 90.);
    TASK_CHECK(find_static(tight, 0, 1) == nullptr);  // 90 degrees is not strictly greater than 90
    TASK_CHECK(tight.edges.size() == 4);
    FeatureEdgeSet just_under = classify_feature_edges(model, 89.9);
    TASK_CHECK(find_static(just_under, 0, 1) != nullptr);
    return 0;
}

int topology_zero_area_excluded() {
    write_file(".task-build/t_zero.obj", kZeroArea);
    Model model(".task-build/t_zero.obj");
    FeatureEdgeSet s = classify_feature_edges(model, 30.);
    TASK_CHECK(model.nfaces() == 2);
    TASK_CHECK(s.valid_face[0] == 1 && s.valid_face[1] == 0);
    TASK_CHECK(norm(s.face_normal[1]) == 0.);
    TASK_CHECK(s.adjacency.size() == 3);          // degenerate face contributes nothing
    for (const EdgeAdjacency &e : s.adjacency) TASK_CHECK(e.faces.size() == 1);
    TASK_CHECK(s.edges.size() == 3);
    return 0;
}

int topology_three_neighbors_is_crease() {
    write_file(".task-build/t_three.obj", kThreeNeighbors);
    Model model(".task-build/t_three.obj");
    FeatureEdgeSet s = classify_feature_edges(model, 30.);
    TASK_CHECK(s.adjacency.size() == 7);
    const FeatureEdge *e = find_static(s, 0, 1);
    TASK_CHECK(e && e->kind == FEATURE_CREASE);
    TASK_CHECK(e->faces.size() == 3);
    TASK_CHECK(has_face(*e, 0) && has_face(*e, 1) && has_face(*e, 2));
    return 0;
}

int visibility_silhouette_moves_with_eye() {
    write_file(".task-build/t_corner.obj", kCorner);
    Model model(".task-build/t_corner.obj");
    FeatureEdgeSet s = classify_feature_edges(model, 95.); // 90 degree fold is below the threshold
    TASK_CHECK(find_static(s, 0, 1) == nullptr);
    setup_camera(10., 0., 10.);                            // both faces point away
    std::vector<FeatureEdge> hidden = classify_view_edges(s, model, {10., 0., 10.});
    TASK_CHECK(hidden.empty());
    setup_camera(-10., 0., 10.);                           // one face turns toward the viewer
    std::vector<FeatureEdge> shown = classify_view_edges(s, model, {-10., 0., 10.});
    TASK_CHECK(shown.size() == 1);
    TASK_CHECK(shown[0].kind == FEATURE_SILHOUETTE);
    TASK_CHECK(shown[0].a == 0 && shown[0].b == 1);
    TASK_CHECK(shown[0].faces.size() == 2);
    return 0;
}

int visibility_width_and_round_caps() {
    write_file(".task-build/t_tri.obj",
        "v -.5 -.5 0\nv .5 -.5 0\nv 0 .5 0\nvt 0 0\nvt 1 0\nvt .5 1\nvn 0 0 1\n"
        "f 1/1/1 2/2/1 3/3/1\n");
    Model model(".task-build/t_tri.obj");
    FeatureEdgeSet s = classify_feature_edges(model, 30.);
    setup_camera(0., 0., 5.);
    FragmentOwnership owner;
    resize_ownership(owner, W, H);
    TGAImage fb(W, H, TGAImage::RGB, BG);

    overlay_feature_lines(s, model, 0, {0., 0., 5.}, 3., RED, fb, owner);
    TASK_CHECK(same_color(fb.get(32, 16), RED)); // on the horizontal edge
    TASK_CHECK(same_color(fb.get(32, 17), RED)); // one pixel away, inside half width 1.5
    TASK_CHECK(same_color(fb.get(32, 18), BG));  // two pixels away, outside
    TASK_CHECK(same_color(fb.get(15, 16), RED)); // round cap around endpoint (16,16)
    TASK_CHECK(same_color(fb.get(14, 16), BG));
    TASK_CHECK(same_color(fb.get(15, 15), RED)); // sqrt(2) <= 1.5
    TASK_CHECK(same_color(fb.get(14, 15), BG));  // sqrt(5) > 1.5

    TGAImage thin(W, H, TGAImage::RGB, BG);
    overlay_feature_lines(s, model, 0, {0., 0., 5.}, 1., RED, thin, owner);
    TASK_CHECK(same_color(thin.get(32, 16), RED));
    TASK_CHECK(same_color(thin.get(32, 17), BG)); // half width 0.5
    return 0;
}

int visibility_partly_offscreen() {
    write_file(".task-build/t_off.obj",
        "v -.5 -.5 0\nv 1.5 -.5 0\nv 0 .5 0\nvt 0 0\nvt 1 0\nvt .5 1\nvn 0 0 1\n"
        "f 1/1/1 2/2/1 3/3/1\n");
    Model model(".task-build/t_off.obj");
    FeatureEdgeSet s = classify_feature_edges(model, 30.);
    setup_camera(0., 0., 5.);
    FragmentOwnership owner;
    resize_ownership(owner, W, H);
    TGAImage fb(W, H, TGAImage::RGB, BG);
    overlay_feature_lines(s, model, 0, {0., 0., 5.}, 3., RED, fb, owner);
    TASK_CHECK(same_color(fb.get(63, 16), RED)); // edge leaves the viewport, parameter stays on the segment
    TASK_CHECK(same_color(fb.get(63, 18), BG));
    TASK_CHECK(same_color(fb.get(0, 0), BG));
    return 0;
}

int visibility_behind_camera_skipped() {
    write_file(".task-build/t_behind.obj",
        "v -.5 -.5 0\nv .5 -.5 6\nv 0 .5 0\nvt 0 0\nvt 1 0\nvt .5 1\nvn 0 0 1\n"
        "f 1/1/1 2/2/1 3/3/1\n");
    Model model(".task-build/t_behind.obj");
    FeatureEdgeSet s = classify_feature_edges(model, 30.);
    setup_camera(0., 0., 5.); // vertex 2 is behind the eye: its projected w is negative
    FragmentOwnership owner;
    resize_ownership(owner, W, H);
    TGAImage fb(W, H, TGAImage::RGB, BG);
    overlay_feature_lines(s, model, 0, {0., 0., 5.}, 3., RED, fb, owner);
    TASK_CHECK(same_color(fb.get(32, 16), BG));  // the edge touching the bad vertex is skipped
    TASK_CHECK(same_color(fb.get(16, 16), RED)); // the remaining valid edge still draws
    return 0;
}

// Two triangles at different depths sharing the same screen region.
const char *kSheetObj =
    "v -.5 -.5 0\nv .5 -.5 0\nv 0 .5 0\nvt 0 0\nvt 1 0\nvt .5 1\nvn 0 0 1\n"
    "f 1/1/1 2/2/1 3/3/1\n";
const char *kOccluderObj =
    "v -.25 -.5 2\nv .25 -.5 2\nv 0 .2 2\nvt 0 0\nvt 1 0\nvt .5 1\nvn 0 0 1\n"
    "f 1/1/1 2/2/1 3/3/1\n";

void rasterize_model(Model &model, int object_id, bool discard, TGAColor color,
                     TGAImage &fb, FragmentOwnership &owner) {
    FlatShader shader(discard, color);
    for (int f = 0; f < model.nfaces(); ++f) {
        Triangle clip = { Perspective * (ModelView * model.vert(f, 0)),
                          Perspective * (ModelView * model.vert(f, 1)),
                          Perspective * (ModelView * model.vert(f, 2)) };
        rasterize(clip, shader, fb, object_id, f, &owner);
    }
}

int integration_intersecting_occluder() {
    write_file(".task-build/t_sheet.obj", kSheetObj);
    write_file(".task-build/t_occ.obj", kOccluderObj);
    Model sheet(".task-build/t_sheet.obj");
    Model occluder(".task-build/t_occ.obj");
    setup_camera(0., 0., 100.);
    FragmentOwnership owner;
    resize_ownership(owner, W, H);
    TGAImage fb(W, H, TGAImage::RGB, BG);
    rasterize_model(sheet, 0, false, BLUE, fb, owner);
    rasterize_model(occluder, 1, false, GREEN, fb, owner);

    const int center = 32 + 16 * W, side = 17 + 16 * W;
    TASK_CHECK(owner.object[center] == 1 && owner.face[center] == 0); // later object owns the center
    TASK_CHECK(owner.object[side] == 0 && owner.face[side] == 0);

    FeatureEdgeSet edges = classify_feature_edges(sheet, 30.);
    overlay_feature_lines(edges, sheet, 0, {0., 0., 100.}, 3., RED, fb, owner);
    TASK_CHECK(same_color(fb.get(32, 16), GREEN)); // occluder strictly blocks the back edge
    TASK_CHECK(same_color(fb.get(17, 16), RED));   // adjacent surface lets its own edge through
    return 0;
}

int integration_same_face_number_other_object() {
    write_file(".task-build/t_sheet2.obj", kSheetObj);
    write_file(".task-build/t_occ2.obj", kOccluderObj);
    Model sheet(".task-build/t_sheet2.obj");
    Model occluder(".task-build/t_occ2.obj"); // both models number their face 0
    setup_camera(0., 0., 100.);
    FragmentOwnership owner;
    resize_ownership(owner, W, H);
    TGAImage fb(W, H, TGAImage::RGB, BG);
    rasterize_model(sheet, 0, false, BLUE, fb, owner);
    rasterize_model(occluder, 1, false, GREEN, fb, owner);
    const int center = 32 + 16 * W;
    TASK_CHECK(owner.object[center] == 1 && owner.face[center] == 0);
    FeatureEdgeSet edges = classify_feature_edges(sheet, 30.);
    overlay_feature_lines(edges, sheet, 0, {0., 0., 100.}, 3., RED, fb, owner);
    TASK_CHECK(same_color(fb.get(32, 16), GREEN)); // equal face numbers must not cancel occlusion
    return 0;
}

int integration_discard_hole() {
    write_file(".task-build/t_sheet3.obj", kSheetObj);
    write_file(".task-build/t_occ3.obj", kOccluderObj);
    Model sheet(".task-build/t_sheet3.obj");
    Model occluder(".task-build/t_occ3.obj");
    setup_camera(0., 0., 100.);
    FragmentOwnership owner;
    resize_ownership(owner, W, H);
    TGAImage fb(W, H, TGAImage::RGB, BG);
    rasterize_model(sheet, 0, false, BLUE, fb, owner);
    const double sheet_depth = zbuffer[32 + 16 * W];
    rasterize_model(occluder, 1, true, GREEN, fb, owner); // every fragment is discarded
    TASK_CHECK(owner.object[32 + 16 * W] == 0);           // ownership of the sheet survives
    TASK_CHECK(zbuffer[32 + 16 * W] == sheet_depth);      // and so does its depth
    FeatureEdgeSet edges = classify_feature_edges(sheet, 30.);
    overlay_feature_lines(edges, sheet, 0, {0., 0., 100.}, 3., RED, fb, owner);
    TASK_CHECK(same_color(fb.get(32, 16), RED)); // the line shows through the discarded hole
    return 0;
}

int integration_overlay_preserves_depth_and_is_stable() {
    write_file(".task-build/t_sheet4.obj", kSheetObj);
    Model sheet(".task-build/t_sheet4.obj");
    setup_camera(0., 0., 100.);
    FragmentOwnership owner;
    resize_ownership(owner, W, H);
    TGAImage fb(W, H, TGAImage::RGB, BG);
    rasterize_model(sheet, 0, false, BLUE, fb, owner);
    const std::vector<double> depth_before = zbuffer;
    const std::vector<int> obj_before = owner.object;
    const std::vector<int> face_before = owner.face;

    FeatureEdgeSet edges = classify_feature_edges(sheet, 30.);
    overlay_feature_lines(edges, sheet, 0, {0., 0., 100.}, 3., BLACK, fb, owner);
    std::vector<TGAColor> once(W * H);
    for (int i = 0; i < W * H; ++i) once[i] = fb.get(i % W, i / W);
    overlay_feature_lines(edges, sheet, 0, {0., 0., 100.}, 3., BLACK, fb, owner); // same opaque color again
    for (int i = 0; i < W * H; ++i) TASK_CHECK(same_color(fb.get(i % W, i / W), once[i]));
    TASK_CHECK(zbuffer == depth_before);
    TASK_CHECK(owner.object == obj_before && owner.face == face_before);
    return 0;
}

int integration_default_rasterize_unchanged() {
    setup_camera(0., 0., 100.);
    FragmentOwnership owner;
    resize_ownership(owner, W, H);
    TGAImage fb(W, H, TGAImage::RGB, BG);
    FlatShader shader(false, GREEN);
    Triangle clip = {{-.8, -.8, 0., 1.}, {.8, -.8, 0., 1.}, {0., .8, 0., 1.}};
    rasterize(clip, shader, fb); // original three-argument call style
    TASK_CHECK(zbuffer[32 + 32 * W] == 0.);
    TASK_CHECK(same_color(fb.get(32, 32), GREEN));
    for (int tag : owner.object) TASK_CHECK(tag == -1); // ownership is opt-in only
    Triangle nearer = {{-.8, -.8, .5, 1.}, {.8, -.8, .5, 1.}, {0., .8, .5, 1.}};
    rasterize(nearer, shader, fb, 7, 9, &owner); // strictly greater depth wins and records ownership
    TASK_CHECK(owner.object[32 + 32 * W] == 7 && owner.face[32 + 32 * W] == 9);
    return 0;
}

int integration_generated_obj_tga_output() {
    const std::string obj =
        "v -.5 -.5 0\nv .5 -.5 0\nv 0 .5 0\n"
        "vt 0 0\nvt 1 0\nvt .5 1\nvn 0 0 1\nf 1/1/1 2/2/1 3/3/1\n";
    write_file(".task-build/t_material.obj", obj);
    TGAImage material(8, 8, TGAImage::RGB, BLUE); // color asset generated in C++, no download
    TASK_CHECK(material.write_tga_file(".task-build/t_material_diffuse.tga", false, false));

    Model model(".task-build/t_material.obj");
    setup_camera(0., 0., 5.);
    FragmentOwnership owner;
    resize_ownership(owner, W, H);
    TGAImage fb(W, H, TGAImage::RGB, BG);
    TexturedShader shader(model, false);
    for (int f = 0; f < model.nfaces(); ++f) {
        Triangle clip = { Perspective * (ModelView * model.vert(f, 0)),
                          Perspective * (ModelView * model.vert(f, 1)),
                          Perspective * (ModelView * model.vert(f, 2)) };
        rasterize(clip, shader, fb, 0, f, &owner);
    }
    FeatureEdgeSet edges = classify_feature_edges(model, 30.);
    overlay_feature_lines(edges, model, 0, {0., 0., 5.}, 3., BLACK, fb, owner);
    TASK_CHECK(fb.write_tga_file(".task-build/feature_lines.tga", false, false));

    TGAImage loaded;
    TASK_CHECK(loaded.read_tga_file(".task-build/feature_lines.tga"));
    TASK_CHECK(loaded.width() == W && loaded.height() == H);
    TASK_CHECK(same_color(loaded.get(32, 16), BLACK)); // occluded line-art TGA, line on its own surface
    TASK_CHECK(same_color(loaded.get(1, 1), BG));
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"topology", "plane_split_uv_no_diagonal",      topology_plane_split_uv},
    {"topology", "duplicate_positions_not_welded",  topology_duplicate_positions_not_welded},
    {"topology", "folded_mesh_crease_threshold",    topology_folded_crease},
    {"topology", "zero_area_face_excluded",         topology_zero_area_excluded},
    {"topology", "three_neighbors_handled_crease",  topology_three_neighbors_is_crease},
    {"visibility", "silhouette_changes_with_eye",   visibility_silhouette_moves_with_eye},
    {"visibility", "line_width_and_round_caps",     visibility_width_and_round_caps},
    {"visibility", "edge_partly_outside_viewport",  visibility_partly_offscreen},
    {"visibility", "non_positive_w_vertex_skipped", visibility_behind_camera_skipped},
    {"integration", "intersecting_occluder",        integration_intersecting_occluder},
    {"integration", "same_face_number_other_object",integration_same_face_number_other_object},
    {"integration", "discarded_fragment_hole",      integration_discard_hole},
    {"integration", "overlay_stable_and_preserves", integration_overlay_preserves_depth_and_is_stable},
    {"integration", "default_rasterize_compatible", integration_default_rasterize_unchanged},
    {"integration", "generated_obj_writes_tga",     integration_generated_obj_tga_output},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
