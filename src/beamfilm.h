#ifndef BEAMFILM_H
#define BEAMFILM_H

#include <vector>
#include <string>

#include "vector.h"
#include "ray.h"
#include "scene.h"
#include "camera.h"
#include "kdtree.h"

// A point in continuous screen coordinates. Pixel (x,y) occupies
// [x,x+1] x [y,y+1], area one, with no half pixel shift.
struct ScreenPoint {
    double x, y;
    ScreenPoint(double x_=0.0, double y_=0.0) : x(x_), y(y_) {}
};

struct BeamPixel {
    Vec beauty;                  // Sum of all by_bounce entries
    std::vector<Vec> by_bounce; // D+1 entries; by_bounce[k] is the radiance
                                // reaching an EMIT surface after exactly k
                                // mirror reflections, integrated over the pixel
    std::vector<double> emitter_by_bounce; // Screen area terminating on an
                                // emitter, split by the number of reflections
    double emitter_area;        // Sum of emitter_by_bounce
    double miss_area;           // Screen area whose rays escaped the scene
    double truncated_area;      // Screen area stopped by the bounce limit D

    BeamPixel() : emitter_area(0.0), miss_area(0.0), truncated_area(0.0) {}
};

struct BeamFilm {
    int width;
    int height;
    int depth;                  // Mirror bounce limit D
    std::vector<BeamPixel> pixels; // Row major, index y*width + x

    // Conservative KD traversal counters accumulated over the whole query.
    unsigned long long nodes_visited;
    unsigned long long candidate_faces; // Faces handed out by KD leaves
    unsigned long long clipped_faces;   // Faces run through exact clipping

    BeamFilm() : width(0), height(0), depth(0),
                 nodes_visited(0), candidate_faces(0), clipped_faces(0) {}

    const BeamPixel& pixel(int x, int y) const {
        return pixels[(size_t)y * (size_t)width + (size_t)x];
    }
};

// Unnormalized integral over a convex screen sub-domain. Splitting a domain
// into convex pieces and adding their results reproduces the whole.
struct BeamSubResult {
    double area;                // Original screen area of the queried domain
    std::vector<Vec> integral;  // D+1 unnormalized per-bounce RGB integrals
    std::vector<double> emitter_by_bounce;
    double emitter_area;
    double miss_area;
    double truncated_area;
    unsigned long long nodes_visited;
    unsigned long long candidate_faces;
    unsigned long long clipped_faces;

    BeamSubResult() : area(0.0), emitter_area(0.0), miss_area(0.0),
                      truncated_area(0.0), nodes_visited(0),
                      candidate_faces(0), clipped_faces(0) {}
};

// Analytic beam transport. A pixel is treated as a continuous bundle of
// pinhole rays; planar mirrors split the bundle by the actually visible
// region and the reflected bundle is integrated in the original screen
// coordinates. No supersampling, centre/corner rays or fixed resolution
// buffers are used.
class BeamTracer {
public:
    BeamTracer(Scene &scene, Camera &camera);

    // Renders every camera pixel. Validates the scene, camera and depth and
    // throws std::invalid_argument before producing partial output. On
    // failure the film returned by last_film() is left untouched.
    BeamFilm render_frame(int depth);

    // Convex polygon sub-domain query in screen coordinates.
    BeamSubResult query(const std::vector<ScreenPoint> &polygon, int depth);

    bool has_film() const { return m_has_film; }
    const BeamFilm& last_film() const { return m_last_film; }

private:
    Scene &m_scene;
    Camera &m_camera;
    BeamFilm m_last_film;
    bool m_has_film;
};

// PPM (P6) beauty dump and the per-pixel/per-bounce CSV channel table.
// CSV columns: y,x,bounce,r,g,b,emitter_area,miss_area,truncated_area.
// miss_area and truncated_area are written on the bounce=0 row only.
void beamfilm_write_ppm(const BeamFilm &film, const std::string &path);
void beamfilm_write_csv(const BeamFilm &film, const std::string &path);

// Number of nodes in a mesh KD tree (root counts as one).
unsigned long long kd_tree_node_count(KDNode *node);

#endif // BEAMFILM_H
