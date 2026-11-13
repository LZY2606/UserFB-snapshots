// This TU links together with another TU that includes the book headers with
// their out-of-class definitions and the stb implementation; skip those here.
#define RTWEEKEND_NO_HEADER_DEFINITIONS

#include "light_path_expression.h"

#include <map>
#include <ostream>
#include <vector>


//==============================================================================================
// Expression parser
//==============================================================================================

bool light_path_expression::parser::valid_letter(char c) {
    return c == 'C' || c == 'D' || c == 'S' || c == 'T'
        || c == 'V' || c == 'E' || c == 'B';
}

light_path_expression::node
light_path_expression::parser::make_concat(node a, node b) {
    node out(NODE_CONCAT);
    if (a.type == NODE_EMPTY) return b;
    if (b.type == NODE_EMPTY) return a;
    if (a.type == NODE_CONCAT)
        out.children = a.children;
    else
        out.children.push_back(a);
    if (b.type == NODE_CONCAT)
        out.children.insert(out.children.end(), b.children.begin(), b.children.end());
    else
        out.children.push_back(b);
    return out;
}

light_path_expression::node
light_path_expression::parser::make_alt(node a, node b) {
    node out(NODE_ALT);
    if (a.type == NODE_ALT)
        out.children = a.children;
    else
        out.children.push_back(a);
    if (b.type == NODE_ALT)
        out.children.insert(out.children.end(), b.children.begin(), b.children.end());
    else
        out.children.push_back(b);
    return out;
}

light_path_expression::node
light_path_expression::parser::apply_quantifier(node base, char op) {
    if (op == '*') {
        node out(NODE_STAR);
        out.children.push_back(base);
        return out;
    }
    if (op == '?') {
        node empty(NODE_EMPTY);
        return make_alt(base, empty);
    }
    // '+' is concat(base, base*)
    node star(NODE_STAR);
    star.children.push_back(base);
    node copy = base;
    return make_concat(copy, star);
}

light_path_expression::node light_path_expression::parser::parse_all() {
    if (src.empty())
        throw lpe_parse_error("empty light path expression", 0);
    node root = parse_alternation();
    if (pos < src.size()) {
        if (src[pos] == ')')
            fail("unbalanced parenthesis");
        fail("unexpected character");
    }
    return root;
}

light_path_expression::node light_path_expression::parser::parse_alternation() {
    node left = parse_concatenation();
    while (pos < src.size() && src[pos] == '|') {
        std::size_t sep = pos;
        if (left.type == NODE_EMPTY)
            throw lpe_parse_error("empty alternative branch", sep);
        ++pos;
        node right = parse_concatenation();
        if (right.type == NODE_EMPTY)
            throw lpe_parse_error("empty alternative branch", sep);
        left = make_alt(left, right);
    }
    return left;
}

light_path_expression::node light_path_expression::parser::parse_concatenation() {
    node out(NODE_EMPTY);
    while (pos < src.size() && src[pos] != '|' && src[pos] != ')') {
        if (src[pos] == '*' || src[pos] == '+' || src[pos] == '?')
            fail("quantifier without an operand");
        node item = parse_primary();
        while (pos < src.size()
               && (src[pos] == '*' || src[pos] == '+' || src[pos] == '?')) {
            char op = src[pos];
            ++pos;
            item = apply_quantifier(item, op);
        }
        out = make_concat(out, item);
    }
    return out;
}

light_path_expression::node light_path_expression::parser::parse_primary() {
    if (pos >= src.size())
        fail("unexpected end of expression");
    char c = src[pos];
    if (c == '(') {
        std::size_t open = pos;
        ++pos;
        node group = parse_alternation();
        if (pos >= src.size() || src[pos] != ')')
            throw lpe_parse_error("unbalanced parenthesis", open);
        ++pos;
        if (group.type == NODE_EMPTY)
            throw lpe_parse_error("empty parenthesized group", open);
        return group;
    }
    if (c == ')')
        fail("unbalanced parenthesis");
    if (c == '.') {
        ++pos;
        return node(NODE_EVENT, 0);
    }
    if (!valid_letter(c))
        fail("unknown event letter");
    ++pos;
    return node(NODE_EVENT, c);
}


light_path_expression::light_path_expression(const std::string& source)
  : source_(source) {
    parser p(source);
    root_ = p.parse_all();
}


//==============================================================================================
// Matcher: NFA-style reachability with per-(node, offset) deduplication
//==============================================================================================

namespace {

struct memo_entry {
    enum state_t { UNSEEN, COMPUTING, DONE } state;
    std::vector<int> ends;
    memo_entry() : state(UNSEEN) {}
};

void add_offset(std::vector<int>& v, int off) {
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i] == off) return;
    v.push_back(off);
}

} // namespace

void light_path_expression::index_nodes(
        const node& n, std::vector<const node*>& out) {
    out.push_back(&n);
    for (std::size_t i = 0; i < n.children.size(); ++i)
        index_nodes(n.children[i], out);
}

bool light_path_expression::matcher::run() {
    std::vector<const node*> all;
    index_nodes(root, all);
    std::map<const node*, std::size_t> id;
    for (std::size_t i = 0; i < all.size(); ++i)
        id[all[i]] = i;

    const std::size_t len = events.size();
    std::vector<std::vector<memo_entry> > memo(
        all.size(), std::vector<memo_entry>(len + 1));
    const std::vector<int> empty_ends;

    // Returns the reachable end offsets after matching n starting at `start`.
    // Defined locally so it can close over the memo table.
    struct solver {
        std::vector<std::vector<memo_entry> >& memo;
        std::map<const node*, std::size_t>& id;
        const std::string& events;
        const std::vector<int> empty_ends;

        solver(std::vector<std::vector<memo_entry> >& m,
               std::map<const node*, std::size_t>& i,
               const std::string& e)
          : memo(m), id(i), events(e), empty_ends() {}

        const std::vector<int>& solve(const node& n, std::size_t start) {
            memo_entry& entry = memo[id[&n]][start];
            if (entry.state == memo_entry::DONE) return entry.ends;
            if (entry.state == memo_entry::COMPUTING) return empty_ends;
            entry.state = memo_entry::COMPUTING;

            switch (n.type) {
                case NODE_EMPTY:
                    add_offset(entry.ends, int(start));
                    break;

                case NODE_EVENT:
                    if (start < events.size()
                        && (n.letter == 0 || events[start] == n.letter))
                        add_offset(entry.ends, int(start + 1));
                    break;

                case NODE_CONCAT: {
                    std::vector<int> current;
                    add_offset(current, int(start));
                    for (std::size_t k = 0; k < n.children.size(); ++k) {
                        std::vector<int> next;
                        for (std::size_t a = 0; a < current.size(); ++a) {
                            const std::vector<int>& sub =
                                solve(n.children[k], current[a]);
                            for (std::size_t b = 0; b < sub.size(); ++b)
                                add_offset(next, sub[b]);
                        }
                        current = next;
                    }
                    entry.ends = current;
                    break;
                }

                case NODE_ALT:
                    for (std::size_t k = 0; k < n.children.size(); ++k) {
                        const std::vector<int>& sub =
                            solve(n.children[k], start);
                        for (std::size_t b = 0; b < sub.size(); ++b)
                            add_offset(entry.ends, sub[b]);
                    }
                    break;

                case NODE_PLUS:
                case NODE_OPTIONAL:
                    // '+' and '?' are compiled into concat/star/alt nodes;
                    // these enumerators never appear in a compiled tree.
                    break;

                case NODE_STAR: {
                    add_offset(entry.ends, int(start));
                    bool changed = true;
                    while (changed) {
                        changed = false;
                        std::vector<int> snapshot = entry.ends;
                        for (std::size_t a = 0; a < snapshot.size(); ++a) {
                            const std::vector<int>& sub =
                                solve(n.children[0], snapshot[a]);
                            for (std::size_t b = 0; b < sub.size(); ++b) {
                                std::size_t before = entry.ends.size();
                                add_offset(entry.ends, sub[b]);
                                if (entry.ends.size() != before)
                                    changed = true;
                            }
                        }
                    }
                    break;
                }
            }

            entry.state = memo_entry::DONE;
            return entry.ends;
        }
    };

    solver s(memo, id, events);
    const std::vector<int>& ends = s.solve(root, 0);
    for (std::size_t i = 0; i < ends.size(); ++i)
        if (std::size_t(ends[i]) == len) return true;
    return false;
}

bool light_path_expression::matches(const std::string& events) const {
    matcher m(root_, events);
    return m.run();
}


//==============================================================================================
// Frame storage
//==============================================================================================

void aov_frame::add_channel(const std::string& name,
                            const std::string& expression) {
    if (index_by_name.find(name) != index_by_name.end())
        throw std::runtime_error("duplicate AOV channel name: " + name);
    lpe_channel channel(name, expression);  // compiles, throws lpe_parse_error
    index_by_name[name] = channels.size();
    channels.push_back(std::move(channel));
}

const lpe_channel* aov_frame::find_channel(const std::string& name) const {
    std::map<std::string, std::size_t>::const_iterator it =
        index_by_name.find(name);
    if (it == index_by_name.end()) return nullptr;
    return &channels[it->second];
}

lpe_channel* aov_frame::find_channel(const std::string& name) {
    std::map<std::string, std::size_t>::iterator it =
        index_by_name.find(name);
    if (it == index_by_name.end()) return nullptr;
    return &channels[it->second];
}

color aov_frame::pixel(const lpe_channel& channel, int x, int y) const {
    if (image_width <= 0 || image_height <= 0)
        throw std::runtime_error("frame has not been rendered");
    if (x < 0 || x >= image_width || y < 0 || y >= image_height)
        throw std::runtime_error("pixel coordinate out of range");
    return channel.accum[std::size_t(y) * image_width + x];
}


//==============================================================================================
// AOV camera: geometry/sampling copied verbatim from camera
//==============================================================================================

void aov_camera::initialize() {
    image_height = int(image_width / aspect_ratio);
    image_height = (image_height < 1) ? 1 : image_height;

    sqrt_spp = int(std::sqrt(samples_per_pixel));
    pixel_samples_scale = 1.0 / (sqrt_spp * sqrt_spp);
    recip_sqrt_spp = 1.0 / sqrt_spp;

    center = lookfrom;

    auto theta = degrees_to_radians(vfov);
    auto h = std::tan(theta/2);
    auto viewport_height = 2 * h * focus_dist;
    auto viewport_width = viewport_height * (double(image_width)/image_height);

    w = unit_vector(lookfrom - lookat);
    u = unit_vector(cross(vup, w));
    v = cross(w, u);

    vec3 viewport_u = viewport_width * u;
    vec3 viewport_v = viewport_height * -v;

    pixel_delta_u = viewport_u / image_width;
    pixel_delta_v = viewport_v / image_height;

    auto viewport_upper_left = center - (focus_dist * w) - viewport_u/2 - viewport_v/2;
    pixel00_loc = viewport_upper_left + 0.5 * (pixel_delta_u + pixel_delta_v);

    auto defocus_radius = focus_dist * std::tan(degrees_to_radians(defocus_angle / 2));
    defocus_disk_u = u * defocus_radius;
    defocus_disk_v = v * defocus_radius;
}

ray aov_camera::get_ray(int i, int j, int s_i, int s_j) const {
    auto offset = sample_square_stratified(s_i, s_j);
    auto pixel_sample = pixel00_loc
                      + ((i + offset.x()) * pixel_delta_u)
                      + ((j + offset.y()) * pixel_delta_v);

    auto ray_origin = (defocus_angle <= 0) ? center : defocus_disk_sample();
    auto ray_direction = pixel_sample - ray_origin;
    auto ray_time = random_double();

    return ray(ray_origin, ray_direction, ray_time);
}

vec3 aov_camera::sample_square_stratified(int s_i, int s_j) const {
    auto px = ((s_i + random_double()) * recip_sqrt_spp) - 0.5;
    auto py = ((s_j + random_double()) * recip_sqrt_spp) - 0.5;

    return vec3(px, py, 0);
}

point3 aov_camera::defocus_disk_sample() const {
    auto p = random_in_unit_disk();
    return center + (p[0] * defocus_disk_u) + (p[1] * defocus_disk_v);
}

void aov_camera::record(aov_frame& frame, color* sink,
                        const std::string& prefix, char terminal,
                        const color& weighted) const {
    if (frame.channels.empty()) return;
    std::string full = prefix;
    full.push_back(terminal);
    for (std::size_t k = 0; k < frame.channels.size(); ++k) {
        if (frame.channels[k].expression.matches(full))
            sink[k] += weighted;
    }
}

color aov_camera::ray_color_lpe(const ray& r, int depth,
                                const hittable& world, const hittable& lights,
                                std::string prefix, aov_frame& frame,
                                color* sink, color throughput) const {
    // If we've exceeded the ray bounce limit, no more light is gathered.
    if (depth <= 0)
        return color(0,0,0);

    hit_record rec;

    // If the ray hits nothing, the background color closes the path with B.
    if (!world.hit(r, interval(0.001, infinity), rec)) {
        record(frame, sink, prefix, 'B', throughput * background);
        return background;
    }

    scatter_record srec;
    color color_from_emission = rec.mat->emitted(r, rec, rec.u, rec.v, rec.p);

    if (!rec.mat->scatter(r, rec, srec)) {
        record(frame, sink, prefix, 'E', throughput * color_from_emission);
        return color_from_emission;
    }

    // The emission closes one path with E; it must never extend the prefix of
    // the scattering continuation below.
    record(frame, sink, prefix, 'E', throughput * color_from_emission);

    std::string next_prefix = prefix;
    next_prefix.push_back(srec.event);

    if (srec.skip_pdf) {
        color child = ray_color_lpe(srec.skip_pdf_ray, depth-1, world, lights,
                                    next_prefix, frame, sink,
                                    throughput * srec.attenuation);
        return color_from_emission + srec.attenuation * child;
    }

    auto light_ptr = make_shared<hittable_pdf>(lights, rec.p);
    mixture_pdf p(light_ptr, srec.pdf_ptr);

    ray scattered = ray(rec.p, p.generate(), r.time());
    auto pdf_value = p.value(scattered.direction());

    double scattering_pdf = rec.mat->scattering_pdf(r, rec, scattered);

    color child_throughput =
        (throughput * srec.attenuation * scattering_pdf) / pdf_value;
    color sample_color = ray_color_lpe(scattered, depth-1, world, lights,
                                       next_prefix, frame, sink,
                                       child_throughput);
    color color_from_scatter =
        (srec.attenuation * scattering_pdf * sample_color) / pdf_value;

    return color_from_emission + color_from_scatter;
}

void aov_camera::render_aov(aov_frame& frame,
                            const hittable& world, const hittable& lights) {
    initialize();

    frame.image_width = image_width;
    frame.image_height = image_height;
    frame.samples_per_pixel = sqrt_spp * sqrt_spp;

    std::size_t pixel_count = std::size_t(image_width) * image_height;
    beauty_accum.assign(pixel_count, color(0,0,0));
    for (std::size_t k = 0; k < frame.channels.size(); ++k)
        frame.channels[k].accum.assign(pixel_count, color(0,0,0));

    std::vector<color> sink(frame.channels.size());

    for (int j = 0; j < image_height; j++) {
        for (int i = 0; i < image_width; i++) {
            std::size_t pixel_index = std::size_t(j) * image_width + i;
            color pixel_color(0,0,0);
            for (int s_j = 0; s_j < sqrt_spp; s_j++) {
                for (int s_i = 0; s_i < sqrt_spp; s_i++) {
                    ray r = get_ray(i, j, s_i, s_j);
                    for (std::size_t k = 0; k < sink.size(); ++k)
                        sink[k] = color(0,0,0);
                    color sample = ray_color_lpe(r, max_depth, world, lights,
                                                 "C", frame, sink.data(),
                                                 color(1,1,1));
                    pixel_color += sample;
                    for (std::size_t k = 0; k < frame.channels.size(); ++k)
                        frame.channels[k].accum[pixel_index] += sink[k];
                }
            }
            beauty_accum[pixel_index] = pixel_color;
        }
    }
}

color aov_camera::trace_ray(aov_frame& frame, const ray& r,
                            const hittable& world, const hittable& lights,
                            std::vector<color>& channel_values) const {
    channel_values.assign(frame.channels.size(), color(0,0,0));
    return ray_color_lpe(r, max_depth, world, lights, "C", frame,
                         channel_values.data(), color(1,1,1));
}


//==============================================================================================
// PPM display helpers (gamma is applied here, only at display time)
//==============================================================================================

namespace {

void write_ppm_header(std::ostream& out, int width, int height) {
    out << "P3\n" << width << ' ' << height << "\n255\n";
}

} // namespace

void write_channel_ppm(std::ostream& out, const aov_frame& frame,
                       std::size_t channel_index) {
    if (channel_index >= frame.channels.size())
        throw std::runtime_error("channel index out of range");
    const double scale = 1.0 / frame.samples_per_pixel;
    const lpe_channel& channel = frame.channels[channel_index];
    write_ppm_header(out, frame.image_width, frame.image_height);
    std::size_t count = std::size_t(frame.image_width) * frame.image_height;
    for (std::size_t i = 0; i < count; ++i)
        write_color(out, scale * channel.accum[i]);
}

void write_beauty_ppm(std::ostream& out, const aov_camera& cam,
                      const aov_frame& frame) {
    const double scale = 1.0 / frame.samples_per_pixel;
    write_ppm_header(out, frame.image_width, frame.image_height);
    const std::vector<color>& beauty = cam.beauty();
    for (std::size_t i = 0; i < beauty.size(); ++i)
        write_color(out, scale * beauty[i]);
}
