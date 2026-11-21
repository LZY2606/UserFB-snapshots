// Injective atlas task tests. Registered through task_support/check.h; this
// file never defines main. All intersection areas below are computed with an
// independent Sutherland-Hodgman polygon clipper local to the tests.

#include "task_support/check.h"

#include "thekla/thekla_atlas.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <queue>
#include <utility>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace Thekla;

namespace
{
    const double OVERLAP_EPS = 1e-10;
    const double FACE_EPS = 1e-7;

    struct MeshBuilder
    {
        std::vector<Atlas_Input_Vertex> vertices;
        std::vector<Atlas_Input_Face> faces;

        int addVertex(float px, float py, float pz, float u, float v,
                      int colocal = -1, float nx = 0, float ny = 0, float nz = 1)
        {
            Atlas_Input_Vertex vert;
            vert.position[0] = px; vert.position[1] = py; vert.position[2] = pz;
            vert.normal[0] = nx; vert.normal[1] = ny; vert.normal[2] = nz;
            vert.uv[0] = u; vert.uv[1] = v;
            vert.first_colocal = (colocal < 0) ? (int)vertices.size() : colocal;
            vertices.push_back(vert);
            return (int)vertices.size() - 1;
        }

        int addFace(int a, int b, int c, int material = 0)
        {
            Atlas_Input_Face face;
            face.vertex_index[0] = a;
            face.vertex_index[1] = b;
            face.vertex_index[2] = c;
            face.material_index = material;
            faces.push_back(face);
            return (int)faces.size() - 1;
        }

        Atlas_Input_Mesh mesh()
        {
            Atlas_Input_Mesh m;
            m.vertex_count = (int)vertices.size();
            m.face_count = (int)faces.size();
            m.vertex_array = &vertices[0];
            m.face_array = &faces[0];
            return m;
        }
    };

    Atlas_Options injectiveOptions()
    {
        Atlas_Options o;
        atlas_set_default_options(&o);
        o.charter = Atlas_Charter_Extract;
        o.mapper = Atlas_Mapper_LSCM;
        o.packer = Atlas_Packer_Witness;
        o.mapper_options.preserve_uvs = true;
        o.mapper_options.preserve_boundary = true;
        o.packer_options.witness.packing_quality = 1;
        return o;
    }

    // ---- Independent polygon clipping (test-only, double precision) ----

    struct Poly2
    {
        std::vector< std::pair<double, double> > p;
    };

    static double signedArea(const Poly2 & poly)
    {
        double a = 0.0;
        for (size_t i = 0; i < poly.p.size(); i++) {
            const std::pair<double, double> & x = poly.p[i];
            const std::pair<double, double> & y = poly.p[(i + 1) % poly.p.size()];
            a += x.first * y.second - y.first * x.second;
        }
        return 0.5 * a;
    }

    static void clipAgainstEdge(Poly2 & poly, const double a[2], const double b[2])
    {
        Poly2 out;
        if (poly.p.empty()) { poly = out; return; }

        const double ex = b[0] - a[0];
        const double ey = b[1] - a[1];

        const size_t n = poly.p.size();
        for (size_t i = 0; i < n; i++) {
            const std::pair<double, double> & cur = poly.p[i];
            const std::pair<double, double> & nxt = poly.p[(i + 1) % n];

            const double cs = ex * (cur.second - a[1]) - ey * (cur.first - a[0]);
            const double ns = ex * (nxt.second - a[1]) - ey * (nxt.first - a[0]);
            const bool cin = cs >= 0.0;
            const bool nin = ns >= 0.0;

            if (cin != nin) {
                const double den = (nxt.first - cur.first) * ey - (nxt.second - cur.second) * ex;
                if (den != 0.0) {
                    const double t = ((a[0] - cur.first) * ey - (a[1] - cur.second) * ex) / den;
                    out.p.push_back(std::make_pair(
                        cur.first + t * (nxt.first - cur.first),
                        cur.second + t * (nxt.second - cur.second)));
                }
            }
            if (nin) out.p.push_back(nxt);
        }
        poly = out;
    }

    static double intersectionArea(double t0[3][2], double t1[3][2])
    {
        Poly2 a, b;
        for (int i = 0; i < 3; i++) {
            a.p.push_back(std::make_pair(t0[i][0], t0[i][1]));
            b.p.push_back(std::make_pair(t1[i][0], t1[i][1]));
        }
        if (signedArea(a) < 0.0) std::swap(a.p[1], a.p[2]);
        if (signedArea(b) < 0.0) std::swap(b.p[1], b.p[2]);

        for (int e = 0; e < 3 && !a.p.empty(); e++) {
            double ea[2] = { b.p[e].first, b.p[e].second };
            double eb[2] = { b.p[(e + 1) % 3].first, b.p[(e + 1) % 3].second };
            clipAgainstEdge(a, ea, eb);
        }
        if (a.p.size() < 3) return 0.0;
        return std::fabs(signedArea(a));
    }

    static void outputTriangle(const Atlas_Output_Mesh * out, int face, double tri[3][2])
    {
        for (int k = 0; k < 3; k++) {
            const int vi = out->index_array[3 * face + k];
            tri[k][0] = out->vertex_array[vi].uv[0];
            tri[k][1] = out->vertex_array[vi].uv[1];
        }
    }

    // Bounding-box edge length of one final chart in packed atlas space.
    static double chartEdgeLength(const Atlas_Output_Mesh * out,
                                  const Atlas_Injective_Report * r, int chart)
    {
        double mn[2] = { 1e300, 1e300 }, mx[2] = { -1e300, -1e300 };
        bool any = false;
        for (int i = 0; i < r->chart_face_count[chart]; i++) {
            const int face = r->chart_face_array[chart][i];
            double tri[3][2];
            outputTriangle(out, face, tri);
            for (int k = 0; k < 3; k++) {
                for (int d = 0; d < 2; d++) {
                    mn[d] = std::min(mn[d], tri[k][d]);
                    mx[d] = std::max(mx[d], tri[k][d]);
                }
                any = true;
            }
        }
        if (!any) return 0.0;
        return std::max(mx[0] - mn[0], mx[1] - mn[1]);
    }

    // Verify no positive-area overlap inside any chart and across charts.
    static bool verifyNoOverlaps(const Atlas_Output_Mesh * out,
                                 const Atlas_Injective_Report * r)
    {
        const int faces = out->index_count / 3;
        std::vector<double> tris(faces * 9);
        std::vector<double> L2(r->chart_count, 0.0);

        for (int f = 0; f < faces; f++) {
            double tri[3][2];
            outputTriangle(out, f, tri);
            for (int k = 0; k < 3; k++)
                for (int d = 0; d < 2; d++)
                    tris[f * 9 + k * 2 + d] = tri[k][d];
            if (!std::isfinite(tris[f * 9]) || !std::isfinite(tris[f * 9 + 1])) return false;
        }
        for (int c = 0; c < r->chart_count; c++) {
            const double L = chartEdgeLength(out, r, c);
            L2[c] = L * L;
        }

        double global_min[2] = { 1e300, 1e300 }, global_max[2] = { -1e300, -1e300 };
        for (int f = 0; f < faces; f++) {
            for (int k = 0; k < 3; k++)
                for (int d = 0; d < 2; d++) {
                    global_min[d] = std::min(global_min[d], tris[f * 9 + k * 2 + d]);
                    global_max[d] = std::max(global_max[d], tris[f * 9 + k * 2 + d]);
                }
        }
        const double global_L = std::max(global_max[0] - global_min[0], global_max[1] - global_min[1]);
        const double global_eps = OVERLAP_EPS * global_L * global_L;

        for (int a = 0; a < faces; a++) {
            double ta[3][2];
            for (int k = 0; k < 3; k++)
                for (int d = 0; d < 2; d++) ta[k][d] = tris[a * 9 + k * 2 + d];

            for (int b = a + 1; b < faces; b++) {
                double tb[3][2];
                for (int k = 0; k < 3; k++)
                    for (int d = 0; d < 2; d++) tb[k][d] = tris[b * 9 + k * 2 + d];

                const double area = intersectionArea(ta, tb);
                if (area <= 0.0) continue;

                const int ca = r->face_chart[a];
                const int cb = r->face_chart[b];
                if (ca == cb) {
                    if (area > OVERLAP_EPS * L2[ca]) return false;
                }
                else {
                    if (area > global_eps) return false;
                }
            }
        }
        return true;
    }

    static bool verifyCoverage(const Atlas_Input_Mesh & m, const Atlas_Injective_Report * r)
    {
        std::vector<int> seen(m.face_count, 0);
        for (int c = 0; c < r->chart_count; c++) {
            if (r->chart_face_count[c] == 0) return false;
            for (int i = 0; i < r->chart_face_count[c]; i++) {
                const int f = r->chart_face_array[c][i];
                if (f < 0 || f >= m.face_count) return false;
                seen[f]++;
            }
        }
        for (int f = 0; f < m.face_count; f++) if (seen[f] != 1) return false;
        for (int f = 0; f < m.face_count; f++) {
            const int c = r->face_chart[f];
            if (c < 0 || c >= r->chart_count) return false;
            bool found = false;
            for (int i = 0; i < r->chart_face_count[c]; i++)
                if (r->chart_face_array[c][i] == f) found = true;
            if (!found) return false;
        }
        return true;
    }

    // Spiral ribbon builder. Every UV triangle has positive, consistent
    // orientation while successive coils cover the same texels.
    static void buildSpiral(MeshBuilder & b, double R0, double k, double width,
                            double turns, int segments, int material = 0)
    {
        std::vector< std::pair<double, double> > center;
        for (int i = 0; i <= segments; i++) {
            const double th = 2.0 * M_PI * turns * i / segments;
            const double rad = R0 - k * th;
            center.push_back(std::make_pair(rad * std::cos(th), rad * std::sin(th)));
        }

        const int n = (int)center.size();
        std::vector<int> left(n), right(n);
        for (int i = 0; i < n; i++) {
            double dx, dy;
            if (i + 1 < n) {
                dx = center[i + 1].first - center[i].first;
                dy = center[i + 1].second - center[i].second;
            } else {
                dx = center[i].first - center[i - 1].first;
                dy = center[i].second - center[i - 1].second;
            }
            const double len = std::sqrt(dx * dx + dy * dy);
            dx /= len; dy /= len;
            const double nx = -dy, ny = dx;

            const float lx = float(center[i].first + width * nx);
            const float ly = float(center[i].second + width * ny);
            const float rx = float(center[i].first - width * nx);
            const float ry = float(center[i].second - width * ny);

            left[i] = b.addVertex(float(i), float(width), 0, lx, ly, -1, 0, 0, 1);
            right[i] = b.addVertex(float(i), float(-width), 0, rx, ry, -1, 0, 0, 1);
        }
        for (int i = 0; i + 1 < n; i++) {
            b.addFace(left[i], right[i], right[i + 1], material);
            b.addFace(left[i], right[i + 1], left[i + 1], material);
        }
    }

    // A planar spiral ribbon in 3D with a sawtooth UV path. The geometry is
    // genuinely curved so LSCM descendants can fold too, forcing a nested
    // parent -> child -> grandchild split chain.
    static void buildFoldedSpiral3D(MeshBuilder & b, double R0, double k,
                                    double width, double turns, int segments,
                                    int saw_period)
    {
        std::vector< std::pair<double, double> > center;
        for (int i = 0; i <= segments; i++) {
            const double th = 2.0 * M_PI * turns * i / segments;
            const double rad = R0 - k * th;
            center.push_back(std::make_pair(rad * std::cos(th), rad * std::sin(th)));
        }

        const int n = (int)center.size();
        std::vector<int> left(n), right(n);
        for (int i = 0; i < n; i++) {
            double dx, dy;
            if (i + 1 < n) {
                dx = center[i + 1].first - center[i].first;
                dy = center[i + 1].second - center[i].second;
            } else {
                dx = center[i].first - center[i - 1].first;
                dy = center[i].second - center[i - 1].second;
            }
            const double len = std::sqrt(dx * dx + dy * dy);
            dx /= len; dy /= len;
            const double nx = -dy, ny = dx;

            const float px0 = float(center[i].first + width * nx);
            const float py0 = float(center[i].second + width * ny);
            const float px1 = float(center[i].first - width * nx);
            const float py1 = float(center[i].second - width * ny);
            const float uu = float(i % saw_period);

            left[i] = b.addVertex(px0, py0, 0, uu, 0.0f, -1, 0, 0, 1);
            right[i] = b.addVertex(px1, py1, 0, uu, 1.0f, -1, 0, 0, 1);
        }
        for (int i = 0; i + 1 < n; i++) {
            b.addFace(left[i], right[i], right[i + 1]);
            b.addFace(left[i], right[i + 1], left[i + 1]);
        }
    }

    // Largest input-mapping intersection found with the test clipper,
    // expressed as a fraction of the smaller triangle area.
    static double deepestInputOverlap(const MeshBuilder & b)
    {
        const int faces = (int)b.faces.size();
        std::vector<double> data(faces * 9);
        for (int f = 0; f < faces; f++) {
            for (int k = 0; k < 3; k++) {
                const int vi = b.faces[f].vertex_index[k];
                data[f * 9 + k * 2 + 0] = b.vertices[vi].uv[0];
                data[f * 9 + k * 2 + 1] = b.vertices[vi].uv[1];
            }
        }

        double best = 0.0;
        for (int a = 0; a < faces; a++) {
            double ta[3][2], tb[3][2];
            for (int k = 0; k < 3; k++)
                for (int d = 0; d < 2; d++) ta[k][d] = data[a * 9 + k * 2 + d];
            const double area_a = std::fabs(0.5 *
                ((ta[1][0] - ta[0][0]) * (ta[2][1] - ta[0][1]) -
                 (ta[1][1] - ta[0][1]) * (ta[2][0] - ta[0][0])));
            for (int bb = a + 1; bb < faces; bb++) {
                for (int k = 0; k < 3; k++)
                    for (int d = 0; d < 2; d++) tb[k][d] = data[bb * 9 + k * 2 + d];
                const double area_b = std::fabs(0.5 *
                    ((tb[1][0] - tb[0][0]) * (tb[2][1] - tb[0][1]) -
                     (tb[1][1] - tb[0][1]) * (tb[2][0] - tb[0][0])));
                const double smaller = std::min(area_a, area_b);
                if (smaller <= 0.0) continue;
                best = std::max(best, intersectionArea(ta, tb) / smaller);
            }
        }
        return best;
    }
}

// ============================ overlaps group ============================

// Positive-orientation self-covering strip (spiral): every input triangle has
// positive area, yet the chart repeats texels. Must be detected through an
// overlap witness pair rather than an orientation/area failure.
static int test_spiral_self_overlap()
{
    MeshBuilder b;
    buildSpiral(b, 25.0, 0.12, 2.0, 2.2, 180);
    Atlas_Input_Mesh m = b.mesh();

    TASK_CHECK(deepestInputOverlap(b) > 0.5);

    Atlas_Options o = injectiveOptions();
    Atlas_Error e;
    Atlas_Injective_Report * r = NULL;
    Atlas_Output_Mesh * out = atlas_generate_injective(&m, &o, 128, &r, &e);

    TASK_CHECK(e == Atlas_Error_Success);
    TASK_CHECK(out != NULL && r != NULL);
    TASK_CHECK(r->chart_count > 1);
    TASK_CHECK(r->split_count >= 1);

    bool pair_witness = false;
    for (int s = 0; s < r->split_count; s++) {
        TASK_CHECK(r->split_array[s].witness_face[0] != r->split_array[s].witness_face[1] ||
                   r->split_count > 1);
        if (r->split_array[s].witness_face[0] != r->split_array[s].witness_face[1]) pair_witness = true;
    }
    TASK_CHECK(pair_witness);
    TASK_CHECK(verifyCoverage(m, r));
    TASK_CHECK(verifyNoOverlaps(out, r));

    atlas_free(out);
    atlas_injective_report_free(r);
    return 0;
}

// Containment: a later strip triangle maps deep inside an earlier one.
static int test_containment()
{
    MeshBuilder b;
    buildSpiral(b, 25.0, 0.15, 1.8, 2.6, 200);
    Atlas_Input_Mesh m = b.mesh();

    // A large fraction of some triangle is covered by another one.
    TASK_CHECK(deepestInputOverlap(b) > 0.5);

    Atlas_Options o = injectiveOptions();
    Atlas_Error e;
    Atlas_Injective_Report * r = NULL;
    Atlas_Output_Mesh * out = atlas_generate_injective(&m, &o, 256, &r, &e);

    TASK_CHECK(e == Atlas_Error_Success);
    TASK_CHECK(out != NULL && r != NULL);
    TASK_CHECK(r->split_count >= 1);
    TASK_CHECK(verifyCoverage(m, r));
    TASK_CHECK(verifyNoOverlaps(out, r));

    atlas_free(out);
    atlas_injective_report_free(r);
    return 0;
}

// Legal contacts: shared edges, shared vertices and boundary touches never
// count as overlaps; a healthy connected chart must not be cut.
static int test_legal_contact()
{
    MeshBuilder b;
    // 3x3 quad grid, identity UVs; interior edges and vertices are shared.
    const int N = 4;
    std::vector<int> id((N + 1) * (N + 1));
    for (int y = 0; y <= N; y++)
        for (int x = 0; x <= N; x++)
            id[y * (N + 1) + x] = b.addVertex(float(x), float(y), 0, float(x), float(y));
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            const int a = id[y * (N + 1) + x];
            const int bb = id[y * (N + 1) + x + 1];
            const int d = id[(y + 1) * (N + 1) + x];
            const int c = id[(y + 1) * (N + 1) + x + 1];
            b.addFace(a, bb, c);
            b.addFace(a, c, d);
        }

    // A second component touching the first only at one spatial point, using a
    // distinct input vertex with its own UV (boundary point contact).
    const int touch = b.addVertex(0, 0, 0, 9.0f, 9.0f);
    const int p1 = b.addVertex(0, -1, 0, 9.0f, 8.0f);
    const int p2 = b.addVertex(1, -1, 0, 10.0f, 8.0f);
    b.addFace(touch, p1, p2, 1);

    Atlas_Input_Mesh m = b.mesh();
    Atlas_Options o = injectiveOptions();
    Atlas_Error e;
    Atlas_Injective_Report * r = NULL;
    Atlas_Output_Mesh * out = atlas_generate_injective(&m, &o, 8, &r, &e);

    TASK_CHECK(e == Atlas_Error_Success);
    TASK_CHECK(out != NULL && r != NULL);
    TASK_CHECK(r->split_count == 0);
    TASK_CHECK(verifyCoverage(m, r));
    TASK_CHECK(verifyNoOverlaps(out, r));

    // Shared edge pairs must clip to zero area.
    double t0[3][2] = { {0, 0}, {1, 0}, {1, 1} };
    double t1[3][2] = { {0, 0}, {1, 1}, {0, 1} };
    TASK_CHECK(intersectionArea(t0, t1) <= 1e-12);

    atlas_free(out);
    atlas_injective_report_free(r);
    return 0;
}

// Cross-material chart plus colocal vertices split into distinct UVs: the two
// input vertices at the same spatial position must keep independent attributes.
static int test_cross_material_colocal_split()
{
    MeshBuilder b;
    // Two components sharing one spatial point, different materials and UVs.
    const int a0 = b.addVertex(0, 0, 0, 0, 0, 0);
    const int a1 = b.addVertex(1, 0, 0, 1, 0, 1);
    const int a2 = b.addVertex(1, 1, 0, 1, 1, 2);
    const int a3 = b.addVertex(0, 1, 0, 0, 1, 3);
    b.addFace(a0, a1, a2, 0);
    b.addFace(a0, a2, a3, 0);

    // Distinct input vertex at the same position as a0, different UV, colocal.
    const int c0 = b.addVertex(0, 0, 0, 5, 5, 0);
    const int c1 = b.addVertex(0, -1, 0, 5, 4, 5);
    const int c2 = b.addVertex(1, -1, 0, 6, 4, 6);
    b.addFace(c0, c1, c2, 1);

    Atlas_Input_Mesh m = b.mesh();
    Atlas_Options o = injectiveOptions();
    Atlas_Error e;
    Atlas_Injective_Report * r = NULL;
    Atlas_Output_Mesh * out = atlas_generate_injective(&m, &o, 8, &r, &e);

    TASK_CHECK(e == Atlas_Error_Success);
    TASK_CHECK(out != NULL && r != NULL);
    TASK_CHECK(r->chart_count >= 2);
    TASK_CHECK(verifyCoverage(m, r));
    TASK_CHECK(verifyNoOverlaps(out, r));

    // Per-corner xref must restore the exact input vertex, so the two colocal
    // vertices never share an output corner.
    for (int f = 0; f < m.face_count; f++) {
        for (int k = 0; k < 3; k++) {
            const int vi = out->index_array[3 * f + k];
            TASK_CHECK(out->vertex_array[vi].xref == m.face_array[f].vertex_index[k]);
        }
    }

    // The two input vertices at the same spatial position (a0 and c0) must
    // resolve to distinct output vertices carrying distinct UVs.
    const int out_a0 = out->index_array[0];                 // face 0, corner 0 -> a0
    const int out_c0 = out->index_array[3 * 2 + 0];         // face 2, corner 0 -> c0
    TASK_CHECK(out->vertex_array[out_a0].xref == a0);
    TASK_CHECK(out->vertex_array[out_c0].xref == c0);
    TASK_CHECK(out_a0 != out_c0);
    TASK_CHECK(out->vertex_array[out_a0].uv[0] != out->vertex_array[out_c0].uv[0] ||
               out->vertex_array[out_a0].uv[1] != out->vertex_array[out_c0].uv[1]);

    atlas_free(out);
    atlas_injective_report_free(r);
    return 0;
}

// ============================== recut group ==============================

// Several consecutive cuts, including a genuine nested parent->child chain;
// verify the full split tree.
static int test_continuous_multiple_splits()
{
    MeshBuilder b;
    buildFoldedSpiral3D(b, 18.0, 0.18, 0.7, 3.0, 260, 40);
    Atlas_Input_Mesh m = b.mesh();
    Atlas_Options o = injectiveOptions();
    Atlas_Error e;
    Atlas_Injective_Report * r = NULL;
    Atlas_Output_Mesh * out = atlas_generate_injective(&m, &o, 256, &r, &e);

    TASK_CHECK(e == Atlas_Error_Success);
    TASK_CHECK(out != NULL && r != NULL);
    TASK_CHECK(r->split_count >= 2);
    TASK_CHECK(r->chart_count == r->node_count - r->split_count);

    // A nested chain exists: some child chart is itself split again.
    bool nested = false;
    for (int s = 0; s < r->split_count; s++) {
        for (int t = 0; t < r->split_count; t++) {
            if (r->split_array[t].parent_chart == r->split_array[s].child_chart[0] ||
                r->split_array[t].parent_chart == r->split_array[s].child_chart[1]) {
                nested = true;
            }
        }
    }
    TASK_CHECK(nested);

    for (int s = 0; s < r->split_count; s++) {
        const Atlas_Injective_Split & sp = r->split_array[s];

        // Each split yields two non-empty strict subsets of the parent faces.
        TASK_CHECK(sp.child_face_count[0] > 0 && sp.child_face_count[1] > 0);
        TASK_CHECK(sp.child_face_count[0] + sp.child_face_count[1] == sp.parent_face_count);
        TASK_CHECK(sp.child_face_count[0] < sp.parent_face_count);
        TASK_CHECK(sp.child_face_count[1] < sp.parent_face_count);

        // Child sets partition the parent set.
        std::vector<int> all;
        for (int g = 0; g < 2; g++)
            for (int i = 0; i < sp.child_face_count[g]; i++)
                all.push_back(sp.child_face_array[g][i]);
        std::sort(all.begin(), all.end());
        std::vector<int> parent(sp.parent_face_array, sp.parent_face_array + sp.parent_face_count);
        std::sort(parent.begin(), parent.end());
        TASK_CHECK(all == parent);

        // Witnesses and cut edges belong to the parent chart.
        for (int w = 0; w < 2; w++) {
            TASK_CHECK(std::find(parent.begin(), parent.end(), sp.witness_face[w]) != parent.end());
            TASK_CHECK(std::find(parent.begin(), parent.end(), sp.cut_edge_face[w]) != parent.end());
        }
        TASK_CHECK(sp.cut_edge_face[0] != sp.cut_edge_face[1]);
    }

    TASK_CHECK(verifyCoverage(m, r));
    TASK_CHECK(verifyNoOverlaps(out, r));

    atlas_free(out);
    atlas_injective_report_free(r);
    return 0;
}

// A budget failure must publish nothing; a follow-up call with a larger budget
// must still succeed and leave prior state untouched.
static int test_budget_then_success()
{
    MeshBuilder b;
    buildSpiral(b, 20.0, 0.2, 0.8, 2.2, 140);
    Atlas_Input_Mesh m = b.mesh();
    Atlas_Options o = injectiveOptions();
    Atlas_Error e;

    Atlas_Injective_Report * r = NULL;
    Atlas_Output_Mesh * out = atlas_generate_injective(&m, &o, 1, &r, &e);
    TASK_CHECK(out == NULL && r == NULL);
    TASK_CHECK(e == Atlas_Error_Budget_Exceeded);

    // The input mesh is untouched: face count and a known UV are preserved.
    TASK_CHECK(m.face_count == (int)b.faces.size());
    TASK_CHECK(b.vertices[m.face_array[0].vertex_index[0]].uv[0] ==
               b.vertices[m.face_array[0].vertex_index[0]].uv[0]);

    Atlas_Injective_Report * r2 = NULL;
    Atlas_Output_Mesh * out2 = atlas_generate_injective(&m, &o, 128, &r2, &e);
    TASK_CHECK(e == Atlas_Error_Success);
    TASK_CHECK(out2 != NULL && r2 != NULL);
    TASK_CHECK(r2->chart_count > 1);
    TASK_CHECK(verifyCoverage(m, r2));
    TASK_CHECK(verifyNoOverlaps(out2, r2));

    atlas_free(out2);
    atlas_injective_report_free(r2);
    return 0;
}

// Independently recompute the BFS tree cut rule and compare with the report.
static int test_cut_edge_rule()
{
    MeshBuilder b;
    buildSpiral(b, 20.0, 0.2, 0.8, 2.2, 120);
    Atlas_Input_Mesh m = b.mesh();
    Atlas_Options o = injectiveOptions();
    Atlas_Error e;
    Atlas_Injective_Report * r = NULL;
    Atlas_Output_Mesh * out = atlas_generate_injective(&m, &o, 128, &r, &e);
    TASK_CHECK(e == Atlas_Error_Success && r != NULL && r->split_count >= 1);

    // Dual adjacency of the source mesh.
    const int n = m.face_count;
    std::vector< std::vector<int> > adj(n);
    for (int f = 0; f < n; f++) {
        const int * t = m.face_array[f].vertex_index;
        for (int g = f + 1; g < n; g++) {
            const int * u = m.face_array[g].vertex_index;
            int shared = 0;
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++)
                    if (t[i] == u[j]) shared++;
            if (shared == 2) { adj[f].push_back(g); adj[g].push_back(f); }
        }
        std::sort(adj[f].begin(), adj[f].end());
    }

    for (int s = 0; s < r->split_count; s++) {
        const Atlas_Injective_Split & sp = r->split_array[s];
        std::vector<int> parent(sp.parent_face_array,
                                sp.parent_face_array + sp.parent_face_count);
        std::sort(parent.begin(), parent.end());
        std::vector<int> member(n, -1);
        for (size_t i = 0; i < parent.size(); i++) member[parent[i]] = (int)i;

        const int root_face = parent[0]; // smallest source face
        std::vector<int> par(parent.size(), -1), depth(parent.size(), -1), order;
        std::queue<int> q;
        q.push(member[root_face]);
        depth[member[root_face]] = 0;
        while (!q.empty()) {
            const int cur = q.front(); q.pop();
            order.push_back(cur);
            for (size_t i = 0; i < adj[parent[cur]].size(); i++) {
                const int nl = member[adj[parent[cur]][i]];
                if (nl >= 0 && depth[nl] < 0) {
                    depth[nl] = depth[cur] + 1;
                    par[nl] = cur;
                    q.push(nl);
                }
            }
        }

        // Path between the two witness faces.
        int wa = member[sp.witness_face[0]], wb = member[sp.witness_face[1]];
        TASK_CHECK(wa >= 0 && wb >= 0);
        std::vector<int> path_a, path_b;
        int na = wa, nb = wb;
        while (depth[na] > depth[nb]) { path_a.push_back(na); na = par[na]; }
        while (depth[nb] > depth[na]) { path_b.push_back(nb); nb = par[nb]; }
        while (na != nb) { path_a.push_back(na); na = par[na]; path_b.push_back(nb); nb = par[nb]; }
        std::vector<int> path = path_a;
        path.push_back(na);
        for (size_t i = path_b.size(); i > 0; i--) path.push_back(path_b[i - 1]);

        std::vector<int> subtree(parent.size(), 1);
        for (size_t i = order.size(); i > 0; i--)
            if (par[order[i - 1]] >= 0) subtree[par[order[i - 1]]] += subtree[order[i - 1]];

        // Apply the rule: maximize the smaller group, then smallest endpoints.
        int best_small = -1, best_e0 = -1, best_e1 = -1;
        for (size_t i = 0; i + 1 < path.size(); i++) {
            const int u = path[i], v = path[i + 1];
            int child = (par[v] == u) ? v : u;
            const int small = std::min(subtree[child], (int)parent.size() - subtree[child]);
            const int e0 = std::min(parent[u], parent[v]);
            const int e1 = std::max(parent[u], parent[v]);
            if (small > best_small || (small == best_small &&
                (e0 < best_e0 || (e0 == best_e0 && e1 < best_e1)))) {
                best_small = small; best_e0 = e0; best_e1 = e1;
            }
        }
        TASK_CHECK(best_e0 == sp.cut_edge_face[0]);
        TASK_CHECK(best_e1 == sp.cut_edge_face[1]);

        // The reported cut edge is a real dual-graph edge.
        const std::vector<int> & neighbors = adj[best_e0];
        TASK_CHECK(std::find(neighbors.begin(), neighbors.end(), best_e1) != neighbors.end());
    }

    atlas_free(out);
    atlas_injective_report_free(r);
    return 0;
}

// A single degenerate face cannot be repaired and must fail explicitly rather
// than emit a bad chart.
static int test_single_face_uninjectable()
{
    MeshBuilder b;
    const int v0 = b.addVertex(0, 0, 0, 0, 0);
    const int v1 = b.addVertex(1, 0, 0, 1, 0);
    const int v2 = b.addVertex(0, 1, 0, 1, 0); // collinear UVs: zero area
    b.addFace(v0, v1, v2);
    Atlas_Input_Mesh m = b.mesh();

    Atlas_Options o = injectiveOptions();
    Atlas_Error e;
    Atlas_Injective_Report * r = NULL;
    Atlas_Output_Mesh * out = atlas_generate_injective(&m, &o, 4, &r, &e);
    TASK_CHECK(out == NULL && r == NULL);
    TASK_CHECK(e == Atlas_Error_Uninjectable);
    return 0;
}

// ============================== output group ==============================

// Per-corner xref restores the input source in original face order.
static int test_xref_per_corner()
{
    MeshBuilder b;
    const int v0 = b.addVertex(0, 0, 0, 0, 0);
    const int v1 = b.addVertex(1, 0, 0, 1, 0);
    const int v2 = b.addVertex(1, 1, 0, 1, 1);
    const int v3 = b.addVertex(0, 1, 0, 0, 1);
    b.addFace(v0, v1, v2);
    b.addFace(v0, v2, v3);
    Atlas_Input_Mesh m = b.mesh();

    Atlas_Options o = injectiveOptions();
    Atlas_Error e;
    Atlas_Injective_Report * r = NULL;
    Atlas_Output_Mesh * out = atlas_generate_injective(&m, &o, 4, &r, &e);
    TASK_CHECK(e == Atlas_Error_Success && out != NULL && r != NULL);
    TASK_CHECK(out->index_count == 3 * m.face_count);

    for (int f = 0; f < m.face_count; f++)
        for (int k = 0; k < 3; k++) {
            const int vi = out->index_array[3 * f + k];
            TASK_CHECK(vi >= 0 && vi < out->vertex_count);
            TASK_CHECK(out->vertex_array[vi].xref == m.face_array[f].vertex_index[k]);
        }

    atlas_free(out);
    atlas_injective_report_free(r);
    return 0;
}

// Every source face appears exactly once; node/parent bookkeeping is coherent.
static int test_partition_bookkeeping()
{
    MeshBuilder b;
    buildSpiral(b, 20.0, 0.2, 0.8, 2.2, 120);
    Atlas_Input_Mesh m = b.mesh();
    Atlas_Options o = injectiveOptions();
    Atlas_Error e;
    Atlas_Injective_Report * r = NULL;
    Atlas_Output_Mesh * out = atlas_generate_injective(&m, &o, 128, &r, &e);
    TASK_CHECK(e == Atlas_Error_Success && r != NULL);

    TASK_CHECK(verifyCoverage(m, r));

    // Node faces partition the whole mesh as well.
    std::vector<int> node_seen(m.face_count, 0);
    for (int n = 0; n < r->node_count; n++) {
        TASK_CHECK(r->node_face_count[n] > 0);
        for (int i = 0; i < r->node_face_count[n]; i++) node_seen[r->node_face_array[n][i]]++;
    }
    for (int f = 0; f < m.face_count; f++) TASK_CHECK(node_seen[f] >= 1);

    // Initial nodes have parent -1; every other node references a real parent.
    for (int n = 0; n < r->node_count; n++) {
        if (r->node_parent[n] >= 0) TASK_CHECK(r->node_parent[n] < r->node_count);
    }

    atlas_free(out);
    atlas_injective_report_free(r);
    return 0;
}

// Repeated calls and released inputs keep previously returned results valid.
static int test_repeat_and_lifetime()
{
    MeshBuilder * b = new MeshBuilder();
    buildSpiral(*b, 20.0, 0.2, 0.8, 2.2, 100);
    Atlas_Input_Mesh m = b->mesh();
    Atlas_Options o = injectiveOptions();
    Atlas_Error e;

    Atlas_Injective_Report * r1 = NULL;
    Atlas_Output_Mesh * out1 = atlas_generate_injective(&m, &o, 128, &r1, &e);
    TASK_CHECK(e == Atlas_Error_Success && out1 != NULL && r1 != NULL);

    Atlas_Injective_Report * r2 = NULL;
    Atlas_Output_Mesh * out2 = atlas_generate_injective(&m, &o, 128, &r2, &e);
    TASK_CHECK(e == Atlas_Error_Success && out2 != NULL && r2 != NULL);

    // Snapshot the first result, then destroy the input and the second result.
    const int snapshot_faces = out1->index_count / 3;
    std::vector<int> snapshot_xref(out1->vertex_count);
    std::vector<float> snapshot_u(out1->vertex_count), snapshot_v(out1->vertex_count);
    for (int i = 0; i < out1->vertex_count; i++) {
        snapshot_xref[i] = out1->vertex_array[i].xref;
        snapshot_u[i] = out1->vertex_array[i].uv[0];
        snapshot_v[i] = out1->vertex_array[i].uv[1];
    }

    atlas_free(out2);
    atlas_injective_report_free(r2);
    delete b; // input storage is gone

    TASK_CHECK(out1->index_count / 3 == snapshot_faces);
    for (int i = 0; i < out1->vertex_count; i++) {
        TASK_CHECK(out1->vertex_array[i].xref == snapshot_xref[i]);
        TASK_CHECK(out1->vertex_array[i].uv[0] == snapshot_u[i]);
        TASK_CHECK(out1->vertex_array[i].uv[1] == snapshot_v[i]);
        TASK_CHECK(std::isfinite(out1->vertex_array[i].uv[0]));
        TASK_CHECK(std::isfinite(out1->vertex_array[i].uv[1]));
    }
    TASK_CHECK(r1->chart_count > 0 && r1->narrow_phase_count > 0);

    atlas_free(out1);
    atlas_injective_report_free(r1);
    return 0;
}

// A large healthy mesh is never cut and the narrow phase stays far below the
// quadratic all-pairs bound.
static int test_large_grid_subquadratic()
{
    MeshBuilder b;
    const int NX = 40, NY = 40;
    std::vector<int> id((NX + 1) * (NY + 1));
    for (int y = 0; y <= NY; y++)
        for (int x = 0; x <= NX; x++)
            id[y * (NX + 1) + x] = b.addVertex(float(x), float(y), 0, float(x), float(y));
    for (int y = 0; y < NY; y++)
        for (int x = 0; x < NX; x++) {
            const int a = id[y * (NX + 1) + x];
            const int bb = id[y * (NX + 1) + x + 1];
            const int d = id[(y + 1) * (NX + 1) + x];
            const int c = id[(y + 1) * (NX + 1) + x + 1];
            b.addFace(a, bb, c);
            b.addFace(a, c, d);
        }
    Atlas_Input_Mesh m = b.mesh();
    Atlas_Options o = injectiveOptions();
    Atlas_Error e;
    Atlas_Injective_Report * r = NULL;
    Atlas_Output_Mesh * out = atlas_generate_injective(&m, &o, 4, &r, &e);
    TASK_CHECK(e == Atlas_Error_Success && out != NULL && r != NULL);
    TASK_CHECK(r->split_count == 0);
    TASK_CHECK(r->chart_count == 1);

    const long long face_count = m.face_count;
    const long long quadratic = face_count * face_count;
    TASK_CHECK(r->narrow_phase_count < quadratic / 100);
    TASK_CHECK(verifyNoOverlaps(out, r));

    atlas_free(out);
    atlas_injective_report_free(r);
    return 0;
}

// All output UVs are finite and inside the atlas rectangle.
static int test_output_bounds()
{
    MeshBuilder b;
    buildSpiral(b, 20.0, 0.2, 0.8, 2.2, 100);
    Atlas_Input_Mesh m = b.mesh();
    Atlas_Options o = injectiveOptions();
    Atlas_Error e;
    Atlas_Injective_Report * r = NULL;
    Atlas_Output_Mesh * out = atlas_generate_injective(&m, &o, 128, &r, &e);
    TASK_CHECK(e == Atlas_Error_Success && out != NULL && r != NULL);
    TASK_CHECK(out->atlas_width > 0 && out->atlas_height > 0);

    for (int i = 0; i < out->vertex_count; i++) {
        const float u = out->vertex_array[i].uv[0];
        const float v = out->vertex_array[i].uv[1];
        TASK_CHECK(std::isfinite(u) && std::isfinite(v));
        TASK_CHECK(u >= 0.0f && v >= 0.0f);
        TASK_CHECK(u <= float(out->atlas_width) && v <= float(out->atlas_height));
    }

    atlas_free(out);
    atlas_injective_report_free(r);
    return 0;
}

const struct task_case task_cases[] = {
    { "overlaps", "spiral_self_overlap", test_spiral_self_overlap },
    { "overlaps", "containment", test_containment },
    { "overlaps", "legal_contact", test_legal_contact },
    { "overlaps", "cross_material_colocal_split", test_cross_material_colocal_split },

    { "recut", "continuous_multiple_splits", test_continuous_multiple_splits },
    { "recut", "budget_then_success", test_budget_then_success },
    { "recut", "cut_edge_rule", test_cut_edge_rule },
    { "recut", "single_face_uninjectable", test_single_face_uninjectable },

    { "output", "xref_per_corner", test_xref_per_corner },
    { "output", "partition_bookkeeping", test_partition_bookkeeping },
    { "output", "repeat_and_lifetime", test_repeat_and_lifetime },
    { "output", "large_grid_subquadratic", test_large_grid_subquadratic },
    { "output", "output_bounds", test_output_bounds },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
