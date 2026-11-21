// Injective chart generation: detect global UV overlaps and repair them by
// recursively cutting charts along dual-graph tree edges.
//
// This code is in the public domain.

#include "nvmesh.h" // pch

#include "InjectiveCharts.h"

#include "Atlas.h"
#include "LeastSquaresConformalMap.h"
#include "OrthogonalProjectionMap.h"
#include "SingleFaceMap.h"

#include "nvmesh/MeshTopology.h"
#include "nvmesh/halfedge/Mesh.h"
#include "nvmesh/halfedge/Face.h"
#include "nvmesh/halfedge/Edge.h"
#include "nvmesh/halfedge/Vertex.h"

#include "nvmath/Vector.inl"

#include "nvcore/Array.inl"

#include <algorithm>
#include <cmath>
#include <queue>
#include <set>
#include <vector>

using namespace nv;
using namespace nv::HalfEdge;

namespace
{
    const double AREA_EPSILON = 1e-10;      // Fraction of L^2 below which overlaps are ignored.
    const double FACE_AREA_EPSILON = 1e-7;  // Minimum valid face area as a fraction of L^2.

    struct Triangle2
    {
        double p[3][2];
        double mn[2];
        double mx[2];
        double signed_area;
        uint source_face;
    };

    static double triangleSignedArea(const double p[3][2])
    {
        return 0.5 * ((p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) -
                      (p[1][1] - p[0][1]) * (p[2][0] - p[0][0]));
    }

    // Sutherland-Hodgman clipping of polygon `poly` against the CCW edge a->b.
    static void clipPolygonEdge(
        std::vector< std::pair<double, double> > & poly,
        const double a[2], const double b[2])
    {
        std::vector< std::pair<double, double> > out;
        if (poly.empty()) { poly = out; return; }

        const double ex = b[0] - a[0];
        const double ey = b[1] - a[1];

        const size_t n = poly.size();
        for (size_t i = 0; i < n; i++) {
            const std::pair<double, double> & cur = poly[i];
            const std::pair<double, double> & nxt = poly[(i + 1) % n];

            const double cur_side = ex * (cur.second - a[1]) - ey * (cur.first - a[0]);
            const double nxt_side = ex * (nxt.second - a[1]) - ey * (nxt.first - a[0]);
            const bool cur_in = cur_side >= 0.0;
            const bool nxt_in = nxt_side >= 0.0;

            if (cur_in != nxt_in) {
                const double denom = (nxt.first - cur.first) * ey - (nxt.second - cur.second) * ex;
                if (denom != 0.0) {
                    const double t = ((a[0] - cur.first) * ey - (a[1] - cur.second) * ex) / denom;
                    out.push_back(std::make_pair(
                        cur.first + t * (nxt.first - cur.first),
                        cur.second + t * (nxt.second - cur.second)));
                }
            }
            if (nxt_in) {
                out.push_back(nxt);
            }
        }

        poly = out;
    }

    // Exact (double precision) intersection area of two triangles computed
    // with an independent polygon clipping implementation.
    static double triangleIntersectionArea(const Triangle2 & ta, const Triangle2 & tb)
    {
        double a[3][2], b[3][2];
        for (int i = 0; i < 3; i++) {
            a[i][0] = ta.p[i][0]; a[i][1] = ta.p[i][1];
            b[i][0] = tb.p[i][0]; b[i][1] = tb.p[i][1];
        }

        // Work with CCW polygons only.
        if (triangleSignedArea(a) < 0.0) {
            double tx = a[1][0], ty = a[1][1];
            a[1][0] = a[2][0]; a[1][1] = a[2][1];
            a[2][0] = tx; a[2][1] = ty;
        }
        if (triangleSignedArea(b) < 0.0) {
            double tx = b[1][0], ty = b[1][1];
            b[1][0] = b[2][0]; b[1][1] = b[2][1];
            b[2][0] = tx; b[2][1] = ty;
        }

        std::vector< std::pair<double, double> > poly;
        for (int i = 0; i < 3; i++) poly.push_back(std::make_pair(a[i][0], a[i][1]));

        for (int e = 0; e < 3 && !poly.empty(); e++) {
            clipPolygonEdge(poly, b[e], b[(e + 1) % 3]);
        }

        if (poly.size() < 3) return 0.0;

        double area = 0.0;
        for (size_t i = 0; i < poly.size(); i++) {
            const std::pair<double, double> & p0 = poly[i];
            const std::pair<double, double> & p1 = poly[(i + 1) % poly.size()];
            area += p0.first * p1.second - p1.first * p0.second;
        }
        return 0.5 * std::fabs(area);
    }

    static bool collectChartTriangles(const Chart * chart, std::vector<Triangle2> & tris)
    {
        tris.clear();

        const Mesh * chart_mesh = chart->chartMesh();
        const uint face_count = chart->faceCount();
        if (face_count == 0 || chart_mesh->faceCount() != face_count) return false;

        bool finite = true;

        for (uint f = 0; f < face_count; f++) {
            const Face * face = chart_mesh->faceAt(f);

            Triangle2 tri;
            tri.source_face = chart->faceAt(f);

            uint k = 0;
            for (Face::ConstEdgeIterator it(face->edges()); !it.isDone(); it.advance()) {
                const Vertex * vertex = it.vertex();
                if (k < 3) {
                    tri.p[k][0] = double(vertex->tex.x);
                    tri.p[k][1] = double(vertex->tex.y);
                    if (!std::isfinite(vertex->tex.x) || !std::isfinite(vertex->tex.y)) {
                        finite = false;
                    }
                }
                k++;
            }
            if (k != 3) return false;

            tri.mn[0] = std::min(std::min(tri.p[0][0], tri.p[1][0]), tri.p[2][0]);
            tri.mn[1] = std::min(std::min(tri.p[0][1], tri.p[1][1]), tri.p[2][1]);
            tri.mx[0] = std::max(std::max(tri.p[0][0], tri.p[1][0]), tri.p[2][0]);
            tri.mx[1] = std::max(std::max(tri.p[0][1], tri.p[1][1]), tri.p[2][1]);
            tri.signed_area = triangleSignedArea(tri.p);

            tris.push_back(tri);
        }

        return finite;
    }

    struct SweepEvent
    {
        double x;
        int type; // 0 = remove, 1 = add
        uint tri;

        bool operator<(const SweepEvent & e) const
        {
            if (x != e.x) return x < e.x;
            return type < e.type; // removals first at identical x
        }
    };

    // Sweep line candidate filter: only pairs whose axis aligned bounding
    // boxes overlap in both axes reach the exact narrow phase. This avoids
    // the quadratic all-pairs test on large healthy charts.
    static std::vector< std::pair<uint, uint> > sweepCandidates(const std::vector<Triangle2> & tris)
    {
        std::vector<SweepEvent> events;
        events.reserve(tris.size() * 2);
        for (uint i = 0; i < tris.size(); i++) {
            SweepEvent e0; e0.x = tris[i].mn[0]; e0.type = 1; e0.tri = i; events.push_back(e0);
            SweepEvent e1; e1.x = tris[i].mx[0]; e1.type = 0; e1.tri = i; events.push_back(e1);
        }
        std::sort(events.begin(), events.end());

        // Active triangles ordered by (min y, local index).
        std::set< std::pair<double, uint> > active;
        std::vector< std::pair<uint, uint> > candidates;

        for (size_t i = 0; i < events.size(); i++) {
            const SweepEvent & ev = events[i];
            const Triangle2 & tri = tris[ev.tri];

            if (ev.type == 0) {
                active.erase(std::make_pair(tri.mn[1], ev.tri));
            }
            else {
                std::set< std::pair<double, uint> >::iterator it = active.begin();
                for (; it != active.end(); ++it) {
                    if (it->first > tri.mx[1]) break;
                    const Triangle2 & other = tris[it->second];
                    if (other.mx[1] >= tri.mn[1]) {
                        uint a = it->second;
                        uint b = ev.tri;
                        if (a > b) std::swap(a, b);
                        candidates.push_back(std::make_pair(a, b));
                    }
                }
                active.insert(std::make_pair(tri.mn[1], ev.tri));
            }
        }

        return candidates;
    }

    struct ValidationResult
    {
        ValidationResult() :
            ok(false),
            local_failure(false),
            narrow_phase_count(0)
        {
            witness_face[0] = 0;
            witness_face[1] = 0;
        }

        bool ok;
        bool local_failure;
        uint witness_face[2];
        uint64 narrow_phase_count;
    };

    // Validate one chart: non zero, consistently oriented area and no
    // positive-area triangle intersections in UV space. Shared edges, shared
    // vertices and boundary contacts produce zero-area clipping results and
    // are therefore not flagged, while fold-back coverage of adjacent faces
    // still yields a positive intersection area and is detected.
    static ValidationResult validateChart(const Chart * chart)
    {
        ValidationResult result;

        if (!chart->isDisk() || chart->faceCount() == 0) {
            result.local_failure = true;
            uint smallest = ~0u;
            for (uint i = 0; i < chart->faceCount(); i++) {
                smallest = std::min(smallest, chart->faceAt(i));
            }
            result.witness_face[0] = result.witness_face[1] = smallest;
            return result;
        }

        std::vector<Triangle2> tris;
        if (!collectChartTriangles(chart, tris) || tris.empty()) {
            result.local_failure = true;
            result.witness_face[0] = result.witness_face[1] = chart->faceAt(0);
            return result;
        }

        double bbox_min[2] = { tris[0].mn[0], tris[0].mn[1] };
        double bbox_max[2] = { tris[0].mx[0], tris[0].mx[1] };
        for (size_t i = 1; i < tris.size(); i++) {
            bbox_min[0] = std::min(bbox_min[0], tris[i].mn[0]);
            bbox_min[1] = std::min(bbox_min[1], tris[i].mn[1]);
            bbox_max[0] = std::max(bbox_max[0], tris[i].mx[0]);
            bbox_max[1] = std::max(bbox_max[1], tris[i].mx[1]);
        }

        const double L = std::max(bbox_max[0] - bbox_min[0], bbox_max[1] - bbox_min[1]);
        if (!std::isfinite(L) || L <= 0.0) {
            result.local_failure = true;
            result.witness_face[0] = result.witness_face[1] = tris[0].source_face;
            return result;
        }
        const double L2 = L * L;

        // Local checks: non zero area and one consistent orientation.
        int orientation = 0;
        for (size_t i = 0; i < tris.size(); i++) {
            if (std::fabs(tris[i].signed_area) <= FACE_AREA_EPSILON * L2) {
                result.local_failure = true;
                result.witness_face[0] = result.witness_face[1] = tris[i].source_face;
                return result;
            }
            const int sign = tris[i].signed_area > 0.0 ? 1 : -1;
            if (orientation == 0) orientation = sign;
            else if (orientation != sign) {
                result.local_failure = true;
                result.witness_face[0] = result.witness_face[1] = tris[i].source_face;
                return result;
            }
        }

        const double overlap_epsilon = AREA_EPSILON * L2;

        std::vector< std::pair<uint, uint> > candidates = sweepCandidates(tris);
        result.narrow_phase_count = candidates.size();

        bool found = false;
        uint best_a = 0, best_b = 0;

        for (size_t i = 0; i < candidates.size(); i++) {
            const uint ia = candidates[i].first;
            const uint ib = candidates[i].second;
            const double area = triangleIntersectionArea(tris[ia], tris[ib]);
            if (area > overlap_epsilon) {
                uint fa = tris[ia].source_face;
                uint fb = tris[ib].source_face;
                if (fa > fb) std::swap(fa, fb);
                if (!found || fa < best_a || (fa == best_a && fb < best_b)) {
                    found = true;
                    best_a = fa;
                    best_b = fb;
                }
            }
        }

        if (found) {
            result.witness_face[0] = best_a;
            result.witness_face[1] = best_b;
            return result;
        }

        result.ok = true;
        return result;
    }

    // Dual graph of a chart: adjacency of source faces through the original mesh.
    static void buildDualGraph(
        const Mesh * mesh,
        const Array<uint> & faces,
        std::vector< std::vector<uint> > & adjacency,
        std::vector<int> & member)
    {
        const uint n = faces.count();
        member.assign(mesh->faceCount(), -1);
        for (uint i = 0; i < n; i++) {
            member[faces[i]] = int(i);
        }

        adjacency.assign(n, std::vector<uint>());

        for (uint i = 0; i < n; i++) {
            const Face * face = mesh->faceAt(faces[i]);
            for (Face::ConstEdgeIterator it(face->edges()); !it.isDone(); it.advance()) {
                const Edge * edge = it.current();
                const Face * neighbor = edge->pair->face;
                if (neighbor != NULL) {
                    const int local = member[neighbor->id];
                    if (local >= 0 && uint(local) != i) {
                        adjacency[i].push_back(neighbor->id);
                    }
                }
            }
            std::sort(adjacency[i].begin(), adjacency[i].end());
            adjacency[i].erase(std::unique(adjacency[i].begin(), adjacency[i].end()), adjacency[i].end());
        }
    }

    struct TreeSplit
    {
        TreeSplit() { edge_face[0] = edge_face[1] = 0; }

        uint edge_face[2];
        std::vector<uint> group[2];
    };

    // BFS tree rooted at the smallest source face, adjacency expanded in
    // ascending source face order. Selects the tree edge on the unique path
    // between the two witness faces that maximizes the smaller resulting
    // group; ties are broken by endpoint source face ids.
    static bool selectTreeSplit(
        const Array<uint> & faces,
        const std::vector< std::vector<uint> > & adjacency,
        const std::vector<int> & member,
        uint witness_a,
        uint witness_b,
        TreeSplit & split)
    {
        const uint n = faces.count();
        if (n < 2) return false;

        std::vector<int> parent(n, -1);
        std::vector<int> depth(n, -1);
        std::vector<uint> order;
        order.reserve(n);

        // faces are sorted ascending, so local index 0 holds the smallest id.
        std::queue<uint> queue;
        queue.push(0);
        depth[0] = 0;
        while (!queue.empty()) {
            const uint cur = queue.front();
            queue.pop();
            order.push_back(cur);
            for (size_t i = 0; i < adjacency[cur].size(); i++) {
                const int nxt = member[adjacency[cur][i]];
                if (nxt >= 0 && depth[nxt] < 0) {
                    depth[nxt] = depth[cur] + 1;
                    parent[nxt] = int(cur);
                    queue.push(uint(nxt));
                }
            }
        }

        const int la = member[witness_a];
        const int lb = member[witness_b];
        if (la < 0 || lb < 0) return false;

        // Unique tree path between the witnesses, stored as local indices.
        std::vector<uint> path_a, path_b;
        int na = la, nb = lb;
        while (depth[na] > depth[nb]) { path_a.push_back(uint(na)); na = parent[na]; }
        while (depth[nb] > depth[na]) { path_b.push_back(uint(nb)); nb = parent[nb]; }
        while (na != nb) {
            path_a.push_back(uint(na)); na = parent[na];
            path_b.push_back(uint(nb)); nb = parent[nb];
        }
        const int lca = na;

        std::vector<uint> path = path_a;
        path.push_back(uint(lca));
        for (size_t i = path_b.size(); i > 0; i--) path.push_back(path_b[i - 1]);

        if (path.size() < 2) return false;

        // Subtree sizes for child -> parent edges.
        std::vector<uint> subtree(n, 1);
        for (size_t i = order.size(); i > 0; i--) {
            const uint node = order[i - 1];
            if (parent[node] >= 0) subtree[parent[node]] += subtree[node];
        }

        bool have = false;
        uint best_small = 0;
        uint best_ep0 = 0, best_ep1 = 0;
        uint best_child = 0;

        for (size_t i = 0; i + 1 < path.size(); i++) {
            const uint u = path[i];
            const uint v = path[i + 1];
            uint child;
            if (parent[v] == int(u)) child = v;
            else if (parent[u] == int(v)) child = u;
            else continue; // defensive: path must follow tree edges

            const uint small = std::min(subtree[child], n - subtree[child]);
            const uint ep0 = std::min(faces[u], faces[v]);
            const uint ep1 = std::max(faces[u], faces[v]);

            if (!have || small > best_small ||
                (small == best_small && (ep0 < best_ep0 || (ep0 == best_ep0 && ep1 < best_ep1))))
            {
                have = true;
                best_small = small;
                best_ep0 = ep0;
                best_ep1 = ep1;
                best_child = child;
            }
        }

        if (!have || best_small == 0 || best_small == n) return false;

        split.edge_face[0] = best_ep0;
        split.edge_face[1] = best_ep1;

        split.group[0].clear();
        split.group[1].clear();
        for (uint i = 0; i < n; i++) {
            // Membership of i in best_child's subtree.
            int node = int(i);
            bool in_subtree = false;
            while (node >= 0) {
                if (node == int(best_child)) { in_subtree = true; break; }
                node = parent[node];
            }
            split.group[in_subtree ? 0 : 1].push_back(faces[i]);
        }

        if (split.group[0].empty() || split.group[1].empty()) return false;
        return true;
    }

    // Farthest source face from witness in the BFS tree; ties go to the
    // smallest source face id.
    static uint farthestTreeFace(
        const Array<uint> & faces,
        const std::vector< std::vector<uint> > & adjacency,
        const std::vector<int> & member,
        uint witness)
    {
        const uint n = faces.count();

        std::vector< std::vector<uint> > tree_adj(n);
        std::vector<int> depth(n, -1);

        std::queue<uint> queue;
        queue.push(0);
        depth[0] = 0;
        while (!queue.empty()) {
            const uint cur = queue.front();
            queue.pop();
            for (size_t i = 0; i < adjacency[cur].size(); i++) {
                const int nxt = member[adjacency[cur][i]];
                if (nxt >= 0 && depth[nxt] < 0) {
                    depth[nxt] = depth[cur] + 1;
                    tree_adj[cur].push_back(uint(nxt));
                    tree_adj[nxt].push_back(cur);
                    queue.push(uint(nxt));
                }
            }
        }

        const int start = member[witness];
        std::vector<int> dist(n, -1);
        std::queue<uint> bfs;
        if (start >= 0) {
            dist[start] = 0;
            bfs.push(uint(start));
        }
        while (!bfs.empty()) {
            const uint cur = bfs.front();
            bfs.pop();
            for (size_t i = 0; i < tree_adj[cur].size(); i++) {
                const uint nxt = tree_adj[cur][i];
                if (dist[nxt] < 0) {
                    dist[nxt] = dist[cur] + 1;
                    bfs.push(nxt);
                }
            }
        }

        uint best_face = faces[0];
        int best_dist = -1;
        for (uint i = 0; i < n; i++) {
            if (dist[i] > best_dist || (dist[i] == best_dist && faces[i] < best_face)) {
                best_dist = dist[i];
                best_face = faces[i];
            }
        }
        return best_face;
    }

    // Rebuild a chart from a source face subset and run the existing LSCM /
    // single face parameterization on it. Input UVs are never reused for a
    // descendant: a fresh parameterization is always computed.
    static Chart * rebuildAndParameterize(const Mesh * mesh, const std::vector<uint> & group)
    {
        Array<uint> face_array;
        for (size_t i = 0; i < group.size(); i++) face_array.append(group[i]);

        AutoPtr<Chart> chart(new Chart());
        chart->build(mesh, face_array, /*from_uvs=*/false);

        if (chart->isDisk()) {
            if (chart->faceCount() == 1) {
                computeSingleFaceMap(chart->unifiedMesh());
            }
            else if (!computeLeastSquaresConformalMap(chart->unifiedMesh(), /*pinned_boundary=*/false)) {
                // Fall back to an orthogonal projection if LSCM cannot solve.
                computeOrthogonalProjectionMap(chart->unifiedMesh());
            }
            chart->transferParameterization();
        }

        return chart.release();
    }

    struct ChartNode
    {
        ChartNode() : chart(NULL), parent(~0u), valid(false) {}

        Chart * chart;  // NULL once the node has been split and replaced.
        uint parent;    // Stable node id of the parent (~0 for initial charts).
        bool valid;
    };

    // Validate the whole packed atlas. Packing only applies a per-chart
    // rotation, uniform scale and translation, but we still re-check every
    // triangle pair (inside and across charts) against the atlas scale L.
    static bool validateAtlasPacking(const Atlas & atlas, uint64 & narrow_phase_count)
    {
        narrow_phase_count = 0;

        std::vector<Triangle2> all_tris;

        double bbox_min[2] = { 0.0, 0.0 };
        double bbox_max[2] = { 0.0, 0.0 };
        bool have_bounds = false;

        const uint chart_count = atlas.chartCount();
        for (uint c = 0; c < chart_count; c++) {
            const Chart * chart = atlas.chartAt(c);
            std::vector<Triangle2> tris;
            if (!collectChartTriangles(chart, tris)) return false;

            for (size_t i = 0; i < tris.size(); i++) {
                for (int k = 0; k < 3; k++) {
                    if (!std::isfinite(tris[i].p[k][0]) || !std::isfinite(tris[i].p[k][1])) return false;
                }
                for (int k = 0; k < 2; k++) {
                    if (!have_bounds) { bbox_min[k] = tris[i].mn[k]; bbox_max[k] = tris[i].mx[k]; }
                    else {
                        bbox_min[k] = std::min(bbox_min[k], tris[i].mn[k]);
                        bbox_max[k] = std::max(bbox_max[k], tris[i].mx[k]);
                    }
                }
                have_bounds = true;
                all_tris.push_back(tris[i]);
            }
        }

        if (!have_bounds) return false;

        const double L = std::max(bbox_max[0] - bbox_min[0], bbox_max[1] - bbox_min[1]);
        if (!std::isfinite(L) || L <= 0.0) return false;
        const double overlap_epsilon = AREA_EPSILON * L * L;

        std::vector< std::pair<uint, uint> > candidates = sweepCandidates(all_tris);
        narrow_phase_count += candidates.size();

        for (size_t i = 0; i < candidates.size(); i++) {
            const uint ia = candidates[i].first;
            const uint ib = candidates[i].second;
            const double area = triangleIntersectionArea(all_tris[ia], all_tris[ib]);
            if (area > overlap_epsilon) return false;
        }

        return true;
    }

    struct LeafOrder
    {
        uint node_id;
        uint min_face;

        bool operator<(const LeafOrder & other) const { return min_face < other.min_face; }
    };

} // namespace

NVMESH_API InjectiveStatus nv::generateInjectiveAtlas(
    Mesh * mesh,
    uint max_charts,
    const InjectiveOptions & options,
    Atlas *& atlas_out,
    InjectiveReport & report)
{
    atlas_out = NULL;

    if (mesh == NULL || mesh->faceCount() == 0 || max_charts == 0) {
        return InjectiveStatus_Invalid_Mesh;
    }

    report.narrow_phase_count = 0;
    report.splits.clear();
    report.node_faces.clear();
    report.node_parent.clear();
    report.chart_faces.clear();
    report.chart_parent_node.clear();
    report.face_chart.clear();

    // Initial extraction, preserving the input parameterization.
    AutoPtr<Atlas> atlas(new Atlas());
    atlas->extractCharts(mesh, /*ensure_disk_charts=*/true);

    MeshCharts * mesh_charts = atlas->meshAt(0);
    const uint initial_count = mesh_charts->chartCount();
    if (initial_count == 0 || initial_count > max_charts) {
        return InjectiveStatus_Budget_Exceeded;
    }

    std::vector<ChartNode> nodes;
    // The final node count never exceeds 2 * faceCount - 1, so cap the hint
    // to avoid a pathological reservation when callers pass a huge budget.
    nodes.reserve(std::min(max_charts, mesh->faceCount() * 2));

    for (uint i = 0; i < initial_count; i++) {
        ChartNode node;
        node.chart = mesh_charts->chartAt(i);
        node.parent = ~0u;
        nodes.push_back(node);

        Array<uint> faces;
        for (uint f = 0; f < node.chart->faceCount(); f++) {
            faces.append(node.chart->faceAt(f));
        }
        report.node_faces.append(faces);
        report.node_parent.append(~0u);
    }

    // Release chart ownership to the node array without deleting anything.
    mesh_charts->releaseCharts();

    InjectiveStatus status = InjectiveStatus_Ok;

    while (status == InjectiveStatus_Ok) {
        bool all_valid = true;

        for (size_t i = 0; i < nodes.size(); i++) {
            if (nodes[i].chart == NULL || nodes[i].valid) continue;

            const ValidationResult vr = validateChart(nodes[i].chart);
            report.narrow_phase_count += vr.narrow_phase_count;

            if (vr.ok) {
                nodes[i].valid = true;
                continue;
            }

            all_valid = false;

            // A single face that cannot produce a valid map is hopeless.
            if (nodes[i].chart->faceCount() <= 1) {
                status = InjectiveStatus_Uninjectable;
                break;
            }

            // Budget check: this cut would replace one leaf with two.
            uint leaf_count = 0;
            for (size_t j = 0; j < nodes.size(); j++) {
                if (nodes[j].chart != NULL) leaf_count++;
            }
            if (leaf_count + 1 > max_charts) {
                status = InjectiveStatus_Budget_Exceeded;
                break;
            }

            // Dual graph of this chart on the source faces.
            Array<uint> faces_sorted;
            for (uint f = 0; f < nodes[i].chart->faceCount(); f++) {
                faces_sorted.append(nodes[i].chart->faceAt(f));
            }
            std::sort(faces_sorted.begin(), faces_sorted.end());

            std::vector< std::vector<uint> > adjacency;
            std::vector<int> member;
            buildDualGraph(mesh, faces_sorted, adjacency, member);

            uint witness_a, witness_b;
            if (vr.local_failure) {
                witness_a = vr.witness_face[0];
                witness_b = farthestTreeFace(faces_sorted, adjacency, member, witness_a);
            }
            else {
                witness_a = vr.witness_face[0];
                witness_b = vr.witness_face[1];
            }

            TreeSplit split;
            if (!selectTreeSplit(faces_sorted, adjacency, member, witness_a, witness_b, split)) {
                status = InjectiveStatus_Uninjectable;
                break;
            }

            // Every split must produce two non empty strict subsets.
            if (split.group[0].empty() || split.group[1].empty() ||
                split.group[0].size() + split.group[1].size() != nodes[i].chart->faceCount())
            {
                status = InjectiveStatus_Uninjectable;
                break;
            }

            Chart * child0 = rebuildAndParameterize(mesh, split.group[0]);
            Chart * child1 = rebuildAndParameterize(mesh, split.group[1]);

            const uint parent_node = uint(i);
            const uint child_node0 = uint(nodes.size());
            const uint child_node1 = uint(nodes.size()) + 1;

            InjectiveSplitRecord record;
            record.parent_node = parent_node;
            record.child_node[0] = child_node0;
            record.child_node[1] = child_node1;
            record.witness_face[0] = vr.witness_face[0];
            record.witness_face[1] = vr.witness_face[1];
            record.cut_edge[0] = split.edge_face[0];
            record.cut_edge[1] = split.edge_face[1];
            for (uint f = 0; f < nodes[i].chart->faceCount(); f++) {
                record.parent_faces.append(nodes[i].chart->faceAt(f));
            }
            for (int g = 0; g < 2; g++) {
                for (size_t f = 0; f < split.group[g].size(); f++) {
                    record.child_faces[g].append(split.group[g][f]);
                }
            }
            report.splits.append(record);

            // Replace the parent node with the two children.
            delete nodes[i].chart;
            nodes[i].chart = NULL;
            nodes[i].valid = false;

            ChartNode n0;
            n0.chart = child0;
            n0.parent = parent_node;
            ChartNode n1;
            n1.chart = child1;
            n1.parent = parent_node;
            nodes.push_back(n0);
            nodes.push_back(n1);

            Array<uint> node_faces0, node_faces1;
            for (size_t f = 0; f < split.group[0].size(); f++) node_faces0.append(split.group[0][f]);
            for (size_t f = 0; f < split.group[1].size(); f++) node_faces1.append(split.group[1][f]);
            report.node_faces.append(node_faces0);
            report.node_faces.append(node_faces1);
            report.node_parent.append(parent_node);
            report.node_parent.append(parent_node);

            break; // Restart validation over the new leaf set.
        }

        if (all_valid) break;
    }

    if (status != InjectiveStatus_Ok) {
        for (size_t i = 0; i < nodes.size(); i++) {
            delete nodes[i].chart; // NULL entries are harmless
        }
        return status;
    }

    // Assemble final charts, ordered by smallest source face.
    std::vector<LeafOrder> leaf_order;
    for (size_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].chart == NULL || !nodes[i].valid) continue;
        LeafOrder order;
        order.node_id = uint(i);
        order.min_face = ~0u;
        for (uint f = 0; f < nodes[i].chart->faceCount(); f++) {
            order.min_face = std::min(order.min_face, nodes[i].chart->faceAt(f));
        }
        leaf_order.push_back(order);
    }

    if (leaf_order.empty() || leaf_order.size() > max_charts) {
        for (size_t i = 0; i < nodes.size(); i++) delete nodes[i].chart;
        return leaf_order.empty() ? InjectiveStatus_Uninjectable : InjectiveStatus_Budget_Exceeded;
    }

    std::sort(leaf_order.begin(), leaf_order.end());

    for (size_t i = 0; i < leaf_order.size(); i++) {
        ChartNode & node = nodes[leaf_order[i].node_id];
        mesh_charts->appendChart(node.chart);
        node.chart = NULL;
    }
    for (size_t i = 0; i < nodes.size(); i++) {
        delete nodes[i].chart; // remaining invalid leftovers, NULL for leaves
    }

    mesh_charts->rebuildIndices();

    // Conservative packing is always enabled for this entry point.
    atlas->packCharts(
        options.packing_quality,
        options.texels_per_unit,
        options.block_align,
        /*conservative=*/true);

    uint64 packing_narrow_count = 0;
    if (!validateAtlasPacking(*atlas, packing_narrow_count)) {
        return InjectiveStatus_Uninjectable;
    }
    report.narrow_phase_count += packing_narrow_count;

    // Fill the final partition report.
    report.face_chart.resize(mesh->faceCount());

    for (uint c = 0; c < mesh_charts->chartCount(); c++) {
        const Chart * chart = mesh_charts->chartAt(c);
        const uint node_id = leaf_order[c].node_id;

        Array<uint> faces;
        for (uint f = 0; f < chart->faceCount(); f++) {
            const uint source_face = chart->faceAt(f);
            faces.append(source_face);
            report.face_chart[source_face] = c;
        }
        report.chart_faces.append(faces);
        report.chart_parent_node.append(nodes[node_id].parent);
    }

    atlas_out = atlas.release();
    return InjectiveStatus_Ok;
}
