#ifndef LIGHT_PATH_EXPRESSION_H
#define LIGHT_PATH_EXPRESSION_H
//==============================================================================================
// Light path expression (LPE) AOV support for book three ("Ray Tracing: The
// Rest of Your Life").
//
// A compiled light_path_expression matches a complete event sequence produced
// by a real camera path. Event letters:
//   C - camera origin (implicit first event)
//   D - diffuse scatter        S - specular reflection
//   T - specular transmission  V - isotropic volume scatter
//   E - surface emission       B - background contribution
//
// The grammar supports concatenation, parentheses, alternation '|', and the
// quantifiers '*', '+' and '?', plus '.' matching any single event. Matching is
// anchored to the whole sequence (no substring search). Expressions are parsed
// once and may be reused across pixels and frames.
//
// The integrator in this header mirrors camera::ray_color exactly, including
// random number consumption and the mixture-pdf weighting, so enabling AOV
// output never perturbs the underlying render.
//==============================================================================================

#include "rtweekend.h"
#include "hittable.h"
#include "pdf.h"
#include "material.h"

#include <map>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>


// Thrown by the light_path_expression constructor. The reported position is the
// zero-based byte offset inside the expression string.
class lpe_parse_error : public std::runtime_error {
  public:
    lpe_parse_error(const std::string& what, std::size_t pos)
      : std::runtime_error(what), pos_(pos) {}

    std::size_t position() const { return pos_; }

  private:
    std::size_t pos_;
};


class light_path_expression {
  public:
    // Compiles an expression; throws lpe_parse_error on unknown letters, empty
    // alternatives, unbalanced parentheses, quantifiers without an operand, or
    // an empty expression.
    explicit light_path_expression(const std::string& source);

    // True if the full event sequence (without any implicit prefix) matches.
    bool matches(const std::string& events) const;

    const std::string& source() const { return source_; }

  private:
    enum node_type {
        NODE_EMPTY,   // matches the empty sequence
        NODE_EVENT,   // one literal event letter, or any event when letter==0
        NODE_CONCAT,
        NODE_ALT,
        NODE_STAR,
        NODE_PLUS,
        NODE_OPTIONAL
    };

    struct node {
        node_type type;
        char letter;
        std::vector<node> children;

        node() : type(NODE_EMPTY), letter(0) {}
        explicit node(node_type t) : type(t), letter(0) {}
        node(node_type t, char c) : type(t), letter(c) {}
    };

    struct parser {
        const std::string& src;
        std::size_t pos;

        parser(const std::string& s) : src(s), pos(0) {}

        [[noreturn]] void fail(const std::string& msg) {
            throw lpe_parse_error(msg, pos < src.size() ? pos : src.size());
        }

        node parse_all();
        node parse_alternation();
        node parse_concatenation();
        node parse_primary();

        static bool valid_letter(char c);
        static node make_concat(node a, node b);
        static node make_alt(node a, node b);
        node apply_quantifier(node base, char op);
    };

    static void index_nodes(const node& n,
                            std::vector<const node*>& out);

    // Backtracking matcher with explicit ambiguity deduplication: reachable
    // sequence offsets per (node, offset) pair are computed once, so an
    // expression containing several equivalent match paths only counts once.
    struct matcher {
        const node& root;
        const std::string& events;

        matcher(const node& r, const std::string& e) : root(r), events(e) {}

        bool run();
    };

    std::string source_;
    node root_;
};


// One named expression plus its accumulated per-pixel linear RGB storage.
struct lpe_channel {
    std::string name;
    light_path_expression expression;
    std::vector<color> accum;  // width*height linear sums, one entry per pixel

    lpe_channel(const std::string& channel_name, const std::string& expr)
      : name(channel_name), expression(expr) {}
};


// A full-frame AOV render. Each object owns its own storage, so two cameras
// (or two interleaved renders) never share channel state.
class aov_frame {
  public:
    int image_width = 0;
    int image_height = 0;
    int samples_per_pixel = 0;
    std::vector<lpe_channel> channels;

    aov_frame() {}
    aov_frame(int width, int height, int spp)
      : image_width(width), image_height(height), samples_per_pixel(spp) {}

    // Registers a named expression; throws if the expression cannot be compiled
    // or the name is already in use. Must be called before render_aov.
    void add_channel(const std::string& name, const std::string& expression);

    const lpe_channel* find_channel(const std::string& name) const;
    lpe_channel* find_channel(const std::string& name);

    // Linear accumulated value (sum over samples, no 1/spp scaling, no gamma).
    color pixel(const lpe_channel& channel, int x, int y) const;

  private:
    std::map<std::string, std::size_t> index_by_name;

    friend class aov_camera;
};


// Camera-side AOV driver. It mirrors the public camera configuration fields and
// its get_ray() random consumption exactly; an aov_camera renders every
// requested channel from a single set of camera paths, so all channels see
// identical random walks.
class aov_camera {
  public:
    double aspect_ratio      = 1.0;
    int    image_width       = 100;
    int    samples_per_pixel = 10;
    int    max_depth         = 10;
    color  background;

    double vfov     = 90;
    point3 lookfrom = point3(0,0,0);
    point3 lookat   = point3(0,0,-1);
    vec3   vup      = vec3(0,1,0);

    double defocus_angle = 0;
    double focus_dist = 10;

    // Renders every channel of `frame` from shared camera paths and also
    // returns the ordinary linear beauty color (the unweighted sum of samples).
    void render_aov(aov_frame& frame, const hittable& world, const hittable& lights);

    // Linear beauty values accumulated during the last render_aov call.
    const std::vector<color>& beauty() const { return beauty_accum; }

    // Single-ray diagnostic entry: traces one real camera ray and distributes
    // every emission/background contribution among the frame's channels. The
    // returned color is the ordinary linear radiance along the ray, and
    // channel_values receives one linear contribution sum per frame channel.
    color trace_ray(aov_frame& frame, const ray& r,
                    const hittable& world, const hittable& lights,
                    std::vector<color>& channel_values) const;

  private:
    int    image_height;
    double pixel_samples_scale;
    int    sqrt_spp;
    double recip_sqrt_spp;
    point3 center;
    point3 pixel00_loc;
    vec3   pixel_delta_u;
    vec3   pixel_delta_v;
    vec3   u, v, w;
    vec3   defocus_disk_u;
    vec3   defocus_disk_v;

    std::vector<color> beauty_accum;

    void initialize();
    ray get_ray(int i, int j, int s_i, int s_j) const;
    vec3 sample_square_stratified(int s_i, int s_j) const;
    point3 defocus_disk_sample() const;

    // Recursive path integrator with event-prefix bookkeeping. Structurally
    // identical to camera::ray_color, including the order of random calls.
    // `throughput` is the fully weighted path throughput (attenuations and
    // mixture-pdf weights) arriving at the current vertex; `sink` receives
    // per-channel contributions for this ray.
    color ray_color_lpe(const ray& r, int depth, const hittable& world,
                        const hittable& lights, std::string prefix,
                        aov_frame& frame, color* sink, color throughput) const;

    // Records one already-weighted contribution under prefix + terminal.
    void record(aov_frame& frame, color* sink,
                const std::string& prefix, char terminal,
                const color& weighted) const;
};


// PPM display helpers. Gamma correction happens here, at display time only;
// channel accumulation stays strictly linear. Throws if the channel index is
// out of range.
void write_channel_ppm(std::ostream& out, const aov_frame& frame,
                       std::size_t channel_index);
void write_beauty_ppm(std::ostream& out, const aov_camera& cam,
                      const aov_frame& frame);


#endif
