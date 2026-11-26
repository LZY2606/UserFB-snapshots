//==============================================================================================
// Light path expression (LPE) AOV support — implementation.
//
// The pattern compiler builds a Thompson-construction NFA, so ambiguous patterns that have
// several equivalent ways to match the same sequence still report the match exactly once.
// The tracing entry points mirror camera::ray_color term by term; only the bookkeeping of
// the event prefix and the channel writes is added, so the random number stream matches the
// production integrator.
//
// To the extent possible under law, the author(s) have dedicated all copyright and related
// and neighboring rights to this software to the public domain worldwide. This software is
// distributed without any warranty.
//==============================================================================================

#include "light_path_expression.h"

#include "camera.h"
#include "hittable_list.h"
#include "material.h"
#include "pdf.h"

#include <algorithm>
#include <ostream>


char path_event_to_char(path_event event) {
    static const char letters[PATH_EVENT_COUNT] = { 'C', 'D', 'S', 'T', 'V', 'E', 'B' };
    return letters[event];
}


bool path_event_from_char(char c, path_event& event) {
    switch (c) {
        case 'C': event = PATH_C; return true;
        case 'D': event = PATH_D; return true;
        case 'S': event = PATH_S; return true;
        case 'T': event = PATH_T; return true;
        case 'V': event = PATH_V; return true;
        case 'E': event = PATH_E; return true;
        case 'B': event = PATH_B; return true;
        default: return false;
    }
}


namespace {


struct fragment {
    int start;
    int accept;
};


// Recursive-descent parser with Thompson NFA construction.
class lpe_parser {
  public:
    lpe_parser(const std::string& source, std::vector<lpe_nfa_state>& states)
      : src(source), pos(0), states(states) {}

    fragment parse_top() {
        if (src.empty())
            throw lpe_compile_error("empty expression", 0);

        fragment frag = parse_alternation();

        if (pos != src.size()) {
            // The only token that can survive the grammar here is a stray ')'.
            throw lpe_compile_error("unmatched parenthesis", pos);
        }
        return frag;
    }

  private:
    const std::string& src;
    std::size_t pos;
    std::vector<lpe_nfa_state>& states;

    char peek() const { return pos < src.size() ? src[pos] : '\0'; }

    bool at_branch_end() const {
        char c = peek();
        return c == '\0' || c == '|' || c == ')';
    }

    int new_state() {
        states.push_back(lpe_nfa_state());
        return static_cast<int>(states.size()) - 1;
    }

    void link_epsilon(int from, int to) {
        states[from].epsilon.push_back(to);
    }

    fragment parse_alternation() {
        fragment frag = parse_concatenation();

        while (peek() == '|') {
            std::size_t op_pos = pos;
            ++pos;
            if (at_branch_end())
                throw lpe_compile_error("empty alternation branch", op_pos);

            fragment right = parse_concatenation();
            int s = new_state();
            int a = new_state();
            link_epsilon(s, frag.start);
            link_epsilon(s, right.start);
            link_epsilon(frag.accept, a);
            link_epsilon(right.accept, a);
            frag = fragment{s, a};
        }
        return frag;
    }

    fragment parse_concatenation() {
        bool have = false;
        fragment frag{0, 0};

        while (!at_branch_end()) {
            fragment piece = parse_quantified();
            if (!have) {
                frag = piece;
                have = true;
            } else {
                link_epsilon(frag.accept, piece.start);
                frag.accept = piece.accept;
            }
        }

        if (!have)
            throw lpe_compile_error("empty expression branch", pos);
        return frag;
    }

    fragment parse_quantified() {
        fragment frag = parse_atom();

        while (peek() == '*' || peek() == '+' || peek() == '?') {
            char quantifier = src[pos];
            ++pos;

            int s = new_state();
            int a = new_state();

            if (quantifier == '*') {
                link_epsilon(s, frag.start);
                link_epsilon(s, a);
                link_epsilon(frag.accept, frag.start);
                link_epsilon(frag.accept, a);
            } else if (quantifier == '+') {
                link_epsilon(s, frag.start);
                link_epsilon(frag.accept, frag.start);
                link_epsilon(frag.accept, a);
            } else { // '?'
                link_epsilon(s, frag.start);
                link_epsilon(s, a);
                link_epsilon(frag.accept, a);
            }
            frag = fragment{s, a};
        }
        return frag;
    }

    fragment parse_atom() {
        char c = peek();

        if (c == '(') {
            std::size_t open_pos = pos;
            ++pos;
            if (at_branch_end())
                throw lpe_compile_error("empty parenthesised group", open_pos);

            fragment frag = parse_alternation();
            if (peek() != ')')
                throw lpe_compile_error("unmatched parenthesis", open_pos);
            ++pos;
            return frag;
        }

        if (c == '*' || c == '+' || c == '?')
            throw lpe_compile_error("quantifier without operand", pos);

        if (c == ')' || c == '|' || c == '\0')
            throw lpe_compile_error("operator without operand", pos);

        std::size_t symbol_pos = pos;
        ++pos;

        path_event event;
        bool wildcard = (c == '.');
        if (!wildcard && !path_event_from_char(c, event))
            throw lpe_compile_error("unknown event letter", symbol_pos);

        int s = new_state();
        int a = new_state();
        states[s].next = a;
        states[s].any = wildcard;
        if (!wildcard)
            states[s].symbol = event;
        return fragment{s, a};
    }
};


// Epsilon closure of a single state.
void epsilon_closure(int start, const std::vector<lpe_nfa_state>& states,
                     std::vector<int>& out, std::vector<unsigned char>& seen) {
    std::vector<int> stack;
    stack.push_back(start);
    seen[start] = 1;

    while (!stack.empty()) {
        int s = stack.back();
        stack.pop_back();
        out.push_back(s);

        for (int to : states[s].epsilon) {
            if (!seen[to]) {
                seen[to] = 1;
                stack.push_back(to);
            }
        }
    }
}


bool nfa_matches(const std::vector<lpe_nfa_state>& states, int start, int accept,
                 const std::vector<path_event>& events) {
    std::vector<unsigned char> seen(states.size(), 0);
    std::vector<int> current, next;
    epsilon_closure(start, states, current, seen);

    for (path_event event : events) {
        std::fill(seen.begin(), seen.end(), static_cast<unsigned char>(0));
        next.clear();

        for (int s : current) {
            const lpe_nfa_state& state = states[s];
            if (state.next >= 0 && (state.any || state.symbol == event))
                epsilon_closure(state.next, states, next, seen);
        }

        current.swap(next);
        if (current.empty())
            return false;
    }

    for (int s : current) {
        if (s == accept)
            return true;
    }
    return false;
}


// The integrator: one instance per traced ray, so two cameras tracing interleaved rays
// never share prefix or channel state.
class aov_integrator {
  public:
    aov_integrator(const hittable& world, const hittable& lights, const color& background,
                   const light_path_set& set, std::vector<color>& channel_storage)
      : world(world), lights(lights), background(background), set(set),
        channels(channel_storage) {
        prefix.push_back(PATH_C);
        channels.assign(set.size(), color(0, 0, 0));
    }

    color trace(const ray& r, int depth) {
        return trace(r, depth, color(1, 1, 1));
    }

  private:
    const hittable& world;
    const hittable& lights;
    const color& background;
    const light_path_set& set;
    std::vector<color>& channels;
    std::vector<path_event> prefix;
    std::vector<unsigned char> mask;

    color trace(const ray& r, int depth, const color& throughput) {
        // Depth truncation gathers no light, matching camera::ray_color.
        if (depth <= 0)
            return color(0, 0, 0);

        hit_record rec;

        // Miss: the background contribution terminates the path with a B event.  The
        // channel receives throughput * background, i.e. the fully weighted radiance.
        if (!world.hit(r, interval(0.001, infinity), rec)) {
            prefix.push_back(PATH_B);
            contribute(throughput * background);
            prefix.pop_back();
            return background;
        }

        scatter_record srec;
        color color_from_emission = rec.mat->emitted(r, rec, rec.u, rec.v, rec.p);

        if (!rec.mat->scatter(r, rec, srec)) {
            prefix.push_back(PATH_E);
            contribute(throughput * color_from_emission);
            prefix.pop_back();
            return color_from_emission;
        }

        // Emission is matched with a temporary E suffix.  It is never left on the
        // persistent prefix, so a material that both emits and scatters does not stain
        // later contributions with E.  An exactly black emission is the common case for
        // non-emissive materials; skip only that exact no-op, never a merely small value.
        if (color_from_emission.x() != 0.0 || color_from_emission.y() != 0.0
            || color_from_emission.z() != 0.0) {
            prefix.push_back(PATH_E);
            contribute(throughput * color_from_emission);
            prefix.pop_back();
        }

        if (srec.skip_pdf) {
            color next_throughput = throughput * srec.attenuation;
            color incoming = advance(srec.event, srec.skip_pdf_ray, depth - 1,
                                     next_throughput);
            return color_from_emission + srec.attenuation * incoming;
        }

        auto light_ptr = make_shared<hittable_pdf>(lights, rec.p);
        mixture_pdf p(light_ptr, srec.pdf_ptr);

        ray scattered = ray(rec.p, p.generate(), r.time());
        auto pdf_value = p.value(scattered.direction());
        double scattering_pdf = rec.mat->scattering_pdf(r, rec, scattered);

        color next_throughput =
            throughput * srec.attenuation * (scattering_pdf / pdf_value);
        color incoming = advance(srec.event, scattered, depth - 1, next_throughput);
        color color_from_scatter =
            (srec.attenuation * scattering_pdf * incoming) / pdf_value;

        return color_from_emission + color_from_scatter;
    }

    color advance(path_event event, const ray& next_ray, int depth,
                  const color& next_throughput) {
        prefix.push_back(event);
        color incoming = trace(next_ray, depth, next_throughput);
        prefix.pop_back();
        return incoming;
    }

    void contribute(const color& weighted) {
        // Every terminal contribution goes through the same matcher, including exact
        // zeros (a no-op add), so a set of mutually exclusive expressions reconstructs
        // beauty with no thresholding error.  The weight already carries the full path
        // throughput and the mixture pdf factor.
        set.matching_mask(prefix, mask);
        for (std::size_t k = 0; k < channels.size(); ++k) {
            if (mask[k])
                channels[k] += weighted;
        }
    }
};


} // namespace


light_path_expression::light_path_expression(const std::string& name,
                                             const std::string& pattern)
  : expr_name(name), expr_pattern(pattern), start(-1), accept(-1) {
    lpe_parser parser(pattern, states);
    fragment frag = parser.parse_top();
    start = frag.start;
    accept = frag.accept;
}


bool light_path_expression::matches(const std::vector<path_event>& events) const {
    return nfa_matches(states, start, accept, events);
}


void light_path_set::add(const std::string& name, const std::string& pattern) {
    expressions.push_back(
        make_shared<light_path_expression>(name, pattern));
}


void light_path_set::matching_mask(const std::vector<path_event>& events,
                                   std::vector<unsigned char>& mask) const {
    mask.resize(expressions.size());
    for (std::size_t k = 0; k < expressions.size(); ++k)
        mask[k] = expressions[k]->matches(events) ? 1 : 0;
}


aov_ray_result trace_aov_ray(const ray& r, const hittable& world, const hittable& lights,
                             const color& background, int max_depth,
                             const light_path_set& set) {
    aov_ray_result result;
    aov_integrator integrator(world, lights, background, set, result.channels);
    result.beauty = integrator.trace(r, max_depth);
    return result;
}


aov_frame render_aov(camera& cam, const hittable& world, const hittable& lights,
                     const light_path_set& set, const std::string& frame_name) {
    aov_frame frame;
    frame.frame_name = frame_name;
    frame.image_width = cam.image_width;
    frame.samples_per_pixel = cam.samples_per_pixel;

    frame.channel_names.reserve(set.size());
    for (std::size_t k = 0; k < set.size(); ++k)
        frame.channel_names.push_back(set[k].name());

    cam.prepare();
    frame.image_height = cam.rendered_image_height();

    std::size_t pixel_count = frame.pixel_count();
    frame.beauty.assign(pixel_count, color(0, 0, 0));
    frame.channels.assign(set.size(), std::vector<color>(pixel_count, color(0, 0, 0)));

    cam.for_each_sample_ray([&](int i, int j, const ray& r) {
        std::size_t pixel = static_cast<std::size_t>(j) * frame.image_width + i;
        aov_ray_result result =
            trace_aov_ray(r, world, lights, cam.background, cam.max_depth, set);

        frame.beauty[pixel] += result.beauty;
        for (std::size_t k = 0; k < result.channels.size(); ++k)
            frame.channels[k][pixel] += result.channels[k];
    });

    // Store the same linear per-pixel averages camera::render writes colors from.
    double scale = cam.pixel_scale();
    for (std::size_t p = 0; p < pixel_count; ++p) {
        frame.beauty[p] *= scale;
        for (std::size_t k = 0; k < frame.channels.size(); ++k)
            frame.channels[k][p] *= scale;
    }

    return frame;
}


namespace {


int channel_index(const aov_frame& frame, const std::string& channel_name) {
    for (std::size_t k = 0; k < frame.channel_names.size(); ++k) {
        if (frame.channel_names[k] == channel_name)
            return static_cast<int>(k);
    }
    throw std::runtime_error("unknown AOV channel: " + channel_name);
}


void write_linear_buffer_ppm(std::ostream& out, int width, int height,
                             const std::vector<color>& linear) {
    out << "P3\n" << width << ' ' << height << "\n255\n";
    for (std::size_t p = 0; p < linear.size(); ++p)
        write_color(out, linear[p]);
}


} // namespace


void aov_frame::write_ppm(std::ostream& out, const std::string& channel_name) const {
    int k = channel_index(*this, channel_name);
    write_linear_buffer_ppm(out, image_width, image_height, channels[k]);
}


void aov_frame::write_beauty_ppm(std::ostream& out) const {
    write_linear_buffer_ppm(out, image_width, image_height, beauty);
}
