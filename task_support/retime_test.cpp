// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project
//
// Regression coverage for RetimedClip.  All cases exercise the real library
// (no mocks) and are registered with the task driver via task_cases.

#include "check.h"

#include <opentimelineio/clip.h>
#include <opentimelineio/errorStatus.h>
#include <opentimelineio/externalReference.h>
#include <opentimelineio/gap.h>
#include <opentimelineio/item.h>
#include <opentimelineio/linearTimeWarp.h>
#include <opentimelineio/marker.h>
#include <opentimelineio/retimedClip.h>
#include <opentimelineio/serializableObject.h>
#include <opentimelineio/stack.h>
#include <opentimelineio/track.h>
#include <opentimelineio/trackAlgorithm.h>
#include <opentimelineio/transition.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace o = opentimelineio::OPENTIMELINEIO_VERSION_NS;
using o::RationalTime;
using o::TimeRange;

namespace {

constexpr double kEps = 1e-9;

bool
rt_eq(RationalTime a, RationalTime b)
{
    return a.almost_equal(b, kEps);
}

bool
range_eq(TimeRange a, TimeRange b)
{
    return rt_eq(a.start_time(), b.start_time())
           && rt_eq(a.end_time_exclusive(), b.end_time_exclusive());
}

o::MediaReference*
make_media(TimeRange available)
{
    return new o::ExternalReference("media.mov", available);
}

// Builds a retimed clip with the standard example source range [100, 148).
o::RetimedClip*
make_example_clip(double rate)
{
    auto* clip = new o::RetimedClip("retimed");
    clip->set_media_reference(
        make_media(TimeRange(RationalTime(0, 24), RationalTime(1000, 24))));
    clip->set_source_range(
        TimeRange(RationalTime(100, 24), RationalTime(48, 24)));
    o::ErrorStatus e;
    bool ok = clip->set_playback_rate(rate, &e);
    (void) ok;
    return clip;
}

// -------------------------------------------------------------------------
// duration
// -------------------------------------------------------------------------

int
duration_rates_and_layout()
{
    o::ErrorStatus e;

    // Default construction follows the required interface: rate 1, no media.
    o::SerializableObject::Retainer<o::RetimedClip> fresh(new o::RetimedClip());
    TASK_CHECK(fresh->playback_rate() == 1.0);
    TASK_CHECK(dynamic_cast<o::Clip*>(fresh.value) != nullptr);

    o::SerializableObject::Retainer<o::Track> track(new o::Track("edit"));
    o::SerializableObject::Retainer<o::Gap> gap(new o::Gap(RationalTime(10, 24)));
    o::SerializableObject::Retainer<o::RetimedClip> clip(make_example_clip(-2.0));

    TASK_CHECK(track->append_child(gap, &e));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(track->append_child(clip, &e));
    TASK_CHECK(!o::is_error(e));

    // Program length is the source length divided by |rate|.
    TASK_CHECK(rt_eq(clip->duration(&e), RationalTime(24, 24)));
    TASK_CHECK(!o::is_error(e));

    // Forward playback gives the identical program length.
    clip->set_playback_rate(2.0, &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(rt_eq(clip->duration(&e), RationalTime(24, 24)));

    // Fractional and slow rates are accepted.
    TASK_CHECK(clip->set_playback_rate(0.5, &e));
    TASK_CHECK(rt_eq(clip->duration(&e), RationalTime(96, 24)));
    TASK_CHECK(clip->set_playback_rate(1.0 / 3.0, &e));
    TASK_CHECK(rt_eq(clip->duration(&e), RationalTime(144, 24)));

    // Illegal rates are rejected without disturbing the previous value.
    TASK_CHECK(clip->set_playback_rate(-2.0, &e));
    double before = clip->playback_rate();
    for (double bad: { 0.0,
                       std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity(),
                       -std::numeric_limits<double>::infinity() })
    {
        e = o::ErrorStatus();
        bool ok = clip->set_playback_rate(bad, &e);
        TASK_CHECK(!ok);
        TASK_CHECK(o::is_error(e));
        TASK_CHECK(clip->playback_rate() == before);
    }

    // A following child is laid out by the retimed duration (10 + 24 = 34).
    e = o::ErrorStatus();
    TASK_CHECK(track->append_child(new o::Gap(RationalTime(4, 24)), &e));
    TASK_CHECK(!o::is_error(e));
    TimeRange trailing = track->range_of_child_at_index(2, &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(rt_eq(trailing.start_time(), RationalTime(34, 24)));

    // Changing the rate is reflected immediately in parent-space queries.
    TASK_CHECK(clip->set_playback_rate(4.0, &e));
    TASK_CHECK(!o::is_error(e));
    trailing = track->range_of_child_at_index(2, &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(rt_eq(trailing.start_time(), RationalTime(22, 24)));
    trailing = track->range_of_child_at_index(1, &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(rt_eq(trailing.duration(), RationalTime(12, 24)));

    // With no source_range the clip falls back to the media available_range.
    o::SerializableObject::Retainer<o::RetimedClip> fallback(new o::RetimedClip());
    fallback->set_media_reference(
        make_media(TimeRange(RationalTime(0, 24), RationalTime(48, 24))));
    TASK_CHECK(fallback->set_playback_rate(-2.0, &e));
    TASK_CHECK(rt_eq(fallback->duration(&e), RationalTime(24, 24)));
    TASK_CHECK(!o::is_error(e));
    return 0;
}

int
duration_legacy_compatibility()
{
    o::ErrorStatus e;
    o::SerializableObject::Retainer<o::Track> track(new o::Track("legacy"));

    // A plain clip with a legacy LinearTimeWarp effect keeps a 1:1 layout.
    o::SerializableObject::Retainer<o::Clip> plain(new o::Clip("plain"));
    plain->set_media_reference(
        make_media(TimeRange(RationalTime(0, 24), RationalTime(1000, 24))));
    plain->set_source_range(
        TimeRange(RationalTime(0, 24), RationalTime(48, 24)));
    plain->effects().push_back(new o::LinearTimeWarp("warp", "LinearTimeWarp", 2.0));

    TASK_CHECK(track->append_child(new o::Gap(RationalTime(10, 24)), &e));
    TASK_CHECK(track->append_child(plain, &e));
    TASK_CHECK(!o::is_error(e));

    TASK_CHECK(rt_eq(plain->duration(&e), RationalTime(48, 24)));
    TASK_CHECK(rt_eq(
        track->range_of_child_at_index(1, &e).duration(),
        RationalTime(48, 24)));

    // Legacy time mapping still uses the source start offset.
    TASK_CHECK(rt_eq(
        plain->transformed_time(RationalTime(20, 24), track, &e),
        RationalTime(30, 24)));
    TASK_CHECK(!o::is_error(e));

    // A trailing gap follows the untimed 48-frame length.
    TASK_CHECK(track->append_child(new o::Gap(RationalTime(5, 24)), &e));
    TASK_CHECK(rt_eq(
        track->range_of_child_at_index(2, &e).start_time(),
        RationalTime(58, 24)));
    return 0;
}

// -------------------------------------------------------------------------
// mapping
// -------------------------------------------------------------------------

int
mapping_reverse_boundaries()
{
    o::ErrorStatus e;
    o::SerializableObject::Retainer<o::Track> track(new o::Track("edit"));
    o::SerializableObject::Retainer<o::RetimedClip> clip(make_example_clip(-2.0));

    TASK_CHECK(track->append_child(new o::Gap(RationalTime(10, 24)), &e));
    TASK_CHECK(track->append_child(clip, &e));
    TASK_CHECK(!o::is_error(e));

    // 24fps source [100,148), rate -2, 10-frame gap: 148,136,100 -> 10,16,34.
    TASK_CHECK(rt_eq(
        clip->transformed_time(RationalTime(148, 24), track, &e),
        RationalTime(10, 24)));
    TASK_CHECK(rt_eq(
        clip->transformed_time(RationalTime(136, 24), track, &e),
        RationalTime(16, 24)));
    TASK_CHECK(rt_eq(
        clip->transformed_time(RationalTime(100, 24), track, &e),
        RationalTime(34, 24)));
    TASK_CHECK(!o::is_error(e));

    // The full source interval maps to a non-negative program interval.
    TimeRange in_parent = clip->transformed_time_range(
        TimeRange(RationalTime(100, 24), RationalTime(48, 24)),
        track,
        &e);
    TASK_CHECK(range_eq(
        in_parent,
        TimeRange(RationalTime(10, 24), RationalTime(24, 24))));
    TASK_CHECK(in_parent.duration().value() >= 0.0);

    // The inverse mapping is exact, including through a TimeRange.
    TASK_CHECK(rt_eq(
        track->transformed_time(RationalTime(16, 24), clip, &e),
        RationalTime(136, 24)));
    TASK_CHECK(rt_eq(
        track->transformed_time(RationalTime(10, 24), clip, &e),
        RationalTime(148, 24)));
    TASK_CHECK(rt_eq(
        track->transformed_time(RationalTime(34, 24), clip, &e),
        RationalTime(100, 24)));
    TASK_CHECK(!o::is_error(e));

    TimeRange parent_range(RationalTime(14, 24), RationalTime(8, 24));
    TimeRange in_source = track->transformed_time_range(parent_range, clip, &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(range_eq(
        in_source,
        TimeRange(RationalTime(124, 24), RationalTime(16, 24))));
    TASK_CHECK(in_source.duration().value() >= 0.0);
    return 0;
}

int
mapping_forward_boundaries()
{
    o::ErrorStatus e;
    o::SerializableObject::Retainer<o::Track> track(new o::Track("forward"));
    o::SerializableObject::Retainer<o::RetimedClip> clip(make_example_clip(2.0));

    TASK_CHECK(track->append_child(new o::Gap(RationalTime(10, 24)), &e));
    TASK_CHECK(track->append_child(clip, &e));
    TASK_CHECK(!o::is_error(e));

    // Forward playback walks from the source start.
    TASK_CHECK(rt_eq(
        clip->transformed_time(RationalTime(100, 24), track, &e),
        RationalTime(10, 24)));
    TASK_CHECK(rt_eq(
        clip->transformed_time(RationalTime(148, 24), track, &e),
        RationalTime(34, 24)));
    TASK_CHECK(!o::is_error(e));

    TimeRange in_parent = clip->transformed_time_range(
        TimeRange(RationalTime(100, 24), RationalTime(48, 24)),
        track,
        &e);
    TASK_CHECK(range_eq(
        in_parent,
        TimeRange(RationalTime(10, 24), RationalTime(24, 24))));
    TASK_CHECK(!o::is_error(e));

    // Inverse in both directions.
    TASK_CHECK(rt_eq(
        track->transformed_time(RationalTime(22, 24), clip, &e),
        RationalTime(124, 24)));
    TimeRange back = track->transformed_time_range(
        TimeRange(RationalTime(14, 24), RationalTime(8, 24)),
        clip,
        &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(range_eq(
        back,
        TimeRange(RationalTime(108, 24), RationalTime(16, 24))));
    return 0;
}

int
mapping_nested_mixed_rates()
{
    // Stack -> two Tracks, each with a source_range offset, one holding the
    // retimed clip, the other a normal clip at 48000 Hz.  The two endpoints
    // live in different subtrees and the walk crosses 24 / 25 / 48000 rates.
    o::ErrorStatus e;
    o::SerializableObject::Retainer<o::Stack> root(new o::Stack("root"));

    o::SerializableObject::Retainer<o::Track> outer_a(new o::Track("outerA"));
    outer_a->set_source_range(
        TimeRange(RationalTime(0, 24), RationalTime(1000, 24)));
    o::SerializableObject::Retainer<o::Track> outer_b(new o::Track("outerB"));
    outer_b->set_source_range(
        TimeRange(RationalTime(50, 25), RationalTime(1000, 25)));

    o::SerializableObject::Retainer<o::RetimedClip> clip(make_example_clip(-2.0));
    o::SerializableObject::Retainer<o::Clip> normal(new o::Clip("audio"));
    normal->set_media_reference(
        make_media(TimeRange(RationalTime(0, 48000), RationalTime(480000, 48000))));
    normal->set_source_range(
        TimeRange(RationalTime(800, 48000), RationalTime(48, 48000)));

    TASK_CHECK(outer_a->append_child(new o::Gap(RationalTime(10, 24)), &e));
    TASK_CHECK(outer_a->append_child(clip, &e));
    TASK_CHECK(outer_b->append_child(new o::Gap(RationalTime(5, 48000)), &e));
    TASK_CHECK(outer_b->append_child(normal, &e));
    TASK_CHECK(root->append_child(outer_a, &e));
    TASK_CHECK(root->append_child(outer_b, &e));
    TASK_CHECK(!o::is_error(e));

    // Source 136@24 crosses out through the stack into outerB's local space:
    //   clip -> outerA : 10 + (136-148)/(-2)            = 16@24
    //   outerA -> root : (16 - 0) + 0                   = 16@24
    //   root -> outerB : 16@24 + 50@25                  = 66 2/3@25
    // The 24 -> 25 conversion must stay fractional (no integer rounding).
    RationalTime in_b = clip->transformed_time(RationalTime(136, 24), outer_b, &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(in_b.rate() == 25.0);
    RationalTime expected_b =
        RationalTime(16, 24) + RationalTime(50, 25);
    TASK_CHECK(in_b.almost_equal(expected_b, 1e-9));
    TASK_CHECK(std::abs(in_b.value() - 200.0 / 3.0) < 1e-9);

    // ... and on into the 48000 Hz clip in that other subtree:
    //   66 2/3@25 -> 128000@48000, - 5, + 800 = 128795@48000
    RationalTime in_clip =
        clip->transformed_time(RationalTime(136, 24), normal, &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(rt_eq(in_clip, RationalTime(128795, 48000)));
    TASK_CHECK(in_clip.rate() == 48000.0);

    // Round-trip through the same chain is lossless.
    RationalTime back =
        normal->transformed_time(in_clip, clip, &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(back.almost_equal(RationalTime(136, 24), 1e-9));

    // A range that straddles the reverse mapping keeps both boundaries and a
    // non-negative length across the subtree crossing.
    TimeRange source_range(
        RationalTime(124, 24),
        RationalTime(16, 24));
    TimeRange in_outer_b =
        clip->transformed_time_range(source_range, outer_b, &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(in_outer_b.duration().value() >= 0.0);
    // 124@24 -> program 22@24 ; 140@24 -> program 14@24.  Reversed order is
    // rearranged to a non-negative range, then shifted by outerB's 50@25.
    TASK_CHECK(rt_eq(
        in_outer_b.start_time(),
        RationalTime(14, 24) + RationalTime(50, 25)));
    TASK_CHECK(rt_eq(
        in_outer_b.end_time_exclusive(),
        RationalTime(22, 24) + RationalTime(50, 25)));
    TASK_CHECK(rt_eq(in_outer_b.duration(), RationalTime(8, 24)));
    return 0;
}

// -------------------------------------------------------------------------
// editing_json
// -------------------------------------------------------------------------

int
editing_trim_reverse()
{
    o::ErrorStatus e;
    o::SerializableObject::Retainer<o::Track> track(new o::Track("edit"));
    o::SerializableObject::Retainer<o::RetimedClip> clip(make_example_clip(-2.0));

    TASK_CHECK(track->append_child(new o::Gap(RationalTime(10, 24)), &e));
    TASK_CHECK(track->append_child(clip, &e));
    TASK_CHECK(!o::is_error(e));

    o::SerializableObject::Retainer<o::Track> cut(o::track_trimmed_to_range(
        track,
        TimeRange(RationalTime(14, 24), RationalTime(8, 24)),
        &e));
    TASK_CHECK(cut && !o::is_error(e));
    TASK_CHECK(cut->children().size() == 1);

    auto* part = dynamic_cast<o::RetimedClip*>(cut->children()[0].value);
    TASK_CHECK(part != nullptr);
    TASK_CHECK(part->playback_rate() == -2.0);
    TASK_CHECK(part->source_range().has_value());
    TASK_CHECK(range_eq(
        *part->source_range(),
        TimeRange(RationalTime(124, 24), RationalTime(16, 24))));
    TASK_CHECK(rt_eq(part->duration(&e), RationalTime(8, 24)));
    TASK_CHECK(!o::is_error(e));

    // Media information survives the trim.
    TASK_CHECK(part->media_reference() != nullptr);
    auto* ref = dynamic_cast<o::ExternalReference*>(part->media_reference());
    TASK_CHECK(ref != nullptr);
    TASK_CHECK(ref->target_url() == "media.mov");

    // The input track is untouched.
    auto* original = dynamic_cast<o::RetimedClip*>(track->children()[1].value);
    TASK_CHECK(original != nullptr);
    TASK_CHECK(range_eq(
        *original->source_range(),
        TimeRange(RationalTime(100, 24), RationalTime(48, 24))));
    TASK_CHECK(original->playback_rate() == -2.0);

    // A plain clip trims exactly as before.
    o::SerializableObject::Retainer<o::Track> plain_track(new o::Track("plain"));
    o::SerializableObject::Retainer<o::Clip> plain(new o::Clip("plain"));
    plain->set_media_reference(
        make_media(TimeRange(RationalTime(0, 24), RationalTime(1000, 24))));
    plain->set_source_range(
        TimeRange(RationalTime(100, 24), RationalTime(48, 24)));
    TASK_CHECK(plain_track->append_child(new o::Gap(RationalTime(10, 24)), &e));
    TASK_CHECK(plain_track->append_child(plain, &e));
    auto plain_cut = o::SerializableObject::Retainer<o::Track>(
        o::track_trimmed_to_range(
            plain_track,
            TimeRange(RationalTime(14, 24), RationalTime(8, 24)),
            &e));
    TASK_CHECK(plain_cut && !o::is_error(e));
    auto* plain_part = dynamic_cast<o::Clip*>(plain_cut->children()[0].value);
    TASK_CHECK(plain_part != nullptr);
    TASK_CHECK(range_eq(
        *plain_part->source_range(),
        TimeRange(RationalTime(104, 24), RationalTime(8, 24))));
    return 0;
}

int
editing_transition_handles()
{
    // Reverse: source [100,148), rate -2, head handle 2, tail handle 3.
    {
        o::ErrorStatus e;
        o::SerializableObject::Retainer<o::Track> track(new o::Track("tx"));
        o::SerializableObject::Retainer<o::RetimedClip> clip(make_example_clip(-2.0));

        TASK_CHECK(track->append_child(
            new o::Transition(
                "in",
                o::Transition::Type::SMPTE_Dissolve,
                RationalTime(2, 24),
                RationalTime(0, 24)),
            &e));
        TASK_CHECK(track->append_child(clip, &e));
        TASK_CHECK(track->append_child(
            new o::Transition(
                "out",
                o::Transition::Type::SMPTE_Dissolve,
                RationalTime(0, 24),
                RationalTime(3, 24)),
            &e));
        TASK_CHECK(!o::is_error(e));

        // Handles are program-time lengths; in reverse they swap direction.
        TimeRange visible = clip->visible_range(&e);
        TASK_CHECK(!o::is_error(e));
        TASK_CHECK(range_eq(
            visible,
            TimeRange(RationalTime(94, 24), RationalTime(58, 24))));

        // The stored source range is not pre-shrunk or altered.
        TASK_CHECK(range_eq(
            *clip->source_range(),
            TimeRange(RationalTime(100, 24), RationalTime(48, 24))));
    }

    // Forward: same handles scale into source length but keep direction.
    {
        o::ErrorStatus e;
        o::SerializableObject::Retainer<o::Track> track(new o::Track("tx"));
        o::SerializableObject::Retainer<o::RetimedClip> clip(make_example_clip(2.0));

        TASK_CHECK(track->append_child(
            new o::Transition(
                "in",
                o::Transition::Type::SMPTE_Dissolve,
                RationalTime(2, 24),
                RationalTime(0, 24)),
            &e));
        TASK_CHECK(track->append_child(clip, &e));
        TASK_CHECK(track->append_child(
            new o::Transition(
                "out",
                o::Transition::Type::SMPTE_Dissolve,
                RationalTime(0, 24),
                RationalTime(3, 24)),
            &e));
        TASK_CHECK(!o::is_error(e));

        TimeRange visible = clip->visible_range(&e);
        TASK_CHECK(!o::is_error(e));
        TASK_CHECK(range_eq(
            visible,
            TimeRange(RationalTime(96, 24), RationalTime(58, 24))));
    }

    // A transition fully inside a trim range is still rejected, even when a
    // retimed clip participates.
    {
        o::ErrorStatus e;
        o::SerializableObject::Retainer<o::Track> track(new o::Track("tx"));
        TASK_CHECK(track->append_child(new o::Gap(RationalTime(10, 24)), &e));
        TASK_CHECK(track->append_child(new o::Transition(
                          "mid",
                          o::Transition::Type::SMPTE_Dissolve,
                          RationalTime(2, 24),
                          RationalTime(2, 24)),
                      &e));
        TASK_CHECK(track->append_child(make_example_clip(-2.0), &e));
        TASK_CHECK(!o::is_error(e));

        o::SerializableObject::Retainer<o::Track> cut(
            o::track_trimmed_to_range(
                track,
                TimeRange(RationalTime(11, 24), RationalTime(4, 24)),
                &e));
        TASK_CHECK(cut == nullptr);
        TASK_CHECK(o::is_error(e));
        TASK_CHECK(e.outcome == o::ErrorStatus::CANNOT_TRIM_TRANSITION);
    }
    return 0;
}

int
editing_json_clone_and_continue()
{
    o::ErrorStatus e;
    o::SerializableObject::Retainer<o::Track> track(new o::Track("edit"));
    o::SerializableObject::Retainer<o::RetimedClip> clip(make_example_clip(-2.0));
    clip->metadata()["note"] = std::string("retimed");
    clip->effects().push_back(new o::LinearTimeWarp("warp", "LinearTimeWarp", 1.0));
    clip->markers().push_back(new o::Marker(
        "mark",
        TimeRange(RationalTime(120, 24), RationalTime(1, 24))));

    TASK_CHECK(track->append_child(new o::Gap(RationalTime(10, 24)), &e));
    TASK_CHECK(track->append_child(clip, &e));
    TASK_CHECK(track->append_child(new o::Gap(RationalTime(5, 24)), &e));
    TASK_CHECK(!o::is_error(e));

    std::string json = track->to_json_string(&e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(json.find("RetimedClip.1") != std::string::npos);

    o::SerializableObject::Retainer<o::SerializableObject> decoded(
        o::SerializableObject::from_json_string(json, &e));
    TASK_CHECK(decoded && !o::is_error(e));
    auto* decoded_track = dynamic_cast<o::Track*>(decoded.value);
    TASK_CHECK(decoded_track != nullptr);

    auto* decoded_clip =
        dynamic_cast<o::RetimedClip*>(decoded_track->children()[1].value);
    TASK_CHECK(decoded_clip != nullptr);
    TASK_CHECK(decoded_clip->playback_rate() == -2.0);
    TASK_CHECK(decoded_clip->source_range().has_value());
    TASK_CHECK(range_eq(
        *decoded_clip->source_range(),
        TimeRange(RationalTime(100, 24), RationalTime(48, 24))));
    TASK_CHECK(decoded_clip->metadata().has_key("note"));
    TASK_CHECK(decoded_clip->effects().size() == 1);
    TASK_CHECK(decoded_clip->markers().size() == 1);
    auto* decoded_ref =
        dynamic_cast<o::ExternalReference*>(decoded_clip->media_reference());
    TASK_CHECK(decoded_ref != nullptr);
    TASK_CHECK(decoded_ref->target_url() == "media.mov");

    // Clone preserves the dynamic type and the playback rate.
    o::SerializableObject::Retainer<o::SerializableObject> cloned(
        decoded_clip->clone(&e));
    TASK_CHECK(cloned && !o::is_error(e));
    auto* cloned_clip = dynamic_cast<o::RetimedClip*>(cloned.value);
    TASK_CHECK(cloned_clip != nullptr);
    TASK_CHECK(cloned_clip->playback_rate() == -2.0);
    TASK_CHECK(cloned_clip->schema_name() == "RetimedClip");

    // Editing continues to work after a JSON round-trip: changing the rate
    // immediately updates parent layout.
    TASK_CHECK(decoded_clip->set_playback_rate(4.0, &e));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(rt_eq(decoded_clip->duration(&e), RationalTime(12, 24)));
    TASK_CHECK(rt_eq(
        decoded_track->range_of_child_at_index(2, &e).start_time(),
        RationalTime(22, 24)));
    TASK_CHECK(rt_eq(
        decoded_track->available_range(&e).duration(),
        RationalTime(27, 24)));

    // Reserialization records the new rate and the same schema label.
    std::string again = decoded_track->to_json_string(&e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(again.find("RetimedClip.1") != std::string::npos);

    // An invalid playback rate in JSON must fail, not degrade to a plain Clip.
    std::string bad_json =
        "{\"OTIO_SCHEMA\": \"RetimedClip.1\","
        " \"name\": \"bad\","
        " \"playback_rate\": 0.0,"
        " \"media_references\": {\"DEFAULT_MEDIA\":"
        "   {\"OTIO_SCHEMA\": \"MissingReference.1\"}},"
        " \"active_media_reference_key\": \"DEFAULT_MEDIA\"}";
    e         = o::ErrorStatus();
    auto* bad = o::SerializableObject::from_json_string(bad_json, &e);
    TASK_CHECK(bad == nullptr);
    TASK_CHECK(o::is_error(e));

    // A non-numeric rate is a schema error too.
    std::string wrong_type =
        "{\"OTIO_SCHEMA\": \"RetimedClip.1\","
        " \"name\": \"bad\","
        " \"playback_rate\": \"fast\","
        " \"media_references\": {\"DEFAULT_MEDIA\":"
        "   {\"OTIO_SCHEMA\": \"MissingReference.1\"}},"
        " \"active_media_reference_key\": \"DEFAULT_MEDIA\"}";
    e         = o::ErrorStatus();
    auto* bad2 = o::SerializableObject::from_json_string(wrong_type, &e);
    TASK_CHECK(bad2 == nullptr);
    TASK_CHECK(o::is_error(e));

    // A valid hand-written document decodes as the retimed type.
    std::string good_json =
        "{\"OTIO_SCHEMA\": \"RetimedClip.1\","
        " \"name\": \"good\","
        " \"playback_rate\": -1.5,"
        " \"source_range\": {\"OTIO_SCHEMA\": \"TimeRange.1\","
        "   \"start_time\": {\"OTIO_SCHEMA\": \"RationalTime.1\","
        "     \"rate\": 24.0, \"value\": 100.0},"
        "   \"duration\": {\"OTIO_SCHEMA\": \"RationalTime.1\","
        "     \"rate\": 24.0, \"value\": 48.0}},"
        " \"media_references\": {\"DEFAULT_MEDIA\":"
        "   {\"OTIO_SCHEMA\": \"MissingReference.1\"}},"
        " \"active_media_reference_key\": \"DEFAULT_MEDIA\"}";
    e           = o::ErrorStatus();
    o::SerializableObject::Retainer<o::SerializableObject> good(
        o::SerializableObject::from_json_string(good_json, &e));
    TASK_CHECK(good && !o::is_error(e));
    auto* good_clip = dynamic_cast<o::RetimedClip*>(good.value);
    TASK_CHECK(good_clip != nullptr);
    TASK_CHECK(good_clip->playback_rate() == -1.5);
    TASK_CHECK(rt_eq(good_clip->duration(&e), RationalTime(32, 24)));
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    { "duration", "rates_and_layout", duration_rates_and_layout },
    { "duration", "legacy_compatibility", duration_legacy_compatibility },
    { "mapping", "reverse_boundaries", mapping_reverse_boundaries },
    { "mapping", "forward_boundaries", mapping_forward_boundaries },
    { "mapping", "nested_mixed_rates", mapping_nested_mixed_rates },
    { "editing_json", "trim_reverse", editing_trim_reverse },
    { "editing_json", "transition_handles", editing_transition_handles },
    { "editing_json",
      "json_clone_and_continue",
      editing_json_clone_and_continue },
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
