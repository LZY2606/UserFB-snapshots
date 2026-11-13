// Target cases for the light path expression AOV system. Implements the
// target_cases() entry point expected by support/main.cpp.
//
// The suite has two halves:
//   1. Pure expression tests that enumerate short event strings, checking the
//      match semantics, anchoring and ambiguity deduplication.
//   2. Integrator tests driven by scripted hittables/materials with fully
//      controlled hit and scatter sequences, plus a real camera scene that
//      writes PPM outputs and verifies linear reconstruction and RNG parity.

#include "test_api.h"

#include "rtweekend.h"
#include "camera.h"
#include "sphere.h"
#include "quad.h"
#include "hittable_list.h"
#include "material.h"
#include "pdf.h"
#include "light_path_expression.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>


namespace {


const double TEST_EPS = 1e-10;


bool near_color(const color& a, const color& b, double eps = TEST_EPS) {
    for (int i = 0; i < 3; ++i) {
        double d = std::fabs(a[i] - b[i]);
        if (d > eps * std::fmax(1.0, std::fmax(std::fabs(a[i]), std::fabs(b[i]))))
            return false;
    }
    return true;
}


// A pdf that always proposes one fixed direction with one fixed density.
class fixed_pdf : public pdf {
  public:
    fixed_pdf(const vec3& direction, double density)
      : dir(unit_vector(direction)), density(density) {}

    double value(const vec3& direction) const override { return density; }
    vec3 generate() const override { return dir; }

  private:
    vec3 dir;
    double density;
};


// A light stand-in whose pdf always proposes one fixed direction with one
// fixed density, so the mixture pdf stays fully deterministic.
class fixed_light_hittable : public hittable {
  public:
    fixed_light_hittable(const vec3& direction, double density)
      : dir(unit_vector(direction)), density(density) {}

    bool hit(const ray&, interval, hit_record&) const override { return false; }
    aabb bounding_box() const override { return bbox; }
    double pdf_value(const point3&, const vec3&) const override { return density; }
    vec3 random(const point3&) const override { return dir; }

  private:
    vec3 dir;
    double density;
    aabb bbox;
};


// Delta (skip_pdf) material following a scripted event label and direction.
// Optionally emits, so emission and the scattering continuation can be shown
// to match independently.
class scripted_delta_material : public material {
  public:
    scripted_delta_material(const color& attenuation, char event,
                            const vec3& direction,
                            const color& emission = color(0,0,0))
      : attenuation(attenuation), event(event),
        direction(unit_vector(direction)), emission(emission) {}

    color emitted(const ray&, const hit_record&, double, double,
                  const point3&) const override {
        return emission;
    }

    bool scatter(const ray& r_in, const hit_record& rec,
                 scatter_record& srec) const override {
        srec.attenuation = attenuation;
        srec.pdf_ptr = nullptr;
        srec.skip_pdf = true;
        srec.event = event;
        srec.skip_pdf_ray = ray(rec.p, direction, r_in.time());
        return true;
    }

  private:
    color attenuation;
    char event;
    vec3 direction;
    color emission;
};


// Non-skip material with a controlled pdf and controlled scattering_pdf.
class scripted_diffuse_material : public material {
  public:
    scripted_diffuse_material(const color& attenuation, char event,
                              const vec3& direction, double material_pdf,
                              double scatter_pdf)
      : attenuation(attenuation), event(event),
        direction(unit_vector(direction)),
        material_pdf(material_pdf), scatter_pdf(scatter_pdf) {}

    bool scatter(const ray& r_in, const hit_record& rec,
                 scatter_record& srec) const override {
        srec.attenuation = attenuation;
        srec.pdf_ptr = make_shared<fixed_pdf>(direction, material_pdf);
        srec.skip_pdf = false;
        srec.event = event;
        return true;
    }

    double scattering_pdf(const ray&, const hit_record&,
                          const ray&) const override {
        return scatter_pdf;
    }

  private:
    color attenuation;
    char event;
    vec3 direction;
    double material_pdf;
    double scatter_pdf;
};


// A material that both emits and continues with a non-skip_pdf scatter: its
// emission and the contributions reached after scattering must match
// independently, and the E event must not enter the continuation's prefix.
class scripted_emitting_diffuse_material : public material {
  public:
    scripted_emitting_diffuse_material(const color& attenuation,
                                       const color& emission,
                                       const vec3& direction, double material_pdf,
                                       double scatter_pdf)
      : attenuation(attenuation), emission(emission),
        direction(unit_vector(direction)),
        material_pdf(material_pdf), scatter_pdf(scatter_pdf) {}

    color emitted(const ray&, const hit_record&, double, double,
                  const point3&) const override {
        return emission;
    }

    bool scatter(const ray& r_in, const hit_record& rec,
                 scatter_record& srec) const override {
        srec.attenuation = attenuation;
        srec.pdf_ptr = make_shared<fixed_pdf>(direction, material_pdf);
        srec.skip_pdf = false;
        srec.event = 'D';
        return true;
    }

    double scattering_pdf(const ray&, const hit_record&,
                          const ray&) const override {
        return scatter_pdf;
    }

  private:
    color attenuation;
    color emission;
    vec3 direction;
    double material_pdf;
    double scatter_pdf;
};


// Surface that only emits and terminates the path.
class stop_emitter_material : public material {
  public:
    explicit stop_emitter_material(const color& emission) : emission(emission) {}

    color emitted(const ray&, const hit_record&, double, double,
                  const point3&) const override {
        return emission;
    }

    bool scatter(const ray&, const hit_record&, scatter_record&) const override {
        return false;
    }

  private:
    color emission;
};


// A custom material that never declares an event letter, like the pre-existing
// user materials; its scatter must default to the diffuse label.
class legacy_custom_material : public material {
  public:
    bool scatter(const ray& r_in, const hit_record& rec,
                 scatter_record& srec) const override {
        srec.attenuation = color(0.5, 0.5, 0.5);
        srec.pdf_ptr = make_shared<fixed_pdf>(vec3(0,0,-1), 1.0);
        srec.skip_pdf = false;
        // Deliberately does not touch srec.event.
        return true;
    }
};


int channel_index(const aov_frame& frame, const std::string& name) {
    for (std::size_t i = 0; i < frame.channels.size(); ++i)
        if (frame.channels[i].name == name)
            return int(i);
    return -1;
}


void require_parse_rejected(Suite& s, const std::string& expr,
                            std::size_t expected_pos) {
    bool threw = false;
    std::size_t reported = std::string::npos;
    try {
        light_path_expression compiled(expr);
    } catch (const lpe_parse_error& e) {
        threw = true;
        reported = e.position();
    }
    s.require(threw, ("expression should be rejected: " + expr).c_str());
    s.require(reported == expected_pos,
              ("parse error position mismatch for: " + expr).c_str());
}


// Enumerates every sequence C + (letters drawn from alphabet)^n + terminal for
// n in [0, max_middle], returning the full event strings.
std::vector<std::string> enumerate_sequences(const std::string& alphabet,
                                             int max_middle,
                                             char terminal) {
    std::vector<std::string> out;
    out.push_back(std::string("C") + terminal);
    std::vector<std::string> frontier;
    frontier.push_back("C");
    for (int n = 0; n < max_middle; ++n) {
        std::vector<std::string> next;
        for (std::size_t i = 0; i < frontier.size(); ++i) {
            for (std::size_t a = 0; a < alphabet.size(); ++a) {
                std::string seq = frontier[i] + alphabet[a];
                next.push_back(seq);
                out.push_back(seq + terminal);
            }
        }
        frontier = next;
    }
    return out;
}


} // namespace


void target_cases(Suite& s) {

    //--------------------------------------------------------------------------------------
    // Case 1: parser rejection and byte-accurate error positions.
    //--------------------------------------------------------------------------------------
    s.run("parser rejects malformed expressions with positions", [&]{
        require_parse_rejected(s, "", 0);
        require_parse_rejected(s, "CXE", 1);            // unknown letter
        require_parse_rejected(s, "C(DE", 1);           // unbalanced '('
        require_parse_rejected(s, "CDE)", 3);           // stray ')'
        require_parse_rejected(s, "*CDE", 0);           // quantifier, no operand
        require_parse_rejected(s, "C|*", 2);            // quantifier, no operand
        require_parse_rejected(s, "C|", 1);             // empty right branch
        require_parse_rejected(s, "|C", 0);             // empty left branch
        require_parse_rejected(s, "()", 0);             // empty group
        require_parse_rejected(s, "(C|)", 2);           // empty branch in group
        require_parse_rejected(s, "(*)", 1);            // quantifier, no operand
        require_parse_rejected(s, "CD|*E", 3);          // quantifier after branch

        // The two expressions named in the specification must compile.
        light_path_expression beauty("C(D|S|T|V)*(E|B)");
        light_path_expression direct_diffuse("CDE");
        s.require(beauty.matches("CE") && beauty.matches("CB")
                       && beauty.matches("CDSTVE") && !beauty.matches("CDEB"),
                  "beauty expression match table");
        s.require(direct_diffuse.matches("CDE")
                       && !direct_diffuse.matches("CDDE"),
                  "CDE exact match");
    });


    //--------------------------------------------------------------------------------------
    // Case 2: exhaustive enumeration of short event strings, anchoring and
    // ambiguity deduplication.
    //--------------------------------------------------------------------------------------
    s.run("enumerated event strings match expression semantics", [&]{
        const std::string alphabet = "DSTV";
        std::vector<std::string> seqs = enumerate_sequences(alphabet, 3, 'E');
        std::vector<std::string> bseqs = enumerate_sequences(alphabet, 3, 'B');

        light_path_expression beauty("C(D|S|T|V)*(E|B)");
        light_path_expression only_direct_diffuse("CDE");
        light_path_expression d_or_s("C(D|S)E");
        light_path_expression d_or_s_alt("CDE|CSE");
        light_path_expression optional_d("CD?E");
        light_path_expression star_d("CD*E");
        light_path_expression plus_d("CD+E");
        light_path_expression dot("C.E");
        light_path_expression double_star("CD*D*E");
        light_path_expression repeated_group("(CD)+E");
        light_path_expression substring("DE");      // must stay anchored
        light_path_expression just_c("C");

        for (std::size_t i = 0; i < seqs.size(); ++i) {
            const std::string& q = seqs[i];
            s.require(beauty.matches(q), "beauty collects every emission path");
            s.require(only_direct_diffuse.matches(q) == (q == "CDE"),
                      "CDE matches only the exact sequence");
            s.require(d_or_s.matches(q) == (q == "CDE" || q == "CSE"),
                      "C(D|S)E match table");
            s.require(d_or_s.matches(q) == d_or_s_alt.matches(q),
                      "group and alternation forms agree");
            s.require(optional_d.matches(q) == (q == "CE" || q == "CDE"),
                      "CD?E match table");
            bool star_expected = (q == "CE")
                || (q.find_first_not_of('D', 1) == q.size() - 1
                    && q[q.size()-1] == 'E' && q.size() >= 3);
            s.require(star_d.matches(q) == star_expected, "CD*E match table");
            s.require(double_star.matches(q) == star_d.matches(q),
                      "ambiguous CD*D*E behaves like CD*E");
            bool plus_expected = star_expected && q != "CE";
            s.require(plus_d.matches(q) == plus_expected, "CD+E match table");
            s.require(dot.matches(q) == (q.size() == 3),
                      "dot matches exactly one middle event");
            s.require(!substring.matches(q) && !just_c.matches(q),
                      "matching is anchored to the whole sequence");
        }
        for (std::size_t i = 0; i < bseqs.size(); ++i)
            s.require(beauty.matches(bseqs[i]),
                      "beauty collects background paths too");

        s.require(repeated_group.matches("CDE")
                       && repeated_group.matches("CDCDE")
                       && !repeated_group.matches("CDDE"),
                  "(CD)+E repetition table");
    });


    //--------------------------------------------------------------------------------------
    // Case 3: controlled integrator sequences — event labels, weighted
    // radiance, emission/continuation separation, background, truncation,
    // mixture-pdf weighting and material event defaults.
    //--------------------------------------------------------------------------------------
    s.run("integrator labels events and weights known radiances", [&]{
        std::srand(2024);

        // Chain along -z: S (emits a little, then reflects), T, pure emitter.
        const color att1(0.9, 0.8, 0.7);
        const color att2(0.6, 0.5, 0.4);
        const color emit2(0.2, 0.3, 0.4);
        const color emit_final(1.0, 2.0, 3.0);
        const vec3 down(0, 0, -1);

        hittable_list world;
        // Planes perpendicular to the ray, each crossed exactly once (a sphere
        // surface would be re-entered by the continuing ray).
        world.add(make_shared<quad>(
            point3(-2,-2,-1), vec3(4,0,0), vec3(0,4,0),
            make_shared<scripted_delta_material>(att1, 'S', down)));
        world.add(make_shared<quad>(
            point3(-2,-2,-2.5), vec3(4,0,0), vec3(0,4,0),
            make_shared<scripted_delta_material>(att2, 'T', down, emit2)));
        world.add(make_shared<quad>(
            point3(-2,-2,-4), vec3(4,0,0), vec3(0,4,0),
            make_shared<stop_emitter_material>(emit_final)));
        hittable_list lights;

        aov_frame frame;
        frame.add_channel("all",      "C(D|S|T|V)*(E|B)");
        frame.add_channel("cste",     "CSTE");
        frame.add_channel("cse",      "CSE");
        frame.add_channel("two_any",  "C..E");
        frame.add_channel("diffuse",  "CDE");
        frame.add_channel("bg",       "CB");
        // Two equivalent alternatives: even if the matcher saw both branches,
        // the contribution may only be accumulated once.
        frame.add_channel("amb_bg",   "C(B*|B*)B");

        aov_camera cam;
        cam.max_depth = 10;
        cam.background = color(0.13, 0.17, 0.19);

        std::vector<color> values;
        ray straight(point3(0,0,0), down);
        color beauty = cam.trace_ray(frame, straight, world, lights, values);

        color expected_mid_emission = att1 * emit2;
        color expected_final = att1 * att2 * emit_final;
        s.require(near_color(beauty, expected_mid_emission + expected_final),
                  "chain beauty equals the two weighted emissions");
        s.require(near_color(values[channel_index(frame, "cse")],
                             expected_mid_emission),
                  "mid-vertex emission matched on prefix CS, labeled E");
        s.require(near_color(values[channel_index(frame, "cste")],
                             expected_final),
                  "final emission matched through the S then T labels");
        s.require(near_color(values[channel_index(frame, "two_any")],
                             expected_final),
                  "C..E only sees the length-four path");
        s.require(near_color(values[channel_index(frame, "all")],
                             expected_mid_emission + expected_final),
                  "beauty channel gathers every emission");
        s.require(near_color(values[channel_index(frame, "diffuse")],
                             color(0,0,0)),
                  "no diffuse event on this chain");
        s.require(near_color(values[channel_index(frame, "bg")],
                             color(0,0,0)),
                  "no background event on this chain");

        // A ray that misses everything closes with B at full throughput.
        ray miss(point3(0,0,0), unit_vector(vec3(1,1,0)));
        std::vector<color> miss_values;
        color miss_beauty = cam.trace_ray(frame, miss, world, lights,
                                          miss_values);
        s.require(near_color(miss_beauty, cam.background),
                  "missed ray returns the background");
        s.require(near_color(miss_values[channel_index(frame, "bg")],
                             cam.background),
                  "background contribution labeled CB");
        s.require(near_color(miss_values[channel_index(frame, "amb_bg")],
                             cam.background),
                  "ambiguous alternation accumulates the background once");

        // Depth truncation: with one bounce budget the path cannot reach any
        // emitter, so every channel stays black.
        aov_camera shallow;
        shallow.max_depth = 1;
        shallow.background = cam.background;
        std::vector<color> shallow_values;
        color shallow_beauty =
            shallow.trace_ray(frame, straight, world, lights, shallow_values);
        s.require(near_color(shallow_beauty, color(0,0,0)),
                  "depth truncation yields no radiance");
        bool all_black = true;
        for (std::size_t i = 0; i < shallow_values.size(); ++i)
            if (!near_color(shallow_values[i], color(0,0,0))) all_black = false;
        s.require(all_black, "depth truncation writes no channel");

        // Non-skip branch: deterministic mixture pdf, known weighting.
        const color ad(0.8, 0.6, 0.4);
        const color e1(5.0, 1.0, 2.0);
        const double pm = 0.7, ps = 0.3, pl = 0.5;
        hittable_list diffuse_world;
        diffuse_world.add(make_shared<quad>(
            point3(-2,-2,-1), vec3(4,0,0), vec3(0,4,0),
            make_shared<scripted_diffuse_material>(ad, 'D', down, pm, ps)));
        diffuse_world.add(make_shared<quad>(
            point3(-2,-2,-2.5), vec3(4,0,0), vec3(0,4,0),
            make_shared<stop_emitter_material>(e1)));
        hittable_list light_list;
        light_list.add(make_shared<fixed_light_hittable>(down, pl));

        aov_frame diffuse_frame;
        diffuse_frame.add_channel("direct", "CDE");
        diffuse_frame.add_channel("wrong",  "CSE");
        aov_camera diffuse_cam;
        diffuse_cam.max_depth = 4;
        diffuse_cam.background = color(0,0,0);
        std::vector<color> diffuse_values;
        color diffuse_beauty = diffuse_cam.trace_ray(
            diffuse_frame, straight, diffuse_world, light_list,
            diffuse_values);
        double mixture = 0.5 * (pm + pl);
        color expected_diffuse = ad * (ps / mixture) * e1;
        s.require(near_color(diffuse_beauty, expected_diffuse),
                  "non-skip beauty uses the mixture pdf weighting");
        s.require(near_color(diffuse_values[channel_index(diffuse_frame,
                                                           "direct")],
                             expected_diffuse),
                  "non-skip path labeled CDE with weighted color");
        s.require(near_color(diffuse_values[channel_index(diffuse_frame,
                                                           "wrong")],
                             color(0,0,0)),
                  "non-skip path is not a specular label");

        // Event labels produced by the production materials.
        scatter_record srec;
        hit_record rec;
        rec.p = point3(0,0,1);
        rec.normal = vec3(0,0,1);
        rec.front_face = true;
        ray probe(point3(0,0,2), down);

        metal shiny(color(1,1,1), 0.0);
        s.require(shiny.scatter(probe, rec, srec) && srec.event == 'S',
                  "metal labels specular reflection");
        metal fuzzy(color(1,1,1), 0.4);
        s.require(fuzzy.scatter(probe, rec, srec) && srec.event == 'S',
                  "fuzzed metal still labels specular reflection");

        isotropic volume(color(1,1,1));
        s.require(volume.scatter(probe, rec, srec) && srec.event == 'V',
                  "isotropic phase function labels volume scatter");

        dielectric glass(1.5);
        bool labels_valid = true;
        for (int k = 0; k < 64; ++k) {
            if (!glass.scatter(probe, rec, srec)
                || (srec.event != 'S' && srec.event != 'T'))
                labels_valid = false;
        }
        s.require(labels_valid, "dielectric only produces S or T");

        // Guaranteed total internal reflection: grazing hit from inside.
        hit_record inside;
        inside.p = point3(0,0,0);
        inside.normal = vec3(0,0,1);
        inside.front_face = false;
        ray grazing(point3(0,0,0), vec3(1,0,0));
        bool always_s = true;
        for (int k = 0; k < 16; ++k) {
            scatter_record g;
            if (!glass.scatter(grazing, inside, g) || g.event != 'S')
                always_s = false;
        }
        s.require(always_s, "total internal reflection is always labeled S");

        legacy_custom_material legacy;
        scatter_record legacy_rec;
        s.require(legacy.scatter(probe, rec, legacy_rec)
                       && legacy_rec.event == 'D',
                  "custom material without an event defaults to D");

        // Emitting material that keeps scattering (non skip_pdf): the direct
        // emission closes as CE while the continued path closes as CDE, and E
        // must not leak into the continuation prefix.
        const color em0(0.7, 0.2, 0.1);
        const color ad2(0.8, 0.6, 0.4);
        const color e2(4.0, 3.0, 2.0);
        const double pm2 = 0.4, ps2 = 0.8, pl2 = 0.6;
        hittable_list emit_world;
        emit_world.add(make_shared<quad>(
            point3(-2,-2,-1), vec3(4,0,0), vec3(0,4,0),
            make_shared<scripted_emitting_diffuse_material>(
                ad2, em0, down, pm2, ps2)));
        emit_world.add(make_shared<quad>(
            point3(-2,-2,-2.5), vec3(4,0,0), vec3(0,4,0),
            make_shared<stop_emitter_material>(e2)));
        hittable_list emit_lights;
        emit_lights.add(make_shared<fixed_light_hittable>(down, pl2));

        aov_frame emit_frame;
        emit_frame.add_channel("direct_emit", "CE");
        emit_frame.add_channel("after_scatter", "CDE");
        // If E were wrongly appended to the continuation prefix, this path
        // would arrive as CEDE and light up.
        emit_frame.add_channel("polluted", "CEDE");
        aov_camera emit_cam;
        emit_cam.max_depth = 4;
        emit_cam.background = color(0,0,0);
        std::vector<color> emit_values;
        color emit_beauty = emit_cam.trace_ray(
            emit_frame, straight, emit_world, emit_lights, emit_values);
        double mixture2 = 0.5 * (pm2 + pl2);
        color expected_continuation = ad2 * (ps2 / mixture2) * e2;
        s.require(near_color(
                       emit_values[channel_index(emit_frame, "direct_emit")],
                       em0),
                  "emitting material's own radiance matched as CE");
        s.require(near_color(
                       emit_values[channel_index(emit_frame, "after_scatter")],
                       expected_continuation),
                  "continuation matched separately as CDE, weighted");
        s.require(near_color(
                       emit_values[channel_index(emit_frame, "polluted")],
                       color(0,0,0)),
                  "E never extends the prefix of the continued path");
        s.require(near_color(emit_beauty, em0 + expected_continuation),
                  "emitting material beauty keeps both terms");
    });


    //--------------------------------------------------------------------------------------
    // Case 4: real camera scene — PPM outputs, linear mutex reconstruction,
    // duplicate expression equality, independent storage and RNG parity with
    // the stock camera render.
    //--------------------------------------------------------------------------------------
    s.run("camera AOV frame reconstructs beauty and preserves RNG use", [&]{
        std::srand(99);

        hittable_list world, lights;

        // Diffuse floor.
        world.add(make_shared<quad>(
            point3(-20,-2,-20), vec3(40,0,0), vec3(0,0,40),
            make_shared<lambertian>(color(0.7, 0.6, 0.5))));

        // Glass sphere, diffuse sphere, fuzzed metal sphere.
        world.add(make_shared<sphere>(
            point3(0,0.5,-1), 0.6, make_shared<dielectric>(1.5)));
        world.add(make_shared<sphere>(
            point3(1.1,0.35,-0.8), 0.35,
            make_shared<lambertian>(color(0.9,0.3,0.3))));
        world.add(make_shared<sphere>(
            point3(-1.2,0.4,-0.6), 0.4,
            make_shared<metal>(color(0.8,0.8,0.9), 0.2)));

        // Area light.
        shared_ptr<material> area_mat =
            make_shared<diffuse_light>(color(6,5,4));
        shared_ptr<hittable> area =
            make_shared<quad>(point3(-1,2.2,-2), vec3(2,0,0),
                              vec3(0,0,1.5), area_mat);
        world.add(area);
        lights.add(area);

        const color bg(0.05, 0.08, 0.12);
        const int width = 12, spp = 4, depth = 6;

        aov_frame frame;
        frame.add_channel("beauty",   "C(D|S|T|V)*(E|B)");
        frame.add_channel("beauty2",  "C(D|S|T|V)*(E|B)");  // same expression
        frame.add_channel("emission", "C(D|S|T|V)*E");
        frame.add_channel("backgrnd", "C(D|S|T|V)*B");
        frame.add_channel("direct",   "CE");
        frame.add_channel("direct_bg","CB");
        frame.add_channel("diffuse",  "CDE");
        frame.add_channel("through_glass",
                          "C(D|S|T|V)*T(D|S|T|V)*(E|B)");

        aov_camera cam;
        cam.aspect_ratio = 1.0;
        cam.image_width = width;
        cam.samples_per_pixel = spp;
        cam.max_depth = depth;
        cam.background = bg;
        cam.vfov = 60;
        cam.lookfrom = point3(0,2,3);
        cam.lookat = point3(0,0.5,-1);
        cam.vup = vec3(0,1,0);

        cam.render_aov(frame, world, lights);

        s.require(frame.image_width == width
                       && frame.image_height == width
                       && frame.samples_per_pixel == spp,
                  "frame records dimensions and samples per pixel");
        s.require(!frame.channels.empty()
                       && frame.channels[0].name == "beauty",
                  "frame keeps channel names");

        // Linear mutex partition: every path terminates in E or B.
        int i_b1 = channel_index(frame, "beauty");
        int i_b2 = channel_index(frame, "beauty2");
        int i_emit = channel_index(frame, "emission");
        int i_bg = channel_index(frame, "backgrnd");
        int i_glass = channel_index(frame, "through_glass");
        std::size_t pixels = std::size_t(width) * width;
        color glass_total(0,0,0);
        for (std::size_t p = 0; p < pixels; ++p) {
            color rebuilt = frame.channels[i_emit].accum[p]
                          + frame.channels[i_bg].accum[p];
            s.require(near_color(rebuilt, cam.beauty()[p], 1e-9),
                      "E and B channels linearly reconstruct beauty");
            // The beauty expression itself must match every contribution.
            s.require(near_color(frame.channels[i_b1].accum[p],
                                 cam.beauty()[p], 1e-9),
                      "beauty channel equals the linear camera beauty");
            glass_total += frame.channels[i_glass].accum[p];
        }
        s.require(glass_total.x() + glass_total.y() + glass_total.z() > 0.0,
                  "real glass sphere produces transmission-labeled paths");

        // The same expression under two names yields identical numbers.
        for (std::size_t p = 0; p < pixels; ++p)
            s.require(near_color(frame.channels[i_b1].accum[p],
                                 frame.channels[i_b2].accum[p]),
                      "duplicate expression names share values");

        // Every channel value is a component of the beauty total.
        for (std::size_t c = 0; c < frame.channels.size(); ++c) {
            color channel_sum(0,0,0), beauty_sum(0,0,0);
            for (std::size_t p = 0; p < pixels; ++p) {
                channel_sum += frame.channels[c].accum[p];
                beauty_sum += cam.beauty()[p];
            }
            for (int k = 0; k < 3; ++k)
                s.require(channel_sum[k] <= beauty_sum[k] + 1e-9,
                          "channel contribution never exceeds beauty");
        }

        // PPM outputs (gamma is applied only while writing the display files).
        std::ofstream beauty_file(".build/aov_beauty.ppm");
        s.require(bool(beauty_file), "beauty PPM file opens");
        write_beauty_ppm(beauty_file, cam, frame);
        beauty_file.close();
        for (std::size_t c = 0; c < frame.channels.size(); ++c) {
            std::ostringstream path;
            path << ".build/aov_" << frame.channels[c].name << ".ppm";
            std::ofstream f(path.str());
            s.require(bool(f), "channel PPM file opens");
            write_channel_ppm(f, frame, c);
            f.close();
        }

        std::ifstream check(".build/aov_beauty.ppm");
        std::string magic;
        int w, h, maxv;
        check >> magic >> w >> h >> maxv;
        s.require(magic == "P3" && w == width && h == width && maxv == 255,
                  "beauty PPM header is well formed");
        int byte_count = 0, value;
        bool in_range = true;
        while (check >> value) {
            ++byte_count;
            if (value < 0 || value > 255) in_range = false;
        }
        s.require(byte_count == width * width * 3 && in_range,
                  "beauty PPM contains clamped byte channels");

        // Independent storage: a second interleaved render must not touch the
        // first frame's accumulators.
        std::vector<color> saved = frame.channels[i_b1].accum;
        aov_frame other_frame;
        other_frame.add_channel("beauty", "C(D|S|T|V)*(E|B)");
        aov_camera other_cam;
        other_cam.image_width = 6;
        other_cam.samples_per_pixel = 1;
        other_cam.max_depth = 3;
        other_cam.background = color(0.9, 0.1, 0.1);
        other_cam.vfov = cam.vfov;
        other_cam.lookfrom = cam.lookfrom;
        other_cam.lookat = cam.lookat;
        other_cam.vup = cam.vup;
        other_cam.render_aov(other_frame, world, lights);
        bool storage_untouched = saved.size() == frame.channels[i_b1].accum.size();
        for (std::size_t p = 0; storage_untouched && p < saved.size(); ++p)
            if (!near_color(saved[p], frame.channels[i_b1].accum[p]))
                storage_untouched = false;
        s.require(storage_untouched,
                  "each frame owns its channel storage");

        // RNG parity: the stock camera and the AOV camera walk the same
        // sequence of random numbers for identical configurations.
        auto snapshot = [](int n) {
            std::vector<int> values;
            for (int i = 0; i < n; ++i) values.push_back(std::rand());
            return values;
        };

        camera stock;
        stock.aspect_ratio = 1.0;
        stock.image_width = width;
        stock.samples_per_pixel = spp;
        stock.max_depth = depth;
        stock.background = bg;
        stock.vfov = 60;
        stock.lookfrom = cam.lookfrom;
        stock.lookat = cam.lookat;
        stock.vup = cam.vup;

        std::srand(99);
        std::ostringstream discarded;
        std::streambuf* old = std::cout.rdbuf(discarded.rdbuf());
        stock.render(world, lights);
        std::cout.rdbuf(old);
        std::vector<int> after_stock = snapshot(8);

        std::srand(99);
        aov_frame parity_frame;
        parity_frame.add_channel("beauty", "C(D|S|T|V)*(E|B)");
        parity_frame.add_channel("emission", "C(D|S|T|V)*E");
        aov_camera parity_cam = cam;
        parity_cam.render_aov(parity_frame, world, lights);
        std::vector<int> after_aov = snapshot(8);

        s.require(after_stock == after_aov,
                  "enabling AOV output leaves the next random draws unchanged");
    });
}
