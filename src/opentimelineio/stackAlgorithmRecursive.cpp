// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#include "opentimelineio/stackAlgorithm.h"
#include "opentimelineio/clip.h"
#include "opentimelineio/effect.h"
#include "opentimelineio/gap.h"
#include "opentimelineio/marker.h"
#include "opentimelineio/mediaReference.h"
#include "opentimelineio/stack.h"
#include "opentimelineio/timeEffect.h"
#include "opentimelineio/track.h"
#include "opentimelineio/transition.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {

namespace {

// Recursive flatten compares positions across tracks that may have different
// rates.  Positions are compared in seconds with a tolerance that is far
// smaller than one sample at the fastest commonly used audio rate (192kHz),
// so that adjacent samples are never collapsed while positions that are
// mathematically equal (but arrived at through different rescaling paths)
// still compare equal.
constexpr double _rf_epsilon_s = 1.0 / (4.0 * 1000000.0);

bool
_rf_valid_time(RationalTime const& t)
{
    return std::isfinite(t.value()) && std::isfinite(t.rate()) && t.rate() > 0;
}

bool
_rf_valid_range(TimeRange const& r)
{
    return _rf_valid_time(r.start_time()) && _rf_valid_time(r.duration())
           && r.duration().value() >= 0;
}

bool
_rf_less(RationalTime const& a, RationalTime const& b)
{
    return a.to_seconds() < b.to_seconds() - _rf_epsilon_s;
}

bool
_rf_time_equal(RationalTime const& a, RationalTime const& b)
{
    return std::fabs(a.to_seconds() - b.to_seconds()) <= _rf_epsilon_s;
}

bool
_rf_intersect(TimeRange const& a, TimeRange const& b, TimeRange* result)
{
    RationalTime start =
        _rf_less(a.start_time(), b.start_time()) ? b.start_time()
                                                 : a.start_time();
    RationalTime end = _rf_less(a.end_time_exclusive(), b.end_time_exclusive())
                           ? a.end_time_exclusive()
                           : b.end_time_exclusive();
    if (!_rf_less(start, end))
    {
        return false;
    }
    *result = TimeRange(start, end - start);
    return true;
}

struct _RF_Interval
{
    RationalTime start;
    RationalTime end;
};

struct _RF_Segment
{
    Clip*     clip;
    TimeRange local;  // in the coordinate space of the node returning it
    TimeRange source; // in the clip's media/source coordinates
};

void
_rf_merge_intervals(std::vector<_RF_Interval>* intervals)
{
    if (intervals->empty())
    {
        return;
    }
    std::stable_sort(
        intervals->begin(),
        intervals->end(),
        [](_RF_Interval const& a, _RF_Interval const& b) {
            return _rf_less(a.start, b.start);
        });

    std::vector<_RF_Interval> merged;
    merged.push_back((*intervals)[0]);
    for (size_t i = 1; i < intervals->size(); ++i)
    {
        _RF_Interval const& current = (*intervals)[i];
        if (_rf_less(merged.back().end, current.start))
        {
            merged.push_back(current);
        }
        else if (_rf_less(merged.back().end, current.end))
        {
            merged.back().end = current.end;
        }
    }
    *intervals = std::move(merged);
}

// Subtract the given intervals from range, returning the remaining pieces.
void
_rf_subtract(
    TimeRange const&                range,
    std::vector<_RF_Interval> const& cut,
    std::vector<_RF_Interval>*      pieces)
{
    std::vector<_RF_Interval> sorted = cut;
    std::stable_sort(
        sorted.begin(),
        sorted.end(),
        [](_RF_Interval const& a, _RF_Interval const& b) {
            return _rf_less(a.start, b.start);
        });

    RationalTime current = range.start_time();
    RationalTime end     = range.end_time_exclusive();

    for (auto const& interval: sorted)
    {
        if (!_rf_less(current, end))
        {
            break;
        }
        if (!_rf_less(interval.start, end))
        {
            break;
        }
        if (!_rf_less(current, interval.end))
        {
            continue;
        }
        if (_rf_less(current, interval.start))
        {
            pieces->push_back({ current, interval.start });
        }
        if (_rf_less(current, interval.end))
        {
            current = interval.end;
        }
    }

    if (_rf_less(current, end))
    {
        pieces->push_back({ current, end });
    }
}

bool
_rf_validate(SerializableObject* obj, ErrorStatus* error_status, int depth)
{
    if (depth > 1000)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::INTERNAL_ERROR,
                "composition nesting is too deep or cyclic");
        }
        return false;
    }

    if (dynamic_cast<Transition*>(obj))
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::TYPE_MISMATCH,
                "Transition is not supported by flatten_stack_recursive",
                obj);
        }
        return false;
    }

    Item* item = dynamic_cast<Item*>(obj);
    if (!item)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::TYPE_MISMATCH,
                "flatten_stack_recursive only supports Stack, Track, Clip and Gap",
                obj);
        }
        return false;
    }

    bool known_type = dynamic_cast<Clip*>(obj) || dynamic_cast<Gap*>(obj)
                      || dynamic_cast<Stack*>(obj) || dynamic_cast<Track*>(obj);
    if (!known_type)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::TYPE_MISMATCH,
                "unsupported item type in flatten_stack_recursive",
                obj);
        }
        return false;
    }

    if (item->source_range() && !_rf_valid_range(*item->source_range()))
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::INVALID_TIME_RANGE,
                "invalid source_range in flatten_stack_recursive input",
                obj);
        }
        return false;
    }

    for (auto const& effect: item->effects())
    {
        if (dynamic_cast<TimeEffect*>(effect.value))
        {
            if (error_status)
            {
                *error_status = ErrorStatus(
                    ErrorStatus::TYPE_MISMATCH,
                    "TimeEffect is not supported by flatten_stack_recursive",
                    effect.value);
            }
            return false;
        }
    }

    for (auto const& marker: item->markers())
    {
        if (!_rf_valid_range(marker->marked_range()))
        {
            if (error_status)
            {
                *error_status = ErrorStatus(
                    ErrorStatus::INVALID_TIME_RANGE,
                    "invalid marker range in flatten_stack_recursive input",
                    marker.value);
            }
            return false;
        }
    }

    if (Clip* clip = dynamic_cast<Clip*>(obj))
    {
        for (auto const& entry: clip->media_references())
        {
            if (entry.second && entry.second->available_range()
                && !_rf_valid_range(*entry.second->available_range()))
            {
                if (error_status)
                {
                    *error_status = ErrorStatus(
                        ErrorStatus::INVALID_TIME_RANGE,
                        "invalid media available_range in flatten_stack_recursive input",
                        entry.second);
                }
                return false;
            }
        }
    }

    if (Composition* composition = dynamic_cast<Composition*>(obj))
    {
        for (auto const& child: composition->children())
        {
            if (!_rf_validate(child.value, error_status, depth + 1))
            {
                return false;
            }
        }
    }

    return true;
}

bool
_rf_flatten_node(
    Item*                      item,
    TimeRange const&           allowed,
    std::vector<_RF_Segment>*  result,
    ErrorStatus*               error_status)
{
    if (!item->enabled() || dynamic_cast<Gap*>(item))
    {
        return true;
    }

    if (Clip* clip = dynamic_cast<Clip*>(item))
    {
        TimeRange trimmed = clip->trimmed_range(error_status);
        if (is_error(error_status))
        {
            return false;
        }
        TimeRange visible;
        if (_rf_intersect(allowed, trimmed, &visible))
        {
            result->push_back({ clip, visible, visible });
        }
        return true;
    }

    Composition* composition = dynamic_cast<Composition*>(item);
    if (!composition)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::TYPE_MISMATCH,
                "unsupported item in flatten_stack_recursive",
                item);
        }
        return false;
    }

    TimeRange content = composition->source_range()
                            ? *composition->source_range()
                            : composition->available_range(error_status);
    if (is_error(error_status))
    {
        return false;
    }

    TimeRange visible;
    if (!_rf_intersect(allowed, content, &visible))
    {
        return true;
    }

    std::map<Composable*, TimeRange> ranges =
        composition->range_of_all_children(error_status);
    if (is_error(error_status))
    {
        return false;
    }

    bool                is_stack = dynamic_cast<Stack*>(item) != nullptr;
    std::vector<_RF_Interval> claimed;
    int                 count = int(composition->children().size());

    for (int step = 0; step < count; ++step)
    {
        // For a Stack the last child is on top, so process back-to-front.
        int index = is_stack ? (count - 1 - step) : step;
        Composable* child = composition->children()[index].value;

        Item* child_item = dynamic_cast<Item*>(child);
        if (!child_item)
        {
            if (error_status)
            {
                *error_status = ErrorStatus(
                    ErrorStatus::TYPE_MISMATCH,
                    "unsupported composable in flatten_stack_recursive",
                    child);
            }
            return false;
        }

        auto range_it = ranges.find(child);
        if (range_it == ranges.end())
        {
            if (error_status)
            {
                *error_status = ErrorStatus(
                    ErrorStatus::INTERNAL_ERROR,
                    "failed to find child range in flatten_stack_recursive");
            }
            return false;
        }
        TimeRange child_range = range_it->second;

        TimeRange in_parent;
        if (!_rf_intersect(visible, child_range, &in_parent))
        {
            continue;
        }

        TimeRange child_trimmed = child_item->trimmed_range(error_status);
        if (is_error(error_status))
        {
            return false;
        }

        // parent coordinate -> child coordinate:
        //   child = parent - range.start + trimmed.start
        TimeRange child_allowed(
            in_parent.start_time() - child_range.start_time()
                + child_trimmed.start_time(),
            in_parent.duration());

        std::vector<_RF_Segment> child_segments;
        if (!_rf_flatten_node(
                child_item,
                child_allowed,
                &child_segments,
                error_status))
        {
            return false;
        }

        for (auto const& segment: child_segments)
        {
            // child coordinate -> parent coordinate:
            //   parent = child - trimmed.start + range.start
            TimeRange mapped_local(
                segment.local.start_time() - child_trimmed.start_time()
                    + child_range.start_time(),
                segment.local.duration());

            RationalTime source_offset =
                segment.source.start_time() - mapped_local.start_time();

            std::vector<_RF_Interval> pieces;
            if (is_stack)
            {
                _rf_subtract(mapped_local, claimed, &pieces);
            }
            else
            {
                pieces.push_back(
                    { mapped_local.start_time(),
                      mapped_local.end_time_exclusive() });
            }

            for (auto const& piece: pieces)
            {
                TimeRange piece_local(
                    piece.start,
                    piece.end - piece.start);

                // Express the source range in the leaf clip's own source
                // rate so the emitted source_range feels native to the media.
                double source_rate = segment.clip->trimmed_range(error_status)
                                         .start_time()
                                         .rate();
                if (is_error(error_status))
                {
                    return false;
                }
                TimeRange piece_source(
                    (piece.start + source_offset).rescaled_to(source_rate),
                    (piece.end - piece.start).rescaled_to(source_rate));
                result->push_back(
                    { segment.clip, piece_local, piece_source });
            }

            if (is_stack)
            {
                claimed.push_back(
                    { mapped_local.start_time(),
                      mapped_local.end_time_exclusive() });
            }
        }
    }

    return true;
}

TimeRange
_rf_map_range_to_parent(
    Item*        child,
    TimeRange    range,
    ErrorStatus* error_status)
{
    Composition* parent = child->parent();
    if (!parent)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::NOT_A_CHILD,
                "object has no parent in flatten_stack_recursive",
                child);
        }
        return range;
    }

    TimeRange trimmed = child->trimmed_range(error_status);
    if (is_error(error_status))
    {
        return range;
    }
    TimeRange parent_range = parent->range_of_child(child, error_status);
    if (is_error(error_status))
    {
        return range;
    }

    return TimeRange(
        range.start_time() - trimmed.start_time() + parent_range.start_time(),
        range.duration());
}

TimeRange
_rf_map_range_up(
    Item*        from,
    Item*        to,
    TimeRange    range,
    ErrorStatus* error_status)
{
    Item* current = from;
    while (current != to)
    {
        if (!current->parent())
        {
            if (error_status)
            {
                *error_status = ErrorStatus(
                    ErrorStatus::NOT_DESCENDED_FROM,
                    "marker owner is not an ancestor of the winning leaf",
                    from);
            }
            return range;
        }
        range   = _rf_map_range_to_parent(current, range, error_status);
        current = current->parent();
        if (is_error(error_status))
        {
            return range;
        }
    }
    return range;
}

bool
_rf_is_descendant(Item* leaf, Item* ancestor)
{
    Item* current = leaf;
    while (current)
    {
        if (current == ancestor)
        {
            return true;
        }
        current = current->parent();
    }
    return false;
}

void
_rf_collect_preorder(Item* item, std::vector<Item*>* out)
{
    out->push_back(item);
    if (Composition* composition = dynamic_cast<Composition*>(item))
    {
        for (auto const& child: composition->children())
        {
            if (Item* child_item = dynamic_cast<Item*>(child.value))
            {
                _rf_collect_preorder(child_item, out);
            }
        }
    }
}

} // namespace

Track*
flatten_stack_recursive(Stack* input, ErrorStatus* error_status)
{
    if (!input)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::TYPE_MISMATCH,
                "flatten_stack_recursive requires a non-null Stack");
        }
        return nullptr;
    }

    if (!_rf_validate(input, error_status, 0))
    {
        return nullptr;
    }

    TimeRange content = input->source_range()
                            ? *input->source_range()
                            : input->available_range(error_status);
    if (is_error(error_status))
    {
        return nullptr;
    }

    SerializableObject::Retainer<Track> flat_track(new Track("Flattened"));

    RationalTime total_duration = content.duration();
    if (!_rf_valid_time(total_duration))
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::INVALID_TIME_RANGE,
                "invalid duration in flatten_stack_recursive input",
                input);
        }
        return nullptr;
    }

    // The output track deliberately carries no source_range: its duration is
    // derived from its children (clips plus filler gaps), exactly like the
    // baseline flattener.  That keeps it re-trimmable by track_trimmed_to_range.
    // A zero-duration input yields an empty track.
    if (!_rf_less(RationalTime(0, total_duration.rate()), total_duration))
    {
        return flat_track.take_value();
    }

    std::vector<_RF_Segment> segments;
    if (!_rf_flatten_node(
            input,
            content,
            &segments,
            error_status))
    {
        return nullptr;
    }

    // Sort by position on the program timeline (the root's local timeline).
    std::stable_sort(
        segments.begin(),
        segments.end(),
        [](_RF_Segment const& a, _RF_Segment const& b) {
            return _rf_less(a.local.start_time(), b.local.start_time());
        });

    // Merge consecutive winning pieces that come from the same leaf clip and
    // are contiguous both in the timeline and in the source media.
    std::vector<_RF_Segment> merged;
    for (auto const& segment: segments)
    {
        if (!merged.empty())
        {
            _RF_Segment& last = merged.back();
            if (last.clip == segment.clip
                && _rf_time_equal(
                    last.local.end_time_exclusive(),
                    segment.local.start_time())
                && _rf_time_equal(
                    last.source.end_time_exclusive(),
                    segment.source.start_time()))
            {
                last.local = TimeRange(
                    last.local.start_time(),
                    segment.local.end_time_exclusive()
                        - last.local.start_time());
                last.source = TimeRange(
                    last.source.start_time(),
                    segment.source.end_time_exclusive()
                        - last.source.start_time());
                continue;
            }
        }
        merged.push_back(segment);
    }

    RationalTime season_start = content.start_time();
    RationalTime cursor(0, total_duration.rate());

    for (auto const& segment: merged)
    {
        RationalTime timeline_start =
            segment.local.start_time() - season_start;
        RationalTime timeline_end =
            segment.local.end_time_exclusive() - season_start;

        if (_rf_less(cursor, timeline_start))
        {
            flat_track->append_child(
                new Gap(timeline_start - cursor),
                error_status);
            if (is_error(error_status))
            {
                return nullptr;
            }
        }

        SerializableObject::Retainer<Clip> clip_clone(
            dynamic_cast<Clip*>(segment.clip->clone(error_status)));
        if (is_error(error_status) || !clip_clone)
        {
            if (!is_error(error_status) && error_status)
            {
                *error_status = ErrorStatus(
                    ErrorStatus::INTERNAL_ERROR,
                    "failed to clone clip in flatten_stack_recursive");
            }
            return nullptr;
        }
        clip_clone->set_source_range(segment.source);
        clip_clone->markers().clear();

        flat_track->append_child(clip_clone, error_status);
        if (is_error(error_status))
        {
            return nullptr;
        }

        cursor = timeline_end;
    }

    if (_rf_less(cursor, total_duration))
    {
        flat_track->append_child(
            new Gap(total_duration - cursor),
            error_status);
        if (is_error(error_status))
        {
            return nullptr;
        }
    }

    // Project markers from every object in the original tree onto the output
    // track, in original tree preorder.
    std::vector<Item*> preorder;
    _rf_collect_preorder(input, &preorder);

    std::vector<SerializableObject::Retainer<Marker>> output_markers;

    for (Item* owner: preorder)
    {
        if (owner->markers().empty())
        {
            continue;
        }

        TimeRange owner_trimmed = owner->trimmed_range(error_status);
        if (is_error(error_status))
        {
            return nullptr;
        }

        // Visible coverage of this object's descendant leaves, expressed in
        // the object's local coordinate system.
        std::vector<_RF_Interval> coverage;
        for (auto const& segment: merged)
        {
            if (!_rf_is_descendant(segment.clip, owner))
            {
                continue;
            }
            TimeRange in_owner = _rf_map_range_up(
                segment.clip,
                owner,
                segment.source,
                error_status);
            if (is_error(error_status))
            {
                return nullptr;
            }
            coverage.push_back(
                { in_owner.start_time(), in_owner.end_time_exclusive() });
        }
        _rf_merge_intervals(&coverage);

        for (auto const& marker: owner->markers())
        {
            TimeRange marked = marker->marked_range();
            bool is_point = marked.duration().value() == 0;

            std::vector<TimeRange> pieces;

            if (is_point)
            {
                RationalTime point = marked.start_time();
                if (_rf_less(point, owner_trimmed.start_time())
                    || !_rf_less(point, owner_trimmed.end_time_exclusive()))
                {
                    continue;
                }
                bool found = false;
                for (auto const& interval: coverage)
                {
                    if (!_rf_less(point, interval.start)
                        && _rf_less(point, interval.end))
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    continue;
                }
                pieces.push_back(TimeRange(point));
            }
            else
            {
                TimeRange trimmed_marker;
                if (!_rf_intersect(marked, owner_trimmed, &trimmed_marker))
                {
                    continue;
                }
                for (auto const& interval: coverage)
                {
                    TimeRange piece;
                    if (_rf_intersect(
                            trimmed_marker,
                            TimeRange(
                                interval.start,
                                interval.end - interval.start),
                            &piece))
                    {
                        pieces.push_back(piece);
                    }
                }
            }

            for (auto const& piece: pieces)
            {
                TimeRange root_range = (owner == input)
                                           ? piece
                                           : _rf_map_range_up(
                                                 owner,
                                                 input,
                                                 piece,
                                                 error_status);
                if (is_error(error_status))
                {
                    return nullptr;
                }

                TimeRange program_range(
                    root_range.start_time() - season_start,
                    root_range.duration());

                SerializableObject::Retainer<Marker> marker_clone(
                    dynamic_cast<Marker*>(marker->clone(error_status)));
                if (is_error(error_status) || !marker_clone)
                {
                    if (!is_error(error_status) && error_status)
                    {
                        *error_status = ErrorStatus(
                            ErrorStatus::INTERNAL_ERROR,
                            "failed to clone marker in flatten_stack_recursive");
                    }
                    return nullptr;
                }
                marker_clone->set_marked_range(program_range);
                output_markers.push_back(marker_clone);
            }
        }
    }

    std::stable_sort(
        output_markers.begin(),
        output_markers.end(),
        [](SerializableObject::Retainer<Marker> const& a,
           SerializableObject::Retainer<Marker> const& b) {
            return _rf_less(
                a->marked_range().start_time(),
                b->marked_range().start_time());
        });

    for (auto& marker: output_markers)
    {
        flat_track->markers().push_back(marker);
    }

    return flat_track.take_value();
}
}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS
