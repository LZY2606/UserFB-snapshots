// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#include "opentimelineio/trackAlgorithm.h"
#include "opentimelineio/transition.h"

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {

Track*
track_trimmed_to_range(
    Track*       in_track,
    TimeRange    trim_range,
    ErrorStatus* error_status)
{
    Track* new_track = dynamic_cast<Track*>(in_track->clone(error_status));
    if (is_error(error_status) || !new_track)
    {
        return nullptr;
    }

    auto track_map = new_track->range_of_all_children(error_status);
    if (is_error(error_status))
    {
        return nullptr;
    }

    std::vector<Composable*> children_copy(
        new_track->children().begin(),
        new_track->children().end());

    for (size_t i = children_copy.size(); i--;)
    {
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
        }
        else if (!trim_range.contains(child_range))
        {
            if (dynamic_cast<Transition*>(child))
            {
                if (error_status)
                {
                    *error_status = ErrorStatus(
                        ErrorStatus::CANNOT_TRIM_TRANSITION,
                        "Cannot trim in the middle of a transition");
                }
                return nullptr;
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

            RationalTime head_trim(0, child_source_range.duration().rate());
            RationalTime tail_trim(0, child_source_range.duration().rate());

            if (trim_range.start_time() > child_range.start_time())
            {
                head_trim = trim_range.start_time() - child_range.start_time();
            }

            auto trim_end  = trim_range.end_time_exclusive();
            auto child_end = child_range.end_time_exclusive();
            if (trim_end < child_end)
            {
                tail_trim = child_end - trim_end;
            }

            // Let the item convert program-time trims into source-time trims;
            // the default is 1:1, retimed clips scale by the playback rate.
            child_item->trim_source_range_in_parent_time(
                head_trim,
                tail_trim,
                error_status);
            if (is_error(error_status))
            {
                return nullptr;
            }
        }
    }

    return new_track;
}

}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS
