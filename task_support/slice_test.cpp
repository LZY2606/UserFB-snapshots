// C++ usage example and regression tests for TransitionSlice: trimming a
// track through the middle of an SMPTE dissolve now preserves the remaining
// blend as a TransitionSlice item instead of failing.

#include "task_support/check.h"

#include "opentimelineio/clip.h"
#include "opentimelineio/deserialization.h"
#include "opentimelineio/externalReference.h"
#include "opentimelineio/linearTimeWarp.h"
#include "opentimelineio/serialization.h"
#include "opentimelineio/track.h"
#include "opentimelineio/trackAlgorithm.h"
#include "opentimelineio/transition.h"
#include "opentimelineio/transitionSlice.h"

#include <string>

namespace o = OTIO_NS;
using o::RationalTime;
using o::SerializableObject;
using o::TimeRange;

namespace {

// The 24fps example setup: A[100,124), B[200,224), a 12-frame SMPTE
// dissolve (in/out offsets of 6) covering track range [18,30).
SerializableObject::Retainer<o::Track>
make_track(double rate = 24.0)
{
    o::ErrorStatus                      e;
    SerializableObject::Retainer<o::Track> t(new o::Track("edit"));
    auto media = [rate](const char* url) {
        return new o::ExternalReference(
            url, TimeRange(RationalTime(0, rate), RationalTime(500, rate)));
    };
    t->append_child(
        new o::Clip(
            "A",
            media("A.mov"),
            TimeRange(RationalTime(100, rate), RationalTime(24, rate))),
        &e);
    t->append_child(
        new o::Transition(
            "dissolve",
            o::Transition::Type::SMPTE_Dissolve,
            RationalTime(6, rate),
            RationalTime(6, rate)),
        &e);
    t->append_child(
        new o::Clip(
            "B",
            media("B.mov"),
            TimeRange(RationalTime(200, rate), RationalTime(24, rate))),
        &e);
    return t;
}

o::TransitionSlice*
only_slice(SerializableObject::Retainer<o::Track> const& track)
{
    if (!track || track->children().size() != 1)
    {
        return nullptr;
    }
    return dynamic_cast<o::TransitionSlice*>(track->children()[0].value);
}

// Trim [21,27) out of the example setup: a 6-frame slice of the blend.
int
partial_middle()
{
    o::ErrorStatus e;
    auto           t = make_track();
    SerializableObject::Retainer<o::Track> r(
        o::track_trimmed_to_range(
            t, TimeRange(RationalTime(21, 24), RationalTime(6, 24)), &e));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(r && r->duration(&e) == RationalTime(6, 24));

    o::TransitionSlice* s = only_slice(r);
    TASK_CHECK(s != nullptr);
    TASK_CHECK(s->source_range().has_value());
    TASK_CHECK(s->source_range()->start_time() == RationalTime(3, 24));
    TASK_CHECK(s->source_range()->duration() == RationalTime(6, 24));
    TASK_CHECK(s->full_duration() == RationalTime(12, 24));
    TASK_CHECK(s->outgoing_start() == RationalTime(118, 24));
    TASK_CHECK(s->incoming_start() == RationalTime(194, 24));

    // First kept frame: full-dissolve time 3 -> 121/197, weight 1/4.
    auto s0 = s->sample_at(RationalTime(0, 24), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(s0.outgoing_time == RationalTime(121, 24));
    TASK_CHECK(s0.incoming_time == RationalTime(197, 24));
    TASK_CHECK(s0.incoming_weight == 0.25);

    // Last kept frame: full-dissolve time 8 -> 126/202, weight 2/3.
    auto s5 = s->sample_at(RationalTime(5, 24), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(s5.outgoing_time == RationalTime(126, 24));
    TASK_CHECK(s5.incoming_time == RationalTime(202, 24));
    TASK_CHECK(s5.incoming_weight == 2.0 / 3.0);

    // Sampling outside [0, duration) reports through ErrorStatus.
    e = o::ErrorStatus();
    s->sample_at(RationalTime(6, 24), &e);
    TASK_CHECK(o::is_error(e));
    e = o::ErrorStatus();
    s->sample_at(RationalTime(-1, 24), &e);
    TASK_CHECK(o::is_error(e));

    // The input track is untouched.
    TASK_CHECK(t->children().size() == 3);
    return 0;
}

// Trim [21,40): 9 kept blend frames followed by B's solo source [206,216).
int
partial_through()
{
    o::ErrorStatus e;
    auto           t = make_track();
    SerializableObject::Retainer<o::Track> r(
        o::track_trimmed_to_range(
            t, TimeRange(RationalTime(21, 24), RationalTime(19, 24)), &e));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(r && r->children().size() == 2);
    TASK_CHECK(r->duration(&e) == RationalTime(19, 24));

    auto* s = dynamic_cast<o::TransitionSlice*>(r->children()[0].value);
    TASK_CHECK(s != nullptr);
    TASK_CHECK(s->source_range()->start_time() == RationalTime(3, 24));
    TASK_CHECK(s->source_range()->duration() == RationalTime(9, 24));

    auto s0 = s->sample_at(RationalTime(0, 24), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(s0.outgoing_time == RationalTime(121, 24));
    TASK_CHECK(s0.incoming_time == RationalTime(197, 24));
    TASK_CHECK(s0.incoming_weight == 0.25);

    // Blend runs to the end of the full dissolve without restarting.
    auto s8 = s->sample_at(RationalTime(8, 24), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(s8.outgoing_time == RationalTime(129, 24));
    TASK_CHECK(s8.incoming_time == RationalTime(205, 24));
    TASK_CHECK(s8.incoming_weight == 11.0 / 12.0);

    // B continues right after the blend; its transition frames are not
    // counted twice.
    auto* b = dynamic_cast<o::Clip*>(r->children()[1].value);
    TASK_CHECK(b != nullptr);
    TASK_CHECK(b->source_range()->start_time() == RationalTime(206, 24));
    TASK_CHECK(b->source_range()->duration() == RationalTime(10, 24));

    // The slice keeps deep-copy snapshots of both participants.
    auto* out_ref = dynamic_cast<o::ExternalReference*>(
        s->outgoing()->media_reference());
    auto* in_ref = dynamic_cast<o::ExternalReference*>(
        s->incoming()->media_reference());
    TASK_CHECK(out_ref && out_ref->target_url() == "A.mov");
    TASK_CHECK(in_ref && in_ref->target_url() == "B.mov");
    return 0;
}

// A transition fully inside the trim range keeps its original form.
int
full_transition_kept()
{
    o::ErrorStatus e;
    auto           t = make_track();
    SerializableObject::Retainer<o::Track> r(
        o::track_trimmed_to_range(
            t, TimeRange(RationalTime(10, 24), RationalTime(30, 24)), &e));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(r && r->children().size() == 3);
    TASK_CHECK(r->duration(&e) == RationalTime(30, 24));

    auto* a = dynamic_cast<o::Clip*>(r->children()[0].value);
    auto* tr = dynamic_cast<o::Transition*>(r->children()[1].value);
    auto* b = dynamic_cast<o::Clip*>(r->children()[2].value);
    TASK_CHECK(a && tr && b);
    TASK_CHECK(
        dynamic_cast<o::TransitionSlice*>(r->children()[1].value) == nullptr);
    TASK_CHECK(tr->in_offset() == RationalTime(6, 24));
    TASK_CHECK(tr->out_offset() == RationalTime(6, 24));
    TASK_CHECK(a->source_range()->start_time() == RationalTime(110, 24));
    TASK_CHECK(a->source_range()->duration() == RationalTime(14, 24));
    TASK_CHECK(b->source_range()->start_time() == RationalTime(200, 24));
    TASK_CHECK(b->source_range()->duration() == RationalTime(16, 24));
    return 0;
}

// Fractional rates are preserved through slicing and sampling.
int
fractional_rate()
{
    o::ErrorStatus e;
    const double   rate = 24000.0 / 1001.0;
    auto           t    = make_track(rate);
    SerializableObject::Retainer<o::Track> r(
        o::track_trimmed_to_range(
            t,
            TimeRange(RationalTime(21, rate), RationalTime(6, rate)),
            &e));
    TASK_CHECK(!o::is_error(e));
    o::TransitionSlice* s = only_slice(r);
    TASK_CHECK(s != nullptr);
    TASK_CHECK(s->source_range()->start_time().rate() == rate);
    TASK_CHECK(s->source_range()->start_time().value() == 3);
    TASK_CHECK(s->full_duration().rate() == rate);

    auto s0 = s->sample_at(RationalTime(0, rate), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(s0.outgoing_time.rate() == rate);
    TASK_CHECK(s0.outgoing_time.value() == 121);
    TASK_CHECK(s0.incoming_time.rate() == rate);
    TASK_CHECK(s0.incoming_time.value() == 197);
    TASK_CHECK(s0.incoming_weight == 0.25);
    return 0;
}

// A TransitionSlice can be built natively as a complete dissolve.
int
native_construction()
{
    o::ErrorStatus e;
    auto           media = [](const char* url) {
        return new o::ExternalReference(
            url, TimeRange(RationalTime(0, 24), RationalTime(500, 24)));
    };
    SerializableObject::Retainer<o::Clip> outgoing(
        new o::Clip(
            "a",
            media("A.mov"),
            TimeRange(RationalTime(118, 24), RationalTime(12, 24))));
    SerializableObject::Retainer<o::Clip> incoming(
        new o::Clip(
            "b",
            media("B.mov"),
            TimeRange(RationalTime(194, 24), RationalTime(12, 24))));
    SerializableObject::Retainer<o::TransitionSlice> s(
        new o::TransitionSlice(
            "full",
            outgoing,
            incoming,
            RationalTime(12, 24),
            RationalTime(118, 24),
            RationalTime(194, 24)));

    // No source_range given: the slice covers the complete dissolve.
    TASK_CHECK(s->source_range().has_value());
    TASK_CHECK(s->source_range()->start_time() == RationalTime(0, 24));
    TASK_CHECK(s->source_range()->duration() == RationalTime(12, 24));
    TASK_CHECK(s->duration(&e) == RationalTime(12, 24));

    auto first = s->sample_at(RationalTime(0, 24), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(first.outgoing_time == RationalTime(118, 24));
    TASK_CHECK(first.incoming_time == RationalTime(194, 24));
    TASK_CHECK(first.incoming_weight == 0.0);
    auto last = s->sample_at(RationalTime(11, 24), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(last.outgoing_time == RationalTime(129, 24));
    TASK_CHECK(last.incoming_weight == 11.0 / 12.0);

    // A non-positive full dissolve length cannot be sampled.
    SerializableObject::Retainer<o::TransitionSlice> bad(
        new o::TransitionSlice());
    e = o::ErrorStatus();
    bad->sample_at(RationalTime(0, 24), &e);
    TASK_CHECK(o::is_error(e));
    return 0;
}

// Unsupported configurations report errors and leave the input unchanged.
int
invalid_participants()
{
    // Non-SMPTE transition types are rejected.
    {
        o::ErrorStatus                      e;
        SerializableObject::Retainer<o::Track> t(new o::Track("edit"));
        auto media = [](const char* url) {
            return new o::ExternalReference(
                url, TimeRange(RationalTime(0, 24), RationalTime(500, 24)));
        };
        t->append_child(
            new o::Clip(
                "A",
                media("A.mov"),
                TimeRange(RationalTime(100, 24), RationalTime(24, 24))),
            &e);
        t->append_child(
            new o::Transition(
                "wipe",
                o::Transition::Type::Custom,
                RationalTime(6, 24),
                RationalTime(6, 24)),
            &e);
        t->append_child(
            new o::Clip(
                "B",
                media("B.mov"),
                TimeRange(RationalTime(200, 24), RationalTime(24, 24))),
            &e);
        e = o::ErrorStatus();
        SerializableObject::Retainer<o::Track> r(
            o::track_trimmed_to_range(
                t, TimeRange(RationalTime(21, 24), RationalTime(6, 24)), &e));
        TASK_CHECK(!r);
        TASK_CHECK(e.outcome == o::ErrorStatus::CANNOT_TRIM_TRANSITION);
        TASK_CHECK(t->children().size() == 3);
    }

    // A transition without both participating clips is rejected.
    {
        o::ErrorStatus                      e;
        SerializableObject::Retainer<o::Track> t(new o::Track("edit"));
        t->append_child(
            new o::Clip(
                "A",
                new o::ExternalReference(
                    "A.mov",
                    TimeRange(RationalTime(0, 24), RationalTime(500, 24))),
                TimeRange(RationalTime(100, 24), RationalTime(24, 24))),
            &e);
        t->append_child(
            new o::Transition(
                "dissolve",
                o::Transition::Type::SMPTE_Dissolve,
                RationalTime(6, 24),
                RationalTime(6, 24)),
            &e);
        e = o::ErrorStatus();
        SerializableObject::Retainer<o::Track> r(
            o::track_trimmed_to_range(
                t, TimeRange(RationalTime(21, 24), RationalTime(6, 24)), &e));
        TASK_CHECK(!r);
        TASK_CHECK(e.outcome == o::ErrorStatus::CANNOT_TRIM_TRANSITION);
        TASK_CHECK(t->children().size() == 2);
    }

    // Clips carrying time effects cannot participate.
    {
        o::ErrorStatus e;
        auto           t = make_track();
        auto* b = dynamic_cast<o::Clip*>(t->children()[2].value);
        TASK_CHECK(b != nullptr);
        b->effects().push_back(
            new o::LinearTimeWarp("tw", "LinearTimeWarp", 2.0));
        SerializableObject::Retainer<o::Track> r(
            o::track_trimmed_to_range(
                t, TimeRange(RationalTime(21, 24), RationalTime(6, 24)), &e));
        TASK_CHECK(!r);
        TASK_CHECK(e.outcome == o::ErrorStatus::CANNOT_TRIM_TRANSITION);
        TASK_CHECK(t->children().size() == 3);
    }

    // Media must cover the full handles of the dissolve.
    {
        o::ErrorStatus                      e;
        SerializableObject::Retainer<o::Track> t(new o::Track("edit"));
        t->append_child(
            new o::Clip(
                "A",
                new o::ExternalReference(
                    "A.mov",
                    TimeRange(RationalTime(100, 24), RationalTime(24, 24))),
                TimeRange(RationalTime(100, 24), RationalTime(24, 24))),
            &e);
        t->append_child(
            new o::Transition(
                "dissolve",
                o::Transition::Type::SMPTE_Dissolve,
                RationalTime(6, 24),
                RationalTime(6, 24)),
            &e);
        t->append_child(
            new o::Clip(
                "B",
                new o::ExternalReference(
                    "B.mov",
                    TimeRange(RationalTime(0, 24), RationalTime(500, 24))),
                TimeRange(RationalTime(200, 24), RationalTime(24, 24))),
            &e);
        e = o::ErrorStatus();
        SerializableObject::Retainer<o::Track> r(
            o::track_trimmed_to_range(
                t, TimeRange(RationalTime(21, 24), RationalTime(6, 24)), &e));
        TASK_CHECK(!r);
        TASK_CHECK(e.outcome == o::ErrorStatus::CANNOT_TRIM_TRANSITION);
        TASK_CHECK(t->children().size() == 3);
    }

    // Invalid trim ranges are rejected.
    {
        o::ErrorStatus e;
        auto           t = make_track();
        SerializableObject::Retainer<o::Track> r(
            o::track_trimmed_to_range(
                t, TimeRange(RationalTime(10, 24), RationalTime(-5, 24)), &e));
        TASK_CHECK(!r);
        TASK_CHECK(e.outcome == o::ErrorStatus::INVALID_TIME_RANGE);
        TASK_CHECK(t->children().size() == 3);
    }
    return 0;
}

// Trim the 6-frame slice again: [2,5) -> source_range [5,8).
int
retrim_middle()
{
    o::ErrorStatus e;
    auto           t = make_track();
    SerializableObject::Retainer<o::Track> r1(
        o::track_trimmed_to_range(
            t, TimeRange(RationalTime(21, 24), RationalTime(6, 24)), &e));
    TASK_CHECK(!o::is_error(e));
    o::TransitionSlice* s1 = only_slice(r1);
    TASK_CHECK(s1 != nullptr);

    SerializableObject::Retainer<o::Track> r2(
        o::track_trimmed_to_range(
            r1, TimeRange(RationalTime(2, 24), RationalTime(3, 24)), &e));
    TASK_CHECK(!o::is_error(e));
    o::TransitionSlice* s2 = only_slice(r2);
    TASK_CHECK(s2 != nullptr);
    TASK_CHECK(s2->source_range()->start_time() == RationalTime(5, 24));
    TASK_CHECK(s2->source_range()->duration() == RationalTime(3, 24));
    TASK_CHECK(s2->full_duration() == RationalTime(12, 24));

    // First sample: full-dissolve time 5 -> 123/199, weight 5/12.
    auto s0 = s2->sample_at(RationalTime(0, 24), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(s0.outgoing_time == RationalTime(123, 24));
    TASK_CHECK(s0.incoming_time == RationalTime(199, 24));
    TASK_CHECK(s0.incoming_weight == 5.0 / 12.0);

    auto s2end = s2->sample_at(RationalTime(2, 24), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(s2end.outgoing_time == RationalTime(125, 24));
    TASK_CHECK(s2end.incoming_time == RationalTime(201, 24));
    TASK_CHECK(s2end.incoming_weight == 7.0 / 12.0);

    // The intermediate result is not disturbed by the second trim.
    TASK_CHECK(s1->source_range()->start_time() == RationalTime(3, 24));
    TASK_CHECK(s1->source_range()->duration() == RationalTime(6, 24));
    return 0;
}

// Results are deep copies: mutating them never touches the input.
int
copy_independence()
{
    o::ErrorStatus e;
    auto           t = make_track();
    SerializableObject::Retainer<o::Track> r(
        o::track_trimmed_to_range(
            t, TimeRange(RationalTime(21, 24), RationalTime(19, 24)), &e));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(r && r->children().size() == 2);

    auto* s = dynamic_cast<o::TransitionSlice*>(r->children()[0].value);
    auto* b = dynamic_cast<o::Clip*>(r->children()[1].value);
    TASK_CHECK(s && b);

    // Rewrite the result: slice snapshots and the surviving clip.
    s->outgoing()->set_source_range(
        TimeRange(RationalTime(0, 24), RationalTime(1, 24)));
    s->incoming()->set_source_range(
        TimeRange(RationalTime(0, 24), RationalTime(1, 24)));
    b->set_source_range(TimeRange(RationalTime(0, 24), RationalTime(1, 24)));
    s->set_source_range(TimeRange(RationalTime(4, 24), RationalTime(2, 24)));

    // The input track is completely unaffected.
    TASK_CHECK(t->children().size() == 3);
    auto* in_a = dynamic_cast<o::Clip*>(t->children()[0].value);
    auto* in_tr = dynamic_cast<o::Transition*>(t->children()[1].value);
    auto* in_b = dynamic_cast<o::Clip*>(t->children()[2].value);
    TASK_CHECK(in_a && in_tr && in_b);
    TASK_CHECK(in_a->source_range()->start_time() == RationalTime(100, 24));
    TASK_CHECK(in_a->source_range()->duration() == RationalTime(24, 24));
    TASK_CHECK(in_b->source_range()->start_time() == RationalTime(200, 24));
    TASK_CHECK(in_b->source_range()->duration() == RationalTime(24, 24));
    TASK_CHECK(in_tr->in_offset() == RationalTime(6, 24));

    // Mutating the input afterwards does not leak into the result either.
    in_b->set_source_range(TimeRange(RationalTime(0, 24), RationalTime(2, 24)));
    auto s0 = s->sample_at(RationalTime(0, 24), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(s0.outgoing_time == RationalTime(122, 24));
    TASK_CHECK(s0.incoming_time == RationalTime(198, 24));
    TASK_CHECK(s0.incoming_weight == 1.0 / 3.0);
    return 0;
}

// A slice survives a JSON round trip and can be trimmed again.
int
json_roundtrip()
{
    o::ErrorStatus e;
    auto           t = make_track();
    SerializableObject::Retainer<o::Track> r1(
        o::track_trimmed_to_range(
            t, TimeRange(RationalTime(21, 24), RationalTime(6, 24)), &e));
    TASK_CHECK(!o::is_error(e));

    const std::string json = r1->to_json_string(&e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(json.find("TransitionSlice.1") != std::string::npos);

    SerializableObject::Retainer<o::Track> restored(
        dynamic_cast<o::Track*>(
            o::SerializableObject::from_json_string(json, &e)));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(restored && restored->children().size() == 1);
    TASK_CHECK(restored->duration(&e) == RationalTime(6, 24));

    o::TransitionSlice* s = only_slice(restored);
    TASK_CHECK(s != nullptr);
    TASK_CHECK(s->source_range()->start_time() == RationalTime(3, 24));
    TASK_CHECK(s->full_duration() == RationalTime(12, 24));
    auto s0 = s->sample_at(RationalTime(0, 24), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(s0.outgoing_time == RationalTime(121, 24));
    TASK_CHECK(s0.incoming_time == RationalTime(197, 24));
    TASK_CHECK(s0.incoming_weight == 0.25);

    // Snapshots survived serialization with their media references.
    auto* out_ref = dynamic_cast<o::ExternalReference*>(
        s->outgoing()->media_reference());
    TASK_CHECK(out_ref && out_ref->target_url() == "A.mov");

    // The restored track still participates in track layout and retrim.
    auto ranges = restored->range_of_all_children(&e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(ranges.size() == 1);
    SerializableObject::Retainer<o::Track> r2(
        o::track_trimmed_to_range(
            restored,
            TimeRange(RationalTime(2, 24), RationalTime(3, 24)),
            &e));
    TASK_CHECK(!o::is_error(e));
    o::TransitionSlice* s2 = only_slice(r2);
    TASK_CHECK(s2 != nullptr);
    TASK_CHECK(s2->source_range()->start_time() == RationalTime(5, 24));
    auto rs = s2->sample_at(RationalTime(0, 24), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(rs.outgoing_time == RationalTime(123, 24));
    TASK_CHECK(rs.incoming_weight == 5.0 / 12.0);
    return 0;
}

// A slice survives clone() and the clone is independent.
int
clone_roundtrip()
{
    o::ErrorStatus e;
    auto           t = make_track();
    SerializableObject::Retainer<o::Track> r1(
        o::track_trimmed_to_range(
            t, TimeRange(RationalTime(21, 24), RationalTime(19, 24)), &e));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(r1 && r1->children().size() == 2);

    SerializableObject::Retainer<o::Track> c(
        dynamic_cast<o::Track*>(r1->clone(&e)));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(c && c->children().size() == 2);
    TASK_CHECK(c->duration(&e) == RationalTime(19, 24));

    auto* cs = dynamic_cast<o::TransitionSlice*>(c->children()[0].value);
    TASK_CHECK(cs != nullptr);
    auto s0 = cs->sample_at(RationalTime(0, 24), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(s0.outgoing_time == RationalTime(121, 24));
    TASK_CHECK(s0.incoming_time == RationalTime(197, 24));
    TASK_CHECK(s0.incoming_weight == 0.25);

    // The clone lays out on the track and can be trimmed again.
    SerializableObject::Retainer<o::Track> c2(
        o::track_trimmed_to_range(
            c, TimeRange(RationalTime(0, 24), RationalTime(5, 24)), &e));
    TASK_CHECK(!o::is_error(e));
    o::TransitionSlice* cs2 = only_slice(c2);
    TASK_CHECK(cs2 != nullptr);
    TASK_CHECK(cs2->source_range()->start_time() == RationalTime(3, 24));
    TASK_CHECK(cs2->source_range()->duration() == RationalTime(5, 24));
    auto s4 = cs2->sample_at(RationalTime(4, 24), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(s4.outgoing_time == RationalTime(125, 24));
    TASK_CHECK(s4.incoming_weight == 7.0 / 12.0);

    // The clone is independent from the original result.
    cs->set_source_range(TimeRange(RationalTime(0, 24), RationalTime(1, 24)));
    auto* orig = dynamic_cast<o::TransitionSlice*>(r1->children()[0].value);
    TASK_CHECK(orig != nullptr);
    TASK_CHECK(orig->source_range()->start_time() == RationalTime(3, 24));
    TASK_CHECK(orig->source_range()->duration() == RationalTime(9, 24));
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    { "partial", "partial_middle", partial_middle },
    { "partial", "partial_through", partial_through },
    { "partial", "full_transition_kept", full_transition_kept },
    { "partial", "fractional_rate", fractional_rate },
    { "partial", "native_construction", native_construction },
    { "partial", "invalid_participants", invalid_participants },
    { "retrim", "retrim_middle", retrim_middle },
    { "retrim", "copy_independence", copy_independence },
    { "persistence", "json_roundtrip", json_roundtrip },
    { "persistence", "clone_roundtrip", clone_roundtrip },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
