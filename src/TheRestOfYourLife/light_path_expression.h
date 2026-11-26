#ifndef LIGHT_PATH_EXPRESSION_H
#define LIGHT_PATH_EXPRESSION_H
//==============================================================================================
// Light path expression (LPE) AOV support for "Ray Tracing: The Rest of Your Life".
//
// Event alphabet:
//   C - camera origin (the initial camera ray)
//   D - diffuse (Lambertian) scattering event
//   S - specular reflection event (metal, including non-zero fuzz; dielectric reflection,
//       including total internal reflection)
//   T - specular transmission event (dielectric refraction)
//   V - isotropic volume scattering event
//   E - surface emission contribution
//   B - background contribution
//
// An expression is matched against a complete event sequence (full match, never a
// substring search).  Syntax: concatenation, grouping '(', ')', alternation '|',
// Kleene star '*', one-or-more '+', optional '?', and '.' matching any single event.
//
// To the extent possible under law, the author(s) have dedicated all copyright and related
// and neighboring rights to this software to the public domain worldwide. This software is
// distributed without any warranty.
//==============================================================================================

#include "rtweekend.h"

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>


class hittable;
class camera;


enum path_event {
    PATH_C = 0,
    PATH_D,
    PATH_S,
    PATH_T,
    PATH_V,
    PATH_E,
    PATH_B,
    PATH_EVENT_COUNT
};


char path_event_to_char(path_event event);
bool path_event_from_char(char c, path_event& event);


class lpe_compile_error : public std::runtime_error {
  public:
    lpe_compile_error(const std::string& message, std::size_t byte_offset)
      : std::runtime_error(message), byte_offset(byte_offset) {}

    // Byte position inside the pattern string handed to the compiler.
    std::size_t byte_offset;
};


struct lpe_nfa_state {
    std::vector<int> epsilon;
    int next;             // Symbol-transition target, -1 when this state has none.
    path_event symbol;
    bool any;             // Wildcard '.' state, matches every event.
    lpe_nfa_state() : next(-1), symbol(PATH_C), any(false) {}
};


// A single compiled light path expression.  Compilation happens once; the resulting
// Thompson NFA is reused for every pixel and every contribution.
class light_path_expression {
  public:
    light_path_expression() : start(-1), accept(-1) {}
    light_path_expression(const std::string& name, const std::string& pattern);

    const std::string& name() const { return expr_name; }
    const std::string& pattern() const { return expr_pattern; }

    // Full-sequence match: the whole event sequence must be consumed.
    bool matches(const std::vector<path_event>& events) const;

  private:
    std::string expr_name;
    std::string expr_pattern;
    std::vector<lpe_nfa_state> states;
    int start;
    int accept;
};


// A set of named, independently compiled expressions.  Build once before rendering and
// share the immutable set across rays, pixels and cameras.
class light_path_set {
  public:
    light_path_set() {}

    // Throws lpe_compile_error (with a byte offset) when the pattern is invalid.
    void add(const std::string& name, const std::string& pattern);

    std::size_t size() const { return expressions.size(); }
    const light_path_expression& operator[](std::size_t i) const { return *expressions[i]; }

    // Writes one byte per expression: nonzero when the expression matches the full
    // sequence.  Multiple expressions may match; each expression is counted at most once.
    void matching_mask(const std::vector<path_event>& events,
                       std::vector<unsigned char>& mask) const;

  private:
    std::vector<shared_ptr<light_path_expression>> expressions;
};


// Per-ray diagnostic result.  All colors are linear and already carry the full path
// throughput weight (including the mixture pdf weight).
struct aov_ray_result {
    color beauty;
    std::vector<color> channels;   // One entry per expression in the set, in add() order.
};


// Trace one camera ray through the production integrator, collecting every matching AOV
// from that single path.  Random number consumption is identical to camera::ray_color.
aov_ray_result trace_aov_ray(const ray& r, const hittable& world, const hittable& lights,
                             const color& background, int max_depth,
                             const light_path_set& set);


// Owns the linear storage for one rendered frame and all of its AOV channels.
struct aov_frame {
    std::string frame_name;
    int image_width = 0;
    int image_height = 0;
    int samples_per_pixel = 0;

    std::vector<std::string> channel_names;
    std::vector<color> beauty;                  // Linear per-pixel averages.
    std::vector<std::vector<color>> channels;   // One width*height buffer per channel.

    std::size_t pixel_count() const {
        return static_cast<std::size_t>(image_width) * image_height;
    }

    // Gamma is applied here, at PPM output time only; the stored values stay linear.
    void write_ppm(std::ostream& out, const std::string& channel_name) const;
    void write_beauty_ppm(std::ostream& out) const;
};


// Render a full frame.  Each camera sample drives exactly one path shared by every
// channel, so overlapping expressions can be checked against each other.  The frame owns
// its own storage and can be kept around while another camera renders into another frame.
aov_frame render_aov(camera& cam, const hittable& world, const hittable& lights,
                     const light_path_set& set, const std::string& frame_name = "aov");


#endif
