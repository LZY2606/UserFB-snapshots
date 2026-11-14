// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#include "opentimelineio/trackAlgorithm.h"
#include "opentimelineio/clip.h"
#include "opentimelineio/timeEffect.h"
#include "opentimelineio/transition.h"
#include "opentimelineio/transitionSlice.h"

#include <set>

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {

namespace {

// Subtract the half-open interval [cut_start, cut_end) from the half-open
// interval [*keep_start, *keep_end). Returns false if nothing remains.
bool
_subtract_kept_range(
    RationalTime*       keep_start,
    RationalTime*       keep_end,
    RationalTime const& cut_start,
    RationalTime const& cut_end)
{
    if (cut_end <= *keep_start || cut_start >= *keep_end)
    {
        return true;
    }
    if (cut_start <= *keep_start && cut_end >= *keep_end)
    {
        return false;
    }
    if (cut_start <= *keep_start)
    {
        *keep_start = cut_end;
    }
    else
    {
        *keep_end = cut_start;
    }
    return *keep_start < *keep_end;
}

} // namespace

Track*
track_trimmed_to_range(
    Track*       in_track,
    TimeRange    trim_range,
    ErrorStatus* error_status)
{
    if (trim_range.start_time().is_invalid_time()
        || trim_range.duration().is_invalid_time()
        || trim_range.duration().value() <= 0)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::INVALID_TIME_RANGE,
                "trim range must have a valid, positive duration");
        }
        return nullptr;
    }

    SerializableObject::Retainer<Track> new_track_holder(
        dynamic_cast<Track*>(in_track->clone(error_status)));
    if (is_error(error_status) || !new_track_holder)
    {
        return nullptr;
    }
    Track* new_track = new_track_holder;

    auto track_map = new_track->range_of_all_children(error_status);
    if (is_error(error_status))
    {
        return nullptr;
    }

    std::vector<Composable*> children_copy(
        new_track->children().begin(),
        new_track->children().end());

    // Pass 1: find transitions the trim cuts into and, when supported,
    // convert the kept blend into a TransitionSlice. All validation
    // happens here, before anything is mutated, so the input is never
    // modified and an error leaves the (cloned) result unbuilt.
    struct SlicedTransition
    {
        int       index;
        TimeRange range; // range of the transition in track time
        SerializableObject::Retainer<TransitionSlice> slice;
    };
    std::vector<SlicedTransition> sliced;

    for (size_t i = 0; i < children_copy.size(); i++)
    {
        Transition* transition = dynamic_cast<Transition*>(children_copy[i]);
        if (!transition)
        {
            continue;
        }

        auto transition_range_it = track_map.find(children_copy[i]);
        if (transition_range_it == track_map.end())
        {
            if (error_status)
            {
                *error_status = ErrorStatus(
                    ErrorStatus::CANNOT_COMPUTE_AVAILABLE_RANGE,
                    "failed to find child in track_map map");
            }
            return nullptr;
        }
        const TimeRange transition_range = transition_range_it->second;

        if (!trim_range.intersects(transition_range)
            || trim_range.contains(transition_range))
        {
            // Untouched or fully kept transitions keep their original
            // representation.
            continue;
        }

        // Slicing a transition is only supported for top-level tracks
        // (no source_range set on the track itself).
        if (new_track->source_range())
        {
            if (error_status)
            {
                *error_status = ErrorStatus(
                    ErrorStatus::CANNOT_TRIM_TRANSITION,
                    "Cannot trim in the middle of a transition");
            }
            return nullptr;
        }

        if (transition->transition_type() != Transition::Type::SMPTE_Dissolve)
        {
            if (error_status)
            {
                *error_status = ErrorStatus(
                    ErrorStatus::CANNOT_TRIM_TRANSITION,
                    "Cannot trim in the middle of a transition of "
                    "unsupported type",
                    transition);
            }
            return nullptr;
        }

        Clip* outgoing = i > 0
                             ? dynamic_cast<Clip*>(children_copy[i - 1])
                             : nullptr;
        Clip* incoming = i + 1 < children_copy.size()
                             ? dynamic_cast<Clip*>(children_copy[i + 1])
                             : nullptr;
        if (!outgoing || !incoming)
        {
            if (error_status)
            {
                *error_status = ErrorStatus(
                    ErrorStatus::CANNOT_TRIM_TRANSITION,
                    "Cannot trim transition that is missing a participating "
                    "clip",
                    transition);
            }
            return nullptr;
        }

        for (Clip* participant: { outgoing, incoming })
        {
            for (auto const& effect: participant->effects())
            {
                if (dynamic_cast<TimeEffect*>(effect.value))
                {
                    if (error_status)
                    {
                        *error_status = ErrorStatus(
                            ErrorStatus::CANNOT_TRIM_TRANSITION,
                            "Cannot trim transition next to a clip with a "
                            "time effect",
                            participant);
                    }
                    return nullptr;
                }
            }
        }

        // The coverage ranges of adjacent transitions must not intersect.
        for (size_t j = 0; j < children_copy.size(); j++)
        {
            if (j == i || !dynamic_cast<Transition*>(children_copy[j]))
            {
                continue;
            }
            if (track_map[children_copy[j]].intersects(transition_range))
            {
                if (error_status)
                {
                    *error_status = ErrorStatus(
                        ErrorStatus::CANNOT_TRIM_TRANSITION,
                        "Cannot trim transition whose range intersects "
                        "another transition",
                        transition);
                }
                return nullptr;
            }
        }

        const RationalTime full_duration =
            transition->in_offset() + transition->out_offset();
        if (full_duration.is_invalid_time() || full_duration.value() <= 0)
        {
            if (error_status)
            {
                *error_status = ErrorStatus(
                    ErrorStatus::INVALID_TIME_RANGE,
                    "Cannot trim transition with a non-positive dissolve "
                    "duration",
                    transition);
            }
            return nullptr;
        }

        const TimeRange outgoing_track_range = track_map[outgoing];
        const TimeRange incoming_track_range = track_map[incoming];
        const TimeRange outgoing_source =
            outgoing->trimmed_range(error_status);
        if (is_error(error_status))
        {
            return nullptr;
        }
        const TimeRange incoming_source =
            incoming->trimmed_range(error_status);
        if (is_error(error_status))
        {
            return nullptr;
        }

        // Media time of each participant at the start of the full
        // dissolve; both sides run at normal speed across the blend.
        const RationalTime outgoing_start =
            outgoing_source.start_time()
            + (transition_range.start_time()
               - outgoing_track_range.start_time());
        const RationalTime incoming_start =
            incoming_source.start_time()
            + (transition_range.start_time()
               - incoming_track_range.start_time());

        // Both participants must have media (handles) covering the
        // complete dissolve.
        const TimeRange outgoing_available =
            outgoing->available_range(error_status);
        if (is_error(error_status))
        {
            return nullptr;
        }
        const TimeRange incoming_available =
            incoming->available_range(error_status);
        if (is_error(error_status))
        {
            return nullptr;
        }
        if (!outgoing_available.contains(
                TimeRange(outgoing_start, full_duration))
            || !incoming_available.contains(
                TimeRange(incoming_start, full_duration)))
        {
            if (error_status)
            {
                *error_status = ErrorStatus(
                    ErrorStatus::CANNOT_TRIM_TRANSITION,
                    "Cannot trim transition because the participating clips "
                    "do not have enough handles",
                    transition);
            }
            return nullptr;
        }

        // The kept blend, expressed on the timeline of the full dissolve.
        const RationalTime keep_start = std::max(
            trim_range.start_time(), transition_range.start_time());
        const RationalTime keep_end = std::min(
            trim_range.end_time_exclusive(),
            transition_range.end_time_exclusive());

        // Deep-copy snapshots of the participants: later changes to the
        // result cannot affect the input, nor vice versa.
        SerializableObject::Retainer<Clip> outgoing_snapshot(
            dynamic_cast<Clip*>(outgoing->clone(error_status)));
        if (is_error(error_status) || !outgoing_snapshot)
        {
            return nullptr;
        }
        SerializableObject::Retainer<Clip> incoming_snapshot(
            dynamic_cast<Clip*>(incoming->clone(error_status)));
        if (is_error(error_status) || !incoming_snapshot)
        {
            return nullptr;
        }

        sliced.push_back(SlicedTransition{
            static_cast<int>(i),
            transition_range,
            SerializableObject::Retainer<TransitionSlice>(
                new TransitionSlice(
                    transition->name(),
                    outgoing_snapshot,
                    incoming_snapshot,
                    full_duration,
                    outgoing_start,
                    incoming_start,
                    TimeRange(
                        keep_start - transition_range.start_time(),
                        keep_end - keep_start),
                    transition->metadata())) });
    }

    // Pass 2: replace cut transitions by their slices. Indices into
    // children_copy stay valid because replacements keep the child count.
    std::set<int> sliced_indices;
    for (auto const& s: sliced)
    {
        new_track->set_child(s.index, s.slice, error_status);
        if (is_error(error_status))
        {
            return nullptr;
        }
        sliced_indices.insert(s.index);
    }

    // Pass 3: drop or trim the remaining children, iterating backwards so
    // removals do not disturb pending indices.
    for (size_t i = children_copy.size(); i--;)
    {
        if (sliced_indices.count(static_cast<int>(i)))
        {
            // Already replaced by a slice carrying the kept blend.
            continue;
        }

        Composable* child          = children_copy[i];
        auto        child_range_it = track_map.find(child);
        if (child_range_it == track_map.end())
        {
            if (error_status)
            {
                *error_status = ErrorStatus(
                    ErrorStatus::CANNOT_COMPUTE_AVAILABLE_RANGE,
                    "failed to find child in track_map map");
            }
            return nullptr;
        }

        auto child_range = child_range_it->second;
        if (!trim_range.intersects(child_range))
        {
            new_track->remove_child(static_cast<int>(i), error_status);
            if (is_error(error_status))
            {
                return nullptr;
            }
            continue;
        }

        // Kept interval of this child, in track coordinates. The blend
        // region of a sliced transition now belongs to the slice, so the
        // neighbors only keep their solo parts.
        RationalTime keep_start =
            std::max(trim_range.start_time(), child_range.start_time());
        RationalTime keep_end = std::min(
            trim_range.end_time_exclusive(),
            child_range.end_time_exclusive());
        bool keep_empty = false;
        for (auto const& s: sliced)
        {
            if (!_subtract_kept_range(
                    &keep_start,
                    &keep_end,
                    s.range.start_time(),
                    s.range.end_time_exclusive()))
            {
                keep_empty = true;
                break;
            }
        }

        if (keep_empty || !(keep_start < keep_end))
        {
            new_track->remove_child(static_cast<int>(i), error_status);
            if (is_error(error_status))
            {
                return nullptr;
            }
            continue;
        }

        if (keep_start == child_range.start_time()
            && keep_end == child_range.end_time_exclusive())
        {
            // Fully kept: leave the child untouched.
            continue;
        }

        Item* child_item = dynamic_cast<Item*>(child);
        if (!child_item)
        {
            if (error_status)
            {
                *error_status = ErrorStatus(
                    ErrorStatus::TYPE_MISMATCH,
                    "Expected child of type Item*",
                    child);
            }
            return nullptr;
        }
        auto child_source_range = child_item->trimmed_range(error_status);
        if (is_error(error_status))
        {
            return nullptr;
        }

        child_item->set_source_range(TimeRange(
            child_source_range.start_time()
                + (keep_start - child_range.start_time()),
            keep_end - keep_start));
    }

    return new_track_holder.take_value();
}

}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS
