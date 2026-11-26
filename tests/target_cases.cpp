//==============================================================================================
// Target cases for the light path expression AOV renderer.
//
// Two layers are exercised:
//   1. The expression compiler/matcher itself, by enumerating short event sequences and
//      comparing against independent predicates, plus ambiguous-pattern de-duplication and
//      compile error byte offsets.
//   2. The production integrator driven by controllable hits and scripted materials, with
//      branch labels and fully weighted radiance checked term by term, followed by a real
//      camera scene (glass + diffuse + area light) rendered to PPM.
//
// To the extent possible under law, the author(s) have dedicated all copyright and related
// and neighboring rights to this software to the public domain worldwide. This software is
// distributed without any warranty.
//==============================================================================================

// The engine headers carry their out-of-line definitions inline for single-TU builds.
// In this multi-TU target the library TU (light_path_expression.cpp) owns those
// definitions; this TU takes the declarations only.
#define RTW_NO_HEADER_IMPLEMENTATION

#include "test_api.h"

#include "rtweekend.h"

#include "camera.h"
#include "constant_medium.h"
#include "hittable_list.h"
#include "light_path_expression.h"
#include "material.h"
#include "quad.h"
#include "sphere.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>


namespace {


bool close(const color& a, const color& b, double tol) {
    return std::fabs(a.x() - b.x()) < tol
        && std::fabs(a.y() - b.y()) < tol
        && std::fabs(a.z() - b.z()) < tol;
}


bool same_color(const color& a, const color& b) {
    return a.x() == b.x() && a.y() == b.y() && a.z() == b.z();
}


std::vector<path_event> events_of(const std::string& letters) {
    std::vector<path_event> events;
    for (char c : letters) {
        path_event event;
        if (!path_event_from_char(c, event))
            throw std::runtime_error("bad test event letter");
        events.push_back(event);
    }
    return events;
}


// A skip-pdf material whose scattered ray, attenuation, event label and emission are all
// scripted, so hit sequences can be driven without any random sampling.
class relay_material : public material {
  public:
    relay_material(const color& attenuation, const vec3& direction,
                   path_event event, bool assign_event,
                   const color& emission = color(0, 0, 0))
      : attenuation(attenuation), direction(direction), event(event),
        assign_event(assign_event), emission(emission) {}

    color emitted(const ray&, const hit_record&, double, double, const point3&)
    const override {
        return emission;
    }

    bool scatter(const ray& r_in, const hit_record& rec, scatter_record& srec)
    const override {
        srec.attenuation = attenuation;
        srec.pdf_ptr = nullptr;
        srec.skip_pdf = true;
        srec.skip_pdf_ray = ray(rec.p, direction, r_in.time());
        // Leave the default (D) in place when the test wants a material that does not
        // declare an event.
        if (assign_event)
            srec.event = event;
        return true;
    }

  private:
    color attenuation;
    vec3 direction;
    path_event event;
    bool assign_event;
    color emission;
};


shared_ptr<quad> make_plane(double z, shared_ptr<material> mat) {
    return make_shared<quad>(point3(-10, -10, z), vec3(20, 0, 0), vec3(0, 20, 0), mat);
}


// -------------------------------------------------------------------------------------------
// Case 1: independent enumeration of short event sequences.
// -------------------------------------------------------------------------------------------
void case_expression_enumeration(Suite& s) {
    s.run("lpe_short_event_enumeration", [&]() {
        light_path_expression exact("exact", "CDE");
        light_path_expression star("star", "C(D|S)*E");
        light_path_expression wild("wild", "C.*B");
        light_path_expression ambiguous("ambiguous", "C(D|D)E");
        light_path_expression beauty("beauty", "C(D|S|T|V)*(E|B)");

        const path_event letters[5] = { PATH_C, PATH_D, PATH_S, PATH_E, PATH_B };

        long checked = 0;
        for (int len = 1; len <= 3; ++len) {
            int total = 1;
            for (int k = 0; k < len; ++k) total *= 5;

            for (int code = 0; code < total; ++code) {
                std::vector<path_event> events;
                int value = code;
                for (int k = 0; k < len; ++k) {
                    events.push_back(letters[value % 5]);
                    value /= 5;
                }

                // Independent predicates describing the same languages.
                bool p_exact = (events == events_of("CDE"));
                bool p_star = events.size() >= 2 && events.front() == PATH_C
                           && events.back() == PATH_E;
                if (p_star) {
                    for (std::size_t k = 1; k + 1 < events.size(); ++k)
                        if (events[k] != PATH_D && events[k] != PATH_S) p_star = false;
                }
                bool p_wild = events.size() >= 2 && events.front() == PATH_C
                           && events.back() == PATH_B;
                bool p_amb = (events == events_of("CDE"));

                s.require(exact.matches(events) == p_exact, "CDE enumeration mismatch");
                s.require(star.matches(events) == p_star, "C(D|S)*E enumeration mismatch");
                s.require(wild.matches(events) == p_wild, "C.*B enumeration mismatch");
                s.require(ambiguous.matches(events) == p_amb,
                          "ambiguous C(D|D)E enumeration mismatch");
                ++checked;
            }
        }
        s.require(checked == 5 + 25 + 125, "enumeration coverage");

        // Quantifier and grouping semantics on hand-picked sequences.
        light_path_expression opt("opt", "CS?E");
        s.require(opt.matches(events_of("CE")), "CS?E must match CE");
        s.require(opt.matches(events_of("CSE")), "CS?E must match CSE");
        s.require(!opt.matches(events_of("CSSE")), "CS?E must not match CSSE");

        light_path_expression plus("plus", "CD+E");
        s.require(!plus.matches(events_of("CE")), "CD+E needs at least one D");
        s.require(plus.matches(events_of("CDDE")), "CD+E matches CDDE");

        light_path_expression group("group", "(CD|CS)E");
        s.require(group.matches(events_of("CSE")), "(CD|CS)E matches CSE");
        s.require(!group.matches(events_of("CTE")), "(CD|CS)E rejects CTE");

        // Full match only: a trailing or leading event must break every pattern.
        s.require(!beauty.matches(events_of("CDEB")), "no substring match (trailing)");
        s.require(!beauty.matches(events_of("DCE")), "no substring match (leading)");
        s.require(beauty.matches(events_of("CE")), "beauty covers zero-bounce emission");
        s.require(beauty.matches(events_of("CB")), "beauty covers direct background");
        s.require(beauty.matches(events_of("CSTVE")), "beauty covers a mixed bounce chain");
        s.require(!beauty.matches(events_of("CD")), "beauty needs an E/B terminal");
    });
}


// -------------------------------------------------------------------------------------------
// Case 2: compiler rejection with byte offsets.
// -------------------------------------------------------------------------------------------
void case_compile_errors(Suite& s) {
    s.run("lpe_compile_errors", [&]() {
        struct probe { const char* pattern; std::size_t offset; };
        const probe probes[] = {
            { "",        0 },
            { "X",       0 },
            { "CDXE",    2 },
            { "C D",     1 },
            { "C|",      1 },
            { "|CDE",    0 },
            { "(CDE|)",  4 },  // empty branch reported at the '|'
            { "CDE|",    3 },
            { "(CDE",    0 },
            { "CDE)",    3 },
            { "*CDE",    0 },
            { "C|*E",    2 },
            { "()",      0 },
        };

        for (const probe& p : probes) {
            bool threw = false;
            std::size_t got = 999;
            try {
                light_path_expression expr("bad", p.pattern);
            } catch (const lpe_compile_error& e) {
                threw = true;
                got = e.byte_offset;
            }
            s.require(threw, "pattern must be rejected");
            s.require(got == p.offset, "reported byte offset mismatch");
        }

        // Valid syntax compiles, and one compiled object is reused across many matches.
        light_path_expression reused("reused", "C(D|S|T|V)*(E|B)");
        for (int k = 0; k < 50; ++k)
            s.require(reused.matches(events_of("CDE")), "compiled expression reuse");
    });
}


// -------------------------------------------------------------------------------------------
// Case 3: integrator labels and fully weighted radiance on controlled paths.
// -------------------------------------------------------------------------------------------
void case_integrator_paths(Suite& s) {
    s.run("aov_integrator_labels_and_radiance", [&]() {
        // Built-in material event labels.
        {
            hit_record rec;
            rec.p = point3(0, 0, 0);
            rec.normal = vec3(0, 0, 1);
            rec.front_face = true;
            rec.u = rec.v = 0;
            ray r(point3(0, 0, 1), vec3(0, 0, -1));
            scatter_record sr;

            lambertian diff(color(0.5, 0.5, 0.5));
            s.require(diff.scatter(r, rec, sr) && sr.event == PATH_D, "lambertian is D");

            metal shiny(color(0.8, 0.8, 0.8), 0.0);
            s.require(shiny.scatter(r, rec, sr) && sr.event == PATH_S, "metal is S");
            metal fuzzy(color(0.8, 0.8, 0.8), 0.4);
            s.require(fuzzy.scatter(r, rec, sr) && sr.event == PATH_S,
                      "fuzzy metal is still S");

            isotropic vol(color(0.5, 0.5, 0.5));
            s.require(vol.scatter(r, rec, sr) && sr.event == PATH_V && !sr.skip_pdf,
                      "isotropic volume is V");

            dielectric glass(1.5);
            // Grazing inside hit: total internal reflection can only be S.
            hit_record inside = rec;
            inside.front_face = false;
            ray grazing(point3(0, 0, 0), unit_vector(vec3(1, 0, 0.1)));
            s.require(glass.scatter(grazing, inside, sr) && sr.event == PATH_S,
                      "total internal reflection is S");

            // Near-normal front hits choose S or T from the actual draw, never by material.
            bool saw_t = false, saw_only_st = true;
            for (int k = 0; k < 200; ++k) {
                glass.scatter(r, rec, sr);
                if (sr.event != PATH_S && sr.event != PATH_T) saw_only_st = false;
                if (sr.event == PATH_T) saw_t = true;
            }
            s.require(saw_only_st, "dielectric labels only S or T");
            s.require(saw_t, "dielectric refraction is labelled T");
        }

        // Scripted two-bounce path C -S- T- E.
        const color a1(0.5, 0.6, 0.7);
        const color a2(0.9, 0.8, 0.7);
        const color lamp(2, 3, 4);
        const color expected = a1 * a2 * lamp;

        hittable_list world, lights;
        world.add(make_plane(-1,
            make_shared<relay_material>(a1, vec3(0, 0, -1), PATH_S, true)));
        world.add(make_plane(-2,
            make_shared<relay_material>(a2, vec3(0, 0, -1), PATH_T, true)));
        world.add(make_plane(-3, make_shared<diffuse_light>(lamp)));

        light_path_set set;
        set.add("beauty",   "C(D|S|T|V)*(E|B)");
        set.add("ste",      "CSTE");
        set.add("de",       "CDE");
        set.add("direct",   "CE");
        set.add("polluted", "CESE");
        set.add("amb",      "CSTE|CSTE");   // equivalent branch must not double count

        ray cam_ray(point3(0, 0, 0), vec3(0, 0, -1));
        aov_ray_result result =
            trace_aov_ray(cam_ray, world, lights, color(0.3, 0.4, 0.5), 8, set);

        s.require(close(result.beauty, expected, 1e-12), "known skip-pdf radiance");
        s.require(close(result.channels[0], expected, 1e-12), "beauty channel weighted");
        s.require(close(result.channels[1], expected, 1e-12), "CSTE channel weighted");
        s.require(close(result.channels[5], expected, 1e-12),
                  "ambiguous match accumulated once");
        s.require(result.channels[2].near_zero(), "CDE must not collect an S,T path");
        s.require(result.channels[3].near_zero(), "no direct emission on this path");
        s.require(result.channels[4].near_zero(), "E must not pollute the bounce prefix");

        // A custom material that never assigns an event keeps the default D label.
        hittable_list default_world;
        default_world.add(make_plane(-1,
            make_shared<relay_material>(a1, vec3(0, 0, -1), PATH_S, false)));
        default_world.add(make_plane(-2, make_shared<diffuse_light>(lamp)));
        light_path_set d_set;
        d_set.add("de", "CDE");
        d_set.add("ste", "CSTE");
        aov_ray_result d_result =
            trace_aov_ray(cam_ray, default_world, lights, color(0, 0, 0), 8, d_set);
        s.require(close(d_result.channels[0], a1 * lamp, 1e-12),
                  "undeclared scatter event defaults to D");
        s.require(d_result.channels[1].near_zero(), "default D is not an S event");

        // Emission and scattering on the same surface are matched separately.
        const color own_glow(0.2, 0.3, 0.4);
        hittable_list glow_world;
        glow_world.add(make_plane(-1,
            make_shared<relay_material>(a1, vec3(0, 0, -1), PATH_S, true, own_glow)));
        glow_world.add(make_plane(-2, make_shared<diffuse_light>(lamp)));
        light_path_set glow_set;
        glow_set.add("direct", "CE");
        glow_set.add("ste", "CSE");
        glow_set.add("beauty", "C(D|S|T|V)*(E|B)");
        aov_ray_result glow =
            trace_aov_ray(cam_ray, glow_world, lights, color(0, 0, 0), 8, glow_set);
        s.require(close(glow.channels[0], own_glow, 1e-12),
                  "surface emission matched with a temporary E");
        s.require(close(glow.channels[1], a1 * lamp, 1e-12),
                  "continuing path keeps its prefix free of the temporary E");
        s.require(close(glow.channels[2], own_glow + a1 * lamp, 1e-12),
                  "emission and downstream contribution both land in beauty");

        // Background: direct miss and background reached after a specular event.
        hittable_list bg_world;
        bg_world.add(make_plane(-1,
            make_shared<relay_material>(a1, vec3(0, 0, 1), PATH_S, true)));
        light_path_set bg_set;
        bg_set.add("direct_bg", "CB");
        bg_set.add("spec_bg", "C(S|T)+B");
        const color bg(0.3, 0.4, 0.5);
        ray miss_ray(point3(0, 0, 0), vec3(0, 0, 1));
        aov_ray_result direct_miss =
            trace_aov_ray(miss_ray, bg_world, lights, bg, 8, bg_set);
        s.require(close(direct_miss.beauty, bg, 1e-12), "miss returns background");
        s.require(close(direct_miss.channels[0], bg, 1e-12), "CB collects direct miss");
        s.require(direct_miss.channels[1].near_zero(), "direct miss is not a specular path");

        aov_ray_result spec_miss =
            trace_aov_ray(cam_ray, bg_world, lights, bg, 8, bg_set);
        s.require(close(spec_miss.channels[1], a1 * bg, 1e-12),
                  "background seen through specular carries throughput");
        s.require(spec_miss.channels[0].near_zero(), "specular miss is not direct CB");

        // Depth truncation and zero contributions gather nothing.
        aov_ray_result truncated =
            trace_aov_ray(cam_ray, world, lights, bg, 1, set);
        s.require(truncated.beauty.near_zero(), "depth truncation returns no light");
        for (const color& c : truncated.channels)
            s.require(c.near_zero(), "depth truncation writes no channel");

        hittable_list black_world;
        black_world.add(make_plane(-1, make_shared<diffuse_light>(color(0, 0, 0))));
        aov_ray_result black =
            trace_aov_ray(cam_ray, black_world, lights, color(0, 0, 0), 8, set);
        s.require(black.beauty.near_zero(), "zero emission is invisible");
    });
}


// -------------------------------------------------------------------------------------------
// Case 4: full camera frame, PPM output, partition reconstruction and RNG equivalence.
// -------------------------------------------------------------------------------------------
void case_full_frame(Suite& s) {
    s.run("aov_full_frame_scene", [&]() {
        hittable_list world, lights;

        auto ground = make_shared<lambertian>(color(0.5, 0.55, 0.5));
        auto diff = make_shared<lambertian>(color(0.7, 0.2, 0.2));
        auto glass = make_shared<dielectric>(1.5);
        auto area = make_shared<diffuse_light>(color(18, 18, 18));

        world.add(make_shared<sphere>(point3(0, -100.5, -1), 100, ground));
        world.add(make_shared<sphere>(point3(-0.7, 0, -1), 0.5, glass));
        world.add(make_shared<sphere>(point3(0.9, -0.1, -1.3), 0.4, diff));
        auto light_quad =
            make_shared<quad>(point3(-0.7, 1.4, -1.7), vec3(1.4, 0, 0),
                              vec3(0, 0, 1.1), area);
        world.add(light_quad);

        auto empty_material = shared_ptr<material>();
        lights.add(make_shared<quad>(point3(-0.7, 1.4, -1.7), vec3(1.4, 0, 0),
                                     vec3(0, 0, 1.1), empty_material));
        lights.add(make_shared<sphere>(point3(-0.7, 0, -1), 0.5, empty_material));

        light_path_set set;
        set.add("beauty",      "C(D|S|T|V)*(E|B)");
        set.add("beauty_alias","C(D|S|T|V)*(E|B)");  // same expression, second name
        set.add("direct_emit", "CE");
        set.add("direct_bg",   "CB");
        set.add("bounce_emit", "C(D|S|T|V)+E");
        set.add("bounce_bg",   "C(D|S|T|V)+B");
        set.add("spec_bg",     "C(S|T)+B");

        camera cam;
        cam.image_width = 12;
        cam.aspect_ratio = 1.0;
        cam.samples_per_pixel = 4;
        cam.max_depth = 6;
        cam.background = color(0.3, 0.4, 0.5);
        cam.vfov = 60;
        cam.lookfrom = point3(0, 0.5, 1);
        cam.lookat = point3(0, 0, -1);
        cam.vup = vec3(0, 1, 0);

        std::srand(2026);
        aov_frame frame = render_aov(cam, world, lights, set, "glass_diffuse_light");

        // Frame metadata.
        s.require(frame.image_width == 12 && frame.image_height == 12, "frame dimensions");
        s.require(frame.samples_per_pixel == 4, "frame stores samples per pixel");
        s.require(frame.channel_names.size() == set.size(), "frame stores channel names");
        s.require(frame.channel_names[3] == "direct_bg", "channel name preserved");
        s.require(frame.channels.size() == set.size(), "one storage buffer per channel");
        s.require(frame.beauty.size() == frame.pixel_count(), "beauty storage owned");

        // The same expression under two names yields identical values.
        for (std::size_t p = 0; p < frame.pixel_count(); ++p)
            s.require(same_color(frame.channels[0][p], frame.channels[1][p]),
                      "aliased expressions are numerically identical");

        // The four partition channels are mutually exclusive and reconstruct beauty in
        // linear space (gamma never enters the sum).
        double worst = 0.0;
        for (std::size_t p = 0; p < frame.pixel_count(); ++p) {
            color partition = frame.channels[2][p] + frame.channels[3][p]
                            + frame.channels[4][p] + frame.channels[5][p];
            worst = std::fmax(worst, (partition - frame.beauty[p]).length());
        }
        s.require(worst < 1e-9, "exclusive channels reconstruct linear beauty");

        // Linear HDR storage: raw values may exceed 1 while PPM bytes cannot.
        bool saw_hdr = false;
        for (const color& c : frame.beauty)
            if (c.x() > 1.0 || c.y() > 1.0 || c.z() > 1.0) saw_hdr = true;
        s.require(saw_hdr, "channels keep linear HDR values");

        // PPM output applies gamma only at write time.
        {
            std::ofstream beauty_out(".build/aov_beauty.ppm");
            frame.write_beauty_ppm(beauty_out);
            beauty_out.close();
            std::ofstream bg_out(".build/aov_spec_bg.ppm");
            frame.write_ppm(bg_out, "spec_bg");
            bg_out.close();

            std::ifstream in(".build/aov_beauty.ppm");
            std::string magic;
            int w, h, maxv;
            in >> magic >> w >> h >> maxv;
            s.require(magic == "P3" && w == 12 && h == 12 && maxv == 255,
                      "beauty PPM header");
            int byte_count = 0, value;
            while (in >> value) {
                s.require(value >= 0 && value <= 255, "ppm byte range");
                ++byte_count;
            }
            s.require(byte_count == 12 * 12 * 3, "ppm byte count");
        }

        // Single-ray diagnostic on the real scene: the same partition holds per ray.
        cam.prepare();
        ray one = ray(cam.lookfrom,
                      unit_vector(cam.lookat - cam.lookfrom), 0.0);
        light_path_set partition_set;
        partition_set.add("direct_emit", "CE");
        partition_set.add("direct_bg",   "CB");
        partition_set.add("bounce_emit", "C(D|S|T|V)+E");
        partition_set.add("bounce_bg",   "C(D|S|T|V)+B");
        aov_ray_result one_result =
            trace_aov_ray(one, world, lights, cam.background, cam.max_depth,
                          partition_set);
        color ray_sum(0, 0, 0);
        for (const color& c : one_result.channels) ray_sum += c;
        s.require(close(ray_sum, one_result.beauty, 1e-10),
                  "single-ray partition reconstructs beauty");

        // Enabling AOV output must leave the random number stream untouched.
        std::srand(4242);
        aov_frame seeded = render_aov(cam, world, lights, set, "rng_check");
        int aov_draws[6];
        for (int& v : aov_draws) v = std::rand();

        std::srand(4242);
        std::ostringstream sink;
        std::streambuf* old = std::cout.rdbuf(sink.rdbuf());
        cam.render(world, lights);
        std::cout.rdbuf(old);
        int plain_draws[6];
        for (int& v : plain_draws) v = std::rand();
        for (int k = 0; k < 6; ++k)
            s.require(aov_draws[k] == plain_draws[k],
                      "AOV tracing consumes the same random numbers as render");

        // End-to-end: with the same seed the AOV beauty PPM must be byte-for-byte the
        // production render's PPM (same paths, same arithmetic, gamma only at write time).
        std::srand(77);
        aov_frame ref_frame = render_aov(cam, world, lights, set, "ppm_ref");
        std::ostringstream aov_ppm;
        ref_frame.write_beauty_ppm(aov_ppm);

        std::srand(77);
        std::ostringstream plain_ppm;
        old = std::cout.rdbuf(plain_ppm.rdbuf());
        cam.render(world, lights);
        std::cout.rdbuf(old);
        s.require(aov_ppm.str() == plain_ppm.str(),
                  "AOV beauty PPM matches the production camera render exactly");

        // Frames own their storage: rendering a second camera cannot mutate the first.
        color checksum_before(0, 0, 0);
        for (const color& c : seeded.beauty) checksum_before += c;

        camera other;
        other.image_width = 6;
        other.aspect_ratio = 1.0;
        other.samples_per_pixel = 1;
        other.max_depth = 3;
        other.background = color(0.1, 0.2, 0.3);
        other.vfov = 90;
        light_path_set small_set;
        small_set.add("beauty", "C(D|S|T|V)*(E|B)");
        aov_frame other_frame = render_aov(other, world, lights, small_set, "other");
        s.require(other_frame.image_width == 6 && other_frame.channels.size() == 1,
                  "second camera keeps its own frame storage");

        color checksum_after(0, 0, 0);
        for (const color& c : seeded.beauty) checksum_after += c;
        s.require(same_color(checksum_before, checksum_after),
                  "interleaved cameras do not share channel state");
    });
}


} // namespace


void target_cases(Suite& suite) {
    case_expression_enumeration(suite);
    case_compile_errors(suite);
    case_integrator_paths(suite);
    case_full_frame(suite);
}
