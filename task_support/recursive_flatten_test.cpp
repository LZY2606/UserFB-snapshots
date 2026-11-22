// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

// Tests for the recursive stack flattener.  These exercise the real
// production entry point opentimelineio::flatten_stack_recursive.

#include "check.h"

#include <opentimelineio/clip.h>
#include <opentimelineio/effect.h>
#include <opentimelineio/externalReference.h>
#include <opentimelineio/gap.h>
#include <opentimelineio/linearTimeWarp.h>
#include <opentimelineio/marker.h>
#include <opentimelineio/stack.h>
#include <opentimelineio/stackAlgorithm.h>
#include <opentimelineio/track.h>
#include <opentimelineio/trackAlgorithm.h>
#include <opentimelineio/transition.h>

#include <cmath>
#include <string>
#include <vector>

namespace o = opentimelineio::OPENTIMELINEIO_VERSION_NS;
using o::RationalTime;
using o::TimeRange;

namespace {

constexpr double TEST_EPSILON = 1.0e-9;

bool
time_eq(RationalTime a, RationalTime b)
{
    return std::fabs(a.to_seconds() - b.to_seconds()) < TEST_EPSILON;
}

bool
range_eq(std::optional<TimeRange> got, RationalTime start, RationalTime dur)
{
    return got.has_value() && time_eq(got->start_time(), start)
           && time_eq(got->duration(), dur);
}

TimeRange
r(double start, double dur, double rate)
{
    return TimeRange(RationalTime(start, rate), RationalTime(dur, rate));
}

o::Clip*
make_clip(
    std::string const& name,
    double             start,
    double             dur,
    double             rate,
    std::string const& url = std::string())
{
    o::MediaReference* media = nullptr;
    if (!url.empty())
    {
        media = new o::ExternalReference(
            url, r(0.0, 100000.0, rate));
    }
    return new o::Clip(name, media, r(start, dur, rate));
}

o::Gap*
make_gap(double dur, double rate)
{
    return new o::Gap(RationalTime(dur, rate));
}

// The canonical example from the task description.
o::Stack*
build_canonical(bool outer_trim, double rate = 24.0)
{
    auto scene = new o::Stack("scene");

    auto bottom = new o::Track("bottom");
    bottom->append_child(make_clip("B", 100.0, 20.0, rate, "file://B"));

    auto nest = new o::Stack("nested", r(2.0, 12.0, rate));
    auto inside = new o::Track("inside");
    inside->append_child(make_gap(4.0, rate));
    inside->append_child(make_clip("A", 200.0, 8.0, rate, "file://A"));
    inside->append_child(make_gap(8.0, rate));
    nest->append_child(inside);

    auto top = new o::Track("top");
    top->append_child(nest);

    scene->append_child(bottom);
    scene->append_child(top);

    if (outer_trim)
    {
        scene->set_source_range(r(1.0, 10.0, rate));
    }
    return scene;
}

} // namespace

// ---------------------------------------------------------------------------
// visibility group
// ---------------------------------------------------------------------------

static int
test_canonical_untrimmed()
{
    o::ErrorStatus e;
    o::SerializableObject::Retainer<o::Stack> scene(build_canonical(false));
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &e));
    TASK_CHECK(flat && !o::is_error(e));
    TASK_CHECK(time_eq(flat->duration(&e), RationalTime(20, 24)));
    TASK_CHECK(flat->children().size() == 3);

    auto c0 = dynamic_cast<o::Clip*>(flat->children()[0].value);
    auto c1 = dynamic_cast<o::Clip*>(flat->children()[1].value);
    auto c2 = dynamic_cast<o::Clip*>(flat->children()[2].value);
    TASK_CHECK(c0 && c1 && c2);
    TASK_CHECK(c0->name() == "B");
    TASK_CHECK(c1->name() == "A");
    TASK_CHECK(c2->name() == "B");
    TASK_CHECK(range_eq(c0->source_range(), RationalTime(100, 24),
                        RationalTime(2, 24)));
    TASK_CHECK(range_eq(c1->source_range(), RationalTime(200, 24),
                        RationalTime(8, 24)));
    TASK_CHECK(range_eq(c2->source_range(), RationalTime(110, 24),
                        RationalTime(10, 24)));
    return 0;
}

static int
test_canonical_outer_trim()
{
    o::ErrorStatus e;
    o::SerializableObject::Retainer<o::Stack> scene(build_canonical(true));
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &e));
    TASK_CHECK(flat && !o::is_error(e));
    TASK_CHECK(time_eq(flat->duration(&e), RationalTime(10, 24)));
    TASK_CHECK(flat->children().size() == 3);

    auto c0 = dynamic_cast<o::Clip*>(flat->children()[0].value);
    auto c1 = dynamic_cast<o::Clip*>(flat->children()[1].value);
    auto c2 = dynamic_cast<o::Clip*>(flat->children()[2].value);
    TASK_CHECK(c0 && c1 && c2);
    TASK_CHECK(c0->name() == "B" && c1->name() == "A" && c2->name() == "B");
    // B must start at media 101, and A's source start must not be pulled
    // forward by the outer trim.
    TASK_CHECK(range_eq(c0->source_range(), RationalTime(101, 24),
                        RationalTime(1, 24)));
    TASK_CHECK(range_eq(c1->source_range(), RationalTime(200, 24),
                        RationalTime(8, 24)));
    TASK_CHECK(range_eq(c2->source_range(), RationalTime(110, 24),
                        RationalTime(1, 24)));
    return 0;
}

static int
test_alternating_nesting()
{
    // Stack -> Track -> Stack -> Track, several layers deep, with gaps.
    o::ErrorStatus e;
    auto scene = new o::Stack("scene");
    auto low = new o::Track("low");
    low->append_child(make_clip("L", 0.0, 30.0, 24.0));

    auto mid_stack = new o::Stack("mid");
    auto mid_track = new o::Track("mid_track");
    mid_track->append_child(make_gap(5.0, 24.0));
    mid_track->append_child(make_clip("M", 50.0, 10.0, 24.0));
    mid_track->append_child(make_gap(15.0, 24.0));
    mid_stack->append_child(mid_track);

    auto high_track = new o::Track("high");
    auto high_stack = new o::Stack("high_stack");
    auto high_inner = new o::Track("high_inner");
    high_inner->append_child(make_gap(8.0, 24.0));
    high_inner->append_child(make_clip("H", 90.0, 4.0, 24.0));
    high_inner->append_child(make_gap(18.0, 24.0));
    high_stack->append_child(high_inner);
    high_track->append_child(high_stack);

    scene->append_child(low);
    scene->append_child(mid_stack);
    scene->append_child(high_track);

    o::SerializableObject::Retainer<o::Stack> scene_hold(scene);
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &e));
    TASK_CHECK(flat && !o::is_error(e));
    TASK_CHECK(time_eq(flat->duration(&e), RationalTime(30, 24)));

    // Expected coverage:
    // [0,5)   L   [5,8) M   [8,12) H   [12,15) M   [15,30) L
    TASK_CHECK(flat->children().size() == 5);
    auto names = std::vector<std::string>{ "L", "M", "H", "M", "L" };
    for (size_t i = 0; i < names.size(); ++i)
    {
        auto clip = dynamic_cast<o::Clip*>(flat->children()[i].value);
        TASK_CHECK(clip && clip->name() == names[i]);
    }
    auto m0 = dynamic_cast<o::Clip*>(flat->children()[1].value);
    TASK_CHECK(range_eq(m0->source_range(), RationalTime(50, 24),
                        RationalTime(3, 24)));
    auto m1 = dynamic_cast<o::Clip*>(flat->children()[3].value);
    TASK_CHECK(range_eq(m1->source_range(), RationalTime(57, 24),
                        RationalTime(3, 24)));
    auto h = dynamic_cast<o::Clip*>(flat->children()[2].value);
    TASK_CHECK(range_eq(h->source_range(), RationalTime(90, 24),
                        RationalTime(4, 24)));
    return 0;
}

static int
test_disabled_branch_reveals_lower()
{
    o::ErrorStatus e;
    auto scene = new o::Stack("scene");
    auto low = new o::Track("low");
    low->append_child(make_clip("L", 0.0, 10.0, 24.0));

    auto high = new o::Track("high");
    auto off_clip = make_clip("OFF", 100.0, 10.0, 24.0);
    off_clip->set_enabled(false);
    high->append_child(off_clip);

    scene->append_child(low);
    scene->append_child(high);

    o::SerializableObject::Retainer<o::Stack> scene_hold(scene);
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &e));
    TASK_CHECK(flat && !o::is_error(e));
    TASK_CHECK(flat->children().size() == 1);
    auto only = dynamic_cast<o::Clip*>(flat->children()[0].value);
    TASK_CHECK(only && only->name() == "L");
    TASK_CHECK(range_eq(only->source_range(), RationalTime(0, 24),
                        RationalTime(10, 24)));
    return 0;
}

static int
test_fully_transparent_and_empty()
{
    o::ErrorStatus e;

    // Fully transparent: top clip disabled, bottom track only holds a gap.
    auto scene = new o::Stack("scene");
    auto low = new o::Track("low");
    low->append_child(make_gap(12.0, 24.0));
    auto high = new o::Track("high");
    auto off = make_clip("OFF", 0.0, 12.0, 24.0);
    off->set_enabled(false);
    high->append_child(off);
    scene->append_child(low);
    scene->append_child(high);

    o::SerializableObject::Retainer<o::Stack> scene_hold(scene);
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &e));
    TASK_CHECK(flat && !o::is_error(e));
    TASK_CHECK(time_eq(flat->duration(&e), RationalTime(12, 24)));
    TASK_CHECK(flat->children().size() == 1);
    TASK_CHECK(dynamic_cast<o::Gap*>(flat->children()[0].value) != nullptr);

    // Empty stack without a trim: legal, returns an empty track.
    o::SerializableObject::Retainer<o::Stack> empty(new o::Stack("empty"));
    o::SerializableObject::Retainer<o::Track> flat_empty(
        o::flatten_stack_recursive(empty, &e));
    TASK_CHECK(flat_empty && !o::is_error(e));
    TASK_CHECK(time_eq(flat_empty->duration(&e), RationalTime(0, 1)));
    TASK_CHECK(flat_empty->children().empty());

    // Zero-duration trimmed stack likewise yields an empty track.
    auto zero = new o::Stack("zero", r(3.0, 0.0, 24.0));
    auto zero_track = new o::Track("zero_track");
    zero_track->append_child(make_clip("Z", 0.0, 10.0, 24.0));
    zero->append_child(zero_track);
    o::SerializableObject::Retainer<o::Stack> zero_hold(zero);
    o::SerializableObject::Retainer<o::Track> flat_zero(
        o::flatten_stack_recursive(zero, &e));
    TASK_CHECK(flat_zero && !o::is_error(e));
    TASK_CHECK(flat_zero->children().empty());
    return 0;
}

static int
test_occluded_leaf_split_and_gap_fill()
{
    // Same leaf wins, is occluded, then wins again: two copies, never merged.
    o::ErrorStatus e;
    auto scene = new o::Stack("scene");
    auto low = new o::Track("low");
    low->append_child(make_clip("L", 40.0, 20.0, 24.0));

    auto high = new o::Track("high");
    high->append_child(make_gap(4.0, 24.0));
    high->append_child(make_clip("H", 80.0, 6.0, 24.0));
    high->append_child(make_gap(10.0, 24.0));

    scene->append_child(low);
    scene->append_child(high);
    o::SerializableObject::Retainer<o::Stack> scene_hold(scene);
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &e));
    TASK_CHECK(flat && !o::is_error(e));
    TASK_CHECK(flat->children().size() == 3);
    TASK_CHECK(dynamic_cast<o::Clip*>(flat->children()[0].value)->name()
               == "L");
    TASK_CHECK(dynamic_cast<o::Clip*>(flat->children()[1].value)->name()
               == "H");
    TASK_CHECK(dynamic_cast<o::Clip*>(flat->children()[2].value)->name()
               == "L");
    auto tail = dynamic_cast<o::Clip*>(flat->children()[2].value);
    TASK_CHECK(range_eq(tail->source_range(), RationalTime(50, 24),
                        RationalTime(10, 24)));
    return 0;
}

// ---------------------------------------------------------------------------
// coordinates group
// ---------------------------------------------------------------------------

static int
test_mixed_rates_no_rounding()
{
    // 24fps bottom, 25fps nested stack, 48000Hz audio-rate gap.  Media
    // coordinates must be derived from rational time, never frame-rounded.
    o::ErrorStatus e;
    auto scene = new o::Stack("scene");
    auto low = new o::Track("low");
    low->append_child(make_clip("B", 1000.0, 48.0, 24.0)); // 2 seconds

    auto nest = new o::Stack("nest", r(0.0, 50.0, 25.0)); // 2 seconds
    auto inside = new o::Track("inside");
    inside->append_child(new o::Gap(RationalTime(24000.0, 48000.0))); // 0.5s
    inside->append_child(make_clip("A", 300.0, 25.0, 25.0));          // 1s
    inside->append_child(new o::Gap(RationalTime(24000.0, 48000.0))); // 0.5s
    nest->append_child(inside);

    auto top = new o::Track("top");
    top->append_child(nest);
    scene->append_child(low);
    scene->append_child(top);

    o::SerializableObject::Retainer<o::Stack> scene_hold(scene);
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &e));
    TASK_CHECK(flat && !o::is_error(e));
    TASK_CHECK(std::fabs(flat->duration(&e).to_seconds() - 2.0)
               < TEST_EPSILON);
    TASK_CHECK(flat->children().size() == 3);

    auto b0 = dynamic_cast<o::Clip*>(flat->children()[0].value);
    auto a = dynamic_cast<o::Clip*>(flat->children()[1].value);
    auto b1 = dynamic_cast<o::Clip*>(flat->children()[2].value);
    TASK_CHECK(b0 && a && b1);
    TASK_CHECK(b0->name() == "B" && a->name() == "A" && b1->name() == "B");

    // First B segment: 0.5s at 24fps = 12 frames, starting at media 1000.
    TASK_CHECK(time_eq(b0->source_range()->start_time(),
                       RationalTime(1000, 24)));
    TASK_CHECK(std::fabs(
                   b0->source_range()->duration().to_seconds() - 0.5)
               < TEST_EPSILON);

    // A keeps its native 25fps media coordinates, full 25-frame / 1s span.
    TASK_CHECK(a->source_range()->start_time().rate() == 25.0);
    TASK_CHECK(time_eq(a->source_range()->start_time(),
                       RationalTime(300, 25)));
    TASK_CHECK(time_eq(a->source_range()->duration(),
                       RationalTime(25, 25)));

    // Second B segment starts at media 1000 + 36 frames (1.5s * 24).
    TASK_CHECK(time_eq(b1->source_range()->start_time(),
                       RationalTime(1036, 24)));
    TASK_CHECK(time_eq(b1->source_range()->duration(),
                       RationalTime(12, 24)));
    return 0;
}

static int
test_fractional_time_not_rounded()
{
    // A 25fps nested stack begins 1/25s into the 24fps output, so the bottom
    // 24fps clip's first winning slice has a sub-frame media length.  The
    // result must stay fractional rather than being rounded to a frame.
    o::ErrorStatus e;
    auto scene = new o::Stack("scene");
    auto low = new o::Track("low");
    low->append_child(make_clip("B", 1000.0, 48.0, 24.0)); // 2 seconds

    auto nest = new o::Stack("nest", r(1.0, 25.0, 25.0)); // trim 1/25..26/25
    auto inside = new o::Track("inside");
    inside->append_child(make_clip("A", 0.0, 24.0, 25.0));
    nest->append_child(inside);
    auto top = new o::Track("top");
    top->append_child(nest);

    scene->append_child(low);
    scene->append_child(top);
    o::SerializableObject::Retainer<o::Stack> scene_hold(scene);
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &e));
    TASK_CHECK(flat && !o::is_error(e));
    // The trimmed stack's visible content starts at program 0: A covers
    // [0,23/25), then B resumes for [23/25,2).
    TASK_CHECK(flat->children().size() == 2);

    auto a = dynamic_cast<o::Clip*>(flat->children()[0].value);
    auto b = dynamic_cast<o::Clip*>(flat->children()[1].value);
    TASK_CHECK(a && b);
    TASK_CHECK(a->name() == "A" && b->name() == "B");

    // A keeps 25fps media coordinates; the trim drops its first frame, so the
    // surviving picture starts at media 1 and runs 23 frames.
    TASK_CHECK(a->source_range()->start_time().rate() == 25.0);
    TASK_CHECK(time_eq(a->source_range()->start_time(),
                       RationalTime(1, 25)));
    TASK_CHECK(time_eq(a->source_range()->duration(),
                       RationalTime(23, 25)));

    // B resumes at program 23/25s -> media 22.08 frames, a fractional value,
    // and its 25.92-frame length is likewise fractional: nothing was snapped
    // to a whole frame boundary.
    TASK_CHECK(b->source_range()->start_time().rate() == 24.0);
    TASK_CHECK(std::fabs(
                   b->source_range()->start_time().value() - 1022.08)
               < TEST_EPSILON);
    TASK_CHECK(b->source_range()->start_time().value()
               != std::floor(b->source_range()->start_time().value()));
    TASK_CHECK(std::fabs(b->source_range()->duration().value() - 25.92)
               < TEST_EPSILON);
    return 0;
}

static int
test_media_name_effects_metadata_preserved()
{
    o::ErrorStatus e;
    auto scene = new o::Stack("scene");
    auto low = new o::Track("low");
    auto clip = make_clip("Named", 7.0, 10.0, 24.0, "file://media.mov");
    clip->metadata()["tag"] = std::string("value");
    clip->metadata()["n"] = int64_t(42);
    // A non-time effect must survive.
    clip->effects().emplace_back(new o::Effect("filter", "Blur"));
    low->append_child(clip);
    scene->append_child(low);

    o::SerializableObject::Retainer<o::Stack> scene_hold(scene);
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &e));
    TASK_CHECK(flat && !o::is_error(e));
    auto out = dynamic_cast<o::Clip*>(flat->children()[0].value);
    TASK_CHECK(out && out->name() == "Named");
    auto ref = dynamic_cast<o::ExternalReference*>(out->media_reference());
    TASK_CHECK(ref && ref->target_url() == "file://media.mov");
    TASK_CHECK(out->effects().size() == 1);
    TASK_CHECK(out->effects()[0]->effect_name() == "Blur");
    TASK_CHECK(out->metadata().has_key("tag"));
    TASK_CHECK(std::any_cast<std::string>(out->metadata().at("tag"))
               == "value");
    TASK_CHECK(std::any_cast<int64_t>(out->metadata().at("n")) == 42);
    return 0;
}

static int
test_nested_stack_not_treated_as_opaque()
{
    // The nested stack contains a hole; the lower clip must show through it
    // rather than the whole stack behaving as one opaque picture.
    o::ErrorStatus e;
    auto scene = new o::Stack("scene");
    auto low = new o::Track("low");
    low->append_child(make_clip("L", 0.0, 10.0, 24.0));

    auto nest = new o::Stack("nest");
    auto inside = new o::Track("inside");
    inside->append_child(make_clip("U", 20.0, 3.0, 24.0));
    inside->append_child(make_gap(4.0, 24.0));
    inside->append_child(make_clip("U", 23.0, 3.0, 24.0));
    nest->append_child(inside);
    auto top = new o::Track("top");
    top->append_child(nest);

    scene->append_child(low);
    scene->append_child(top);
    o::SerializableObject::Retainer<o::Stack> scene_hold(scene);
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &e));
    TASK_CHECK(flat && !o::is_error(e));
    TASK_CHECK(flat->children().size() == 3);
    TASK_CHECK(dynamic_cast<o::Clip*>(flat->children()[0].value)->name()
               == "U");
    // The lower L clip shows through the nested stack's internal hole.
    TASK_CHECK(dynamic_cast<o::Clip*>(flat->children()[1].value)->name()
               == "L");
    TASK_CHECK(dynamic_cast<o::Clip*>(flat->children()[2].value)->name()
               == "U");
    auto middle = dynamic_cast<o::Clip*>(flat->children()[1].value);
    TASK_CHECK(range_eq(middle->source_range(), RationalTime(3, 24),
                        RationalTime(4, 24)));
    return 0;
}

static int
test_json_roundtrip_editable()
{
    o::ErrorStatus e;
    o::SerializableObject::Retainer<o::Stack> scene(build_canonical(true));
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &e));
    TASK_CHECK(flat && !o::is_error(e));

    std::string json = flat->to_json_string(&e);
    TASK_CHECK(!o::is_error(e) && !json.empty());
    o::SerializableObject::Retainer<o::SerializableObject> back(
        o::SerializableObject::from_json_string(json, &e));
    TASK_CHECK(back && !o::is_error(e));
    auto re_track = dynamic_cast<o::Track*>(back.value);
    TASK_CHECK(re_track);
    TASK_CHECK(re_track->children().size() == 3);

    // Still editable: trim it, then flatten it again inside a stack.
    auto trimmed = o::track_trimmed_to_range(
        re_track, r(2.0, 6.0, 24.0), &e);
    TASK_CHECK(trimmed && !o::is_error(e));
    o::SerializableObject::Retainer<o::Track> trim_hold(trimmed);
    TASK_CHECK(time_eq(trimmed->duration(&e), RationalTime(6, 24)));

    auto wrap = new o::Stack("wrap");
    wrap->append_child(trimmed);
    o::SerializableObject::Retainer<o::Stack> wrap_hold(wrap);
    o::SerializableObject::Retainer<o::Track> re_flat(
        o::flatten_stack_recursive(wrap, &e));
    TASK_CHECK(re_flat && !o::is_error(e));
    TASK_CHECK(time_eq(re_flat->duration(&e), RationalTime(6, 24)));
    TASK_CHECK(!re_flat->children().empty());
    return 0;
}

static int
test_copies_are_independent()
{
    o::ErrorStatus e;
    o::SerializableObject::Retainer<o::Stack> scene(build_canonical(false));
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &e));
    TASK_CHECK(flat);

    auto out = dynamic_cast<o::Clip*>(flat->children()[1].value);
    TASK_CHECK(out);
    auto top_track = dynamic_cast<o::Track*>(scene->children()[1].value);
    auto nested = dynamic_cast<o::Stack*>(top_track->children()[0].value);
    auto inside = dynamic_cast<o::Track*>(nested->children()[0].value);
    auto in_a = o::SerializableObject::Retainer<o::Clip>(
        dynamic_cast<o::Clip*>(inside->children()[1].value));
    TASK_CHECK(in_a && in_a->name() == "A");

    // Output media reference must not be shared with the input object.
    TASK_CHECK(out->media_reference() != in_a->media_reference());
    TASK_CHECK(out != in_a.value);

    // Mutating the output must not touch the input.
    out->set_name("changed");
    out->metadata()["x"] = int64_t(1);
    TASK_CHECK(in_a->name() == "A");
    TASK_CHECK(!in_a->metadata().has_key("x"));

    // The input tree is unchanged: re-flattening gives the same result.
    o::SerializableObject::Retainer<o::Track> again(
        o::flatten_stack_recursive(scene, &e));
    TASK_CHECK(again && again->children().size() == 3);
    auto again_a = dynamic_cast<o::Clip*>(again->children()[1].value);
    TASK_CHECK(again_a && again_a->name() == "A");
    TASK_CHECK(again_a->media_reference() != out->media_reference());
    return 0;
}

// ---------------------------------------------------------------------------
// markers group
// ---------------------------------------------------------------------------

static int
test_marker_projection_canonical()
{
    o::ErrorStatus e;
    o::SerializableObject::Retainer<o::Stack> scene(build_canonical(false));

    auto nest = dynamic_cast<o::Stack*>(
        dynamic_cast<o::Track*>(scene->children()[1].value)
            ->children()[0].value);
    TASK_CHECK(nest);
    // Marker in the nested stack's local coordinates.
    nest->markers().emplace_back(
        new o::Marker("m_nest", r(0.0, 12.0, 24.0), o::Color::red,
                      o::AnyDictionary(), "comment-nest"));

    auto b_clip = dynamic_cast<o::Clip*>(
        dynamic_cast<o::Track*>(scene->children()[0].value)
            ->children()[0].value);
    TASK_CHECK(b_clip && b_clip->name() == "B");
    // Marker on B expressed in media coordinates.
    b_clip->markers().emplace_back(
        new o::Marker("m_b", r(100.0, 20.0, 24.0), o::Color::blue));

    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &e));
    TASK_CHECK(flat && !o::is_error(e));

    // No markers remain on the output clips.
    for (auto const& child: flat->children())
    {
        if (auto clip = dynamic_cast<o::Clip*>(child.value))
        {
            TASK_CHECK(clip->markers().empty());
        }
    }

    auto const& markers = flat->markers();
    // m_nest -> one projected range [2,10); m_b -> two ranges [0,2),[10,20).
    TASK_CHECK(markers.size() == 3);
    TASK_CHECK(markers[0]->name() == "m_b");
    TASK_CHECK(time_eq(markers[0]->marked_range().start_time(),
                       RationalTime(0, 24)));
    TASK_CHECK(time_eq(markers[0]->marked_range().duration(),
                       RationalTime(2, 24)));
    TASK_CHECK(markers[1]->name() == "m_nest");
    TASK_CHECK(time_eq(markers[1]->marked_range().start_time(),
                       RationalTime(2, 24)));
    TASK_CHECK(time_eq(markers[1]->marked_range().duration(),
                       RationalTime(8, 24)));
    TASK_CHECK(markers[1]->comment() == "comment-nest");
    TASK_CHECK(markers[2]->name() == "m_b");
    TASK_CHECK(time_eq(markers[2]->marked_range().start_time(),
                       RationalTime(10, 24)));
    TASK_CHECK(time_eq(markers[2]->marked_range().duration(),
                       RationalTime(10, 24)));
    return 0;
}

static int
test_marker_splits_and_point_at_boundary()
{
    o::ErrorStatus e;
    auto scene = new o::Stack("scene");
    auto low = new o::Track("low");
    low->append_child(make_clip("L", 0.0, 20.0, 24.0));
    auto high = new o::Track("high");
    high->append_child(make_gap(4.0, 24.0));
    high->append_child(make_clip("H", 0.0, 6.0, 24.0));
    high->append_child(make_gap(10.0, 24.0));
    scene->append_child(low);
    scene->append_child(high);

    // Range marker spanning the whole low clip; it must split around H.
    auto low_clip = dynamic_cast<o::Clip*>(low->children()[0].value);
    low_clip->markers().emplace_back(
        new o::Marker("range", r(0.0, 20.0, 24.0)));
    // Point markers: one strictly inside a visible L run, one exactly on the
    // run's end boundary (must be dropped), one hidden behind H.
    low_clip->markers().emplace_back(
        new o::Marker("point_in", r(2.0, 0.0, 24.0)));
    low_clip->markers().emplace_back(
        new o::Marker("point_end", r(4.0, 0.0, 24.0)));
    low_clip->markers().emplace_back(
        new o::Marker("point_hidden", r(6.0, 0.0, 24.0)));

    o::SerializableObject::Retainer<o::Stack> scene_hold(scene);
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &e));
    TASK_CHECK(flat && !o::is_error(e));

    auto const& markers = flat->markers();
    // range -> [0,4) and [10,20); point_in -> at 2.
    TASK_CHECK(markers.size() == 3);
    TASK_CHECK(markers[0]->name() == "range");
    TASK_CHECK(time_eq(markers[0]->marked_range().start_time(),
                       RationalTime(0, 24)));
    TASK_CHECK(time_eq(markers[0]->marked_range().duration(),
                       RationalTime(4, 24)));
    TASK_CHECK(markers[1]->name() == "point_in");
    TASK_CHECK(time_eq(markers[1]->marked_range().start_time(),
                       RationalTime(2, 24)));
    TASK_CHECK(markers[1]->marked_range().duration().value() == 0.0);
    TASK_CHECK(markers[2]->name() == "range");
    TASK_CHECK(time_eq(markers[2]->marked_range().start_time(),
                       RationalTime(10, 24)));
    TASK_CHECK(time_eq(markers[2]->marked_range().duration(),
                       RationalTime(10, 24)));
    for (auto const& m: markers)
    {
        TASK_CHECK(m->name() != "point_end");
        TASK_CHECK(m->name() != "point_hidden");
    }
    return 0;
}

static int
test_marker_color_and_metadata_preserved()
{
    o::ErrorStatus e;
    auto scene = new o::Stack("scene");
    auto track = new o::Track("track");
    track->append_child(make_clip("C", 0.0, 5.0, 24.0));
    scene->append_child(track);

    o::AnyDictionary md;
    md["k"] = std::string("v");
    scene->markers().emplace_back(new o::Marker(
        "m", r(1.0, 3.0, 24.0), o::Color::purple, md, "note"));

    o::SerializableObject::Retainer<o::Stack> scene_hold(scene);
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &e));
    TASK_CHECK(flat && !o::is_error(e));
    TASK_CHECK(flat->markers().size() == 1);
    auto const& m = flat->markers()[0];
    TASK_CHECK(m->name() == "m");
    TASK_CHECK(m->comment() == "note");
    TASK_CHECK(m->color().has_value() && *m->color() == o::Color::purple);
    TASK_CHECK(std::any_cast<std::string>(m->metadata().at("k")) == "v");
    TASK_CHECK(time_eq(m->marked_range().start_time(),
                       RationalTime(1, 24)));
    TASK_CHECK(time_eq(m->marked_range().duration(),
                       RationalTime(3, 24)));
    return 0;
}

static int
test_marker_adjacent_leaves_and_edges()
{
    o::ErrorStatus e;
    auto scene = new o::Stack("scene");
    auto track = new o::Track("track");
    track->append_child(make_clip("X", 0.0, 4.0, 24.0));
    track->append_child(make_gap(2.0, 24.0));
    track->append_child(make_clip("Y", 10.0, 4.0, 24.0));
    scene->append_child(track);

    // Marker over the whole track: the gap is invisible, so it splits into
    // [0,4) and [6,10) even though both clips descend from the same track.
    track->markers().emplace_back(
        new o::Marker("split", r(0.0, 10.0, 24.0)));
    // A marker ending exactly on the first clip's edge stays a single piece.
    track->markers().emplace_back(
        new o::Marker("edge", r(1.0, 3.0, 24.0)));

    // A second adjacent-leaf case with no gap: marker stays one piece across
    // two different leaf clips.
    auto scene2 = new o::Stack("scene2");
    auto track2 = new o::Track("track2");
    track2->append_child(make_clip("P", 0.0, 4.0, 24.0));
    track2->append_child(make_clip("Q", 4.0, 4.0, 24.0));
    scene2->append_child(track2);
    track2->markers().emplace_back(
        new o::Marker("joined", r(0.0, 8.0, 24.0)));

    o::SerializableObject::Retainer<o::Stack> scene_hold(scene);
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &e));
    TASK_CHECK(flat && !o::is_error(e));
    auto const& markers = flat->markers();
    // split -> two pieces, edge -> one piece.
    TASK_CHECK(markers.size() == 3);
    TASK_CHECK(markers[0]->name() == "split");
    TASK_CHECK(time_eq(markers[0]->marked_range().start_time(),
                       RationalTime(0, 24)));
    TASK_CHECK(time_eq(markers[0]->marked_range().duration(),
                       RationalTime(4, 24)));
    TASK_CHECK(markers[1]->name() == "edge");
    TASK_CHECK(time_eq(markers[1]->marked_range().start_time(),
                       RationalTime(1, 24)));
    TASK_CHECK(time_eq(markers[1]->marked_range().duration(),
                       RationalTime(3, 24)));
    TASK_CHECK(markers[2]->name() == "split");
    TASK_CHECK(time_eq(markers[2]->marked_range().start_time(),
                       RationalTime(6, 24)));
    TASK_CHECK(time_eq(markers[2]->marked_range().duration(),
                       RationalTime(4, 24)));

    o::SerializableObject::Retainer<o::Stack> scene2_hold(scene2);
    o::SerializableObject::Retainer<o::Track> flat2(
        o::flatten_stack_recursive(scene2, &e));
    TASK_CHECK(flat2 && !o::is_error(e));
    TASK_CHECK(flat2->markers().size() == 1);
    TASK_CHECK(flat2->markers()[0]->name() == "joined");
    TASK_CHECK(time_eq(flat2->markers()[0]->marked_range().start_time(),
                       RationalTime(0, 24)));
    TASK_CHECK(time_eq(flat2->markers()[0]->marked_range().duration(),
                       RationalTime(8, 24)));
    return 0;
}

static int
test_marker_trim_clipping()
{
    // A marker wider than the owner's trim is clipped to the trim, then to
    // the visible coverage.
    o::ErrorStatus e;
    auto scene = new o::Stack("scene", r(2.0, 6.0, 24.0));
    auto track = new o::Track("track");
    track->append_child(make_clip("C", 0.0, 20.0, 24.0));
    scene->append_child(track);
    // Marker covers the whole stack local range.
    scene->markers().emplace_back(
        new o::Marker("m", r(0.0, 20.0, 24.0)));

    o::SerializableObject::Retainer<o::Stack> scene_hold(scene);
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &e));
    TASK_CHECK(flat && !o::is_error(e));
    TASK_CHECK(flat->markers().size() == 1);
    TASK_CHECK(time_eq(flat->markers()[0]->marked_range().start_time(),
                       RationalTime(0, 24)));
    TASK_CHECK(time_eq(flat->markers()[0]->marked_range().duration(),
                       RationalTime(6, 24)));
    return 0;
}

// ---------------------------------------------------------------------------
// failure inputs
// ---------------------------------------------------------------------------

static int
test_transition_fails()
{
    o::ErrorStatus e;
    auto scene = new o::Stack("scene");
    auto track = new o::Track("track");
    track->append_child(make_clip("C", 0.0, 5.0, 24.0));
    track->append_child(new o::Transition(
        "t", o::Transition::Type::SMPTE_Dissolve,
        RationalTime(2, 24), RationalTime(2, 24)));
    scene->append_child(track);
    o::SerializableObject::Retainer<o::Stack> scene_hold(scene);

    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &e));
    TASK_CHECK(!flat);
    TASK_CHECK(o::is_error(e));
    return 0;
}

static int
test_time_effect_fails()
{
    o::ErrorStatus e;
    auto scene = new o::Stack("scene");
    auto track = new o::Track("track");
    auto clip = make_clip("C", 0.0, 5.0, 24.0);
    clip->effects().emplace_back(new o::LinearTimeWarp("w", "warp", 2.0));
    track->append_child(clip);
    scene->append_child(track);
    o::SerializableObject::Retainer<o::Stack> scene_hold(scene);

    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &e));
    TASK_CHECK(!flat);
    TASK_CHECK(o::is_error(e));
    return 0;
}

static int
test_unsupported_type_fails()
{
    o::ErrorStatus e;
    auto scene = new o::Stack("scene");
    // A raw Composable is neither an Item nor a Transition.
    scene->append_child(new o::Composable("stray"));
    o::SerializableObject::Retainer<o::Stack> scene_hold(scene);

    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &e));
    TASK_CHECK(!flat);
    TASK_CHECK(o::is_error(e));
    return 0;
}

static int
test_invalid_time_fails()
{
    o::ErrorStatus e;

    // Negative duration via a backwards source range.
    auto bad = new o::Stack("bad");
    bad->set_source_range(r(5.0, -2.0, 24.0));
    o::SerializableObject::Retainer<o::Stack> bad_hold(bad);
    o::SerializableObject::Retainer<o::Track> flat_bad(
        o::flatten_stack_recursive(bad, &e));
    TASK_CHECK(!flat_bad && o::is_error(e));

    // Non-finite time.
    o::ErrorStatus e2;
    auto nan_stack = new o::Stack("nan");
    nan_stack->set_source_range(TimeRange(
        RationalTime(std::nan(""), 24.0), RationalTime(5.0, 24.0)));
    o::SerializableObject::Retainer<o::Stack> nan_hold(nan_stack);
    o::SerializableObject::Retainer<o::Track> flat_nan(
        o::flatten_stack_recursive(nan_stack, &e2));
    TASK_CHECK(!flat_nan && o::is_error(e2));

    // Non-positive rate.
    o::ErrorStatus e3;
    auto zero_rate = new o::Stack("zero_rate");
    auto zr_track = new o::Track("zr_track");
    zr_track->append_child(new o::Clip(
        "Z", nullptr,
        TimeRange(RationalTime(0.0, 0.0), RationalTime(5.0, 0.0))));
    zero_rate->append_child(zr_track);
    o::SerializableObject::Retainer<o::Stack> zr_hold(zero_rate);
    o::SerializableObject::Retainer<o::Track> flat_zr(
        o::flatten_stack_recursive(zero_rate, &e3));
    TASK_CHECK(!flat_zr && o::is_error(e3));
    return 0;
}

static int
test_null_input_fails()
{
    o::ErrorStatus e;
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(nullptr, &e));
    TASK_CHECK(!flat && o::is_error(e));
    return 0;
}

// ---------------------------------------------------------------------------
// registry
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
    { "visibility", "canonical_untrimmed", test_canonical_untrimmed },
    { "visibility", "canonical_outer_trim", test_canonical_outer_trim },
    { "visibility", "alternating_nesting", test_alternating_nesting },
    { "visibility", "disabled_branch_reveals_lower",
      test_disabled_branch_reveals_lower },
    { "visibility", "fully_transparent_and_empty",
      test_fully_transparent_and_empty },
    { "visibility", "occluded_leaf_split", test_occluded_leaf_split_and_gap_fill },

    { "coordinates", "mixed_rates_no_rounding", test_mixed_rates_no_rounding },
    { "coordinates", "fractional_time_not_rounded",
      test_fractional_time_not_rounded },
    { "coordinates", "preserve_media_effects_metadata",
      test_media_name_effects_metadata_preserved },
    { "coordinates", "nested_stack_not_opaque",
      test_nested_stack_not_treated_as_opaque },
    { "coordinates", "json_roundtrip_editable", test_json_roundtrip_editable },
    { "coordinates", "copies_are_independent", test_copies_are_independent },

    { "markers", "projection_canonical", test_marker_projection_canonical },
    { "markers", "splits_and_point_boundary",
      test_marker_splits_and_point_at_boundary },
    { "markers", "color_metadata_preserved",
      test_marker_color_and_metadata_preserved },
    { "markers", "adjacent_leaves_and_edges",
      test_marker_adjacent_leaves_and_edges },
    { "markers", "trim_clipping", test_marker_trim_clipping },

    { "visibility", "fail_transition", test_transition_fails },
    { "visibility", "fail_time_effect", test_time_effect_fails },
    { "visibility", "fail_unsupported_type", test_unsupported_type_fails },
    { "visibility", "fail_invalid_time", test_invalid_time_fails },
    { "visibility", "fail_null_input", test_null_input_fails },
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
