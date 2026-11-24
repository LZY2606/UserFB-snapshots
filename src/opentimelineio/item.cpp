// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#include "opentimelineio/item.h"
#include "opentimelineio/composition.h"
#include "opentimelineio/effect.h"
#include "opentimelineio/marker.h"

#include <algorithm>
#include <assert.h>

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {

Item::Item(
    std::string const&              name,
    std::optional<TimeRange> const& source_range,
    AnyDictionary const&            metadata,
    std::vector<Effect*> const&     effects,
    std::vector<Marker*> const&     markers,
    bool                            enabled,
    std::optional<Color> const&     color)
    : Parent(name, metadata)
    , _source_range(source_range)
    , _effects(effects.begin(), effects.end())
    , _markers(markers.begin(), markers.end())
    , _color(color)
    , _enabled(enabled)
{}

Item::~Item()
{}

bool
Item::visible() const
{
    return _enabled;
}

bool
Item::overlapping() const
{
    return false;
}

RationalTime
Item::duration(ErrorStatus* error_status) const
{
    return trimmed_range(error_status).duration();
}

TimeRange
Item::available_range(ErrorStatus* error_status) const
{
    if (error_status)
    {
        *error_status = ErrorStatus::NOT_IMPLEMENTED;
    }
    return TimeRange();
}

TimeRange
Item::visible_range(ErrorStatus* error_status) const
{
    TimeRange result = trimmed_range(error_status);
    if (parent() && !is_error(error_status))
    {
        auto head_tail = parent()->handles_of_child(this, error_status);
        if (is_error(error_status))
        {
            return result;
        }
        if (head_tail.first)
        {
            result = TimeRange(
                result.start_time() - *head_tail.first,
                result.duration() + *head_tail.first);
        }
        if (head_tail.second)
        {
            result = TimeRange(
                result.start_time(),
                result.duration() + *head_tail.second);
        }
    }
    return result;
}

std::optional<TimeRange>
Item::trimmed_range_in_parent(ErrorStatus* error_status) const
{
    if (!parent())
    {
        if (error_status)
        {
            *error_status                = ErrorStatus::NOT_A_CHILD;
            error_status->object_details = this;
        }
        return std::nullopt;
    }

    return parent()->trimmed_range_of_child(this, error_status);
}

TimeRange
Item::range_in_parent(ErrorStatus* error_status) const
{
    if (!parent())
    {
        if (error_status)
        {
            *error_status                = ErrorStatus::NOT_A_CHILD;
            error_status->object_details = this;
        }
        return TimeRange();
    }

    return parent()->range_of_child(this, error_status);
}

RationalTime
Item::transformed_time(
    RationalTime time,
    Item const*  to_item,
    ErrorStatus* error_status) const
{
    if (!to_item)
    {
        return time;
    }

    auto root   = _highest_ancestor();
    auto item   = this;
    auto result = time;

    // Walk up from this item to the common ancestor, expressing the time in
    // each parent's coordinate system.  This visits the retiming hook on the
    // way out, which is the correct innermost-first order.
    while (item != root && item != to_item)
    {
        auto parent = item->parent();

        result = item->_local_time_to_parent_time(result, error_status);
        if (is_error(error_status))
        {
            return result;
        }

        item = parent;
    }

    if (item == to_item)
    {
        return result;
    }

    auto ancestor = item;

    // Descend from the common ancestor into to_item.  Collect the chain and
    // apply the conversions outermost-first so any scale factors compose in
    // the same order as the forward walk.
    std::vector<Item const*> descent;
    for (auto cur = to_item; cur != ancestor; cur = cur->parent())
    {
        if (!cur)
        {
            // to_item is not in the same hierarchy as this item.
            if (error_status)
            {
                *error_status                = ErrorStatus::NOT_DESCENDED_FROM;
                error_status->object_details = this;
            }
            return result;
        }
        descent.push_back(cur);
    }

    for (auto it = descent.rbegin(); it != descent.rend(); ++it)
    {
        result = (*it)->_parent_time_to_local_time(result, error_status);
        if (is_error(error_status))
        {
            return result;
        }
    }

    return result;
}

TimeRange
Item::transformed_time_range(
    TimeRange    time_range,
    Item const*  to_item,
    ErrorStatus* error_status) const
{
    RationalTime start = transformed_time(
        time_range.start_time(),
        to_item,
        error_status);
    if (is_error(error_status))
    {
        return TimeRange(start, time_range.duration());
    }

    RationalTime end = transformed_time(
        time_range.end_time_exclusive(),
        to_item,
        error_status);
    if (is_error(error_status))
    {
        return TimeRange(start, time_range.duration());
    }

    // Both boundaries are transformed independently, which can reverse their
    // order when an item plays backwards.  Rearrange so the result always has
    // a non-negative duration.
    RationalTime range_start = std::min(start, end);
    RationalTime range_end   = std::max(start, end);
    return TimeRange::range_from_start_end_time(range_start, range_end);
}

RationalTime
Item::_local_time_to_parent_time(
    RationalTime const& time,
    ErrorStatus*        error_status) const
{
    auto parent = this->parent();
    if (!parent)
    {
        return time;
    }

    auto parent_range = parent->range_of_child(this, error_status);
    if (is_error(error_status))
    {
        return time;
    }

    return (time - trimmed_range(error_status).start_time())
           + parent_range.start_time();
}

RationalTime
Item::_parent_time_to_local_time(
    RationalTime const& time,
    ErrorStatus*        error_status) const
{
    auto parent = this->parent();
    if (!parent)
    {
        return time;
    }

    auto parent_range = parent->range_of_child(this, error_status);
    if (is_error(error_status))
    {
        return time;
    }

    return (time - parent_range.start_time())
           + trimmed_range(error_status).start_time();
}

void
Item::trim_source_range_in_parent_time(
    RationalTime head_trim,
    RationalTime tail_trim,
    ErrorStatus* error_status)
{
    TimeRange trimmed = trimmed_range(error_status);
    if (is_error(error_status))
    {
        return;
    }

    set_source_range(TimeRange(
        trimmed.start_time() + head_trim,
        trimmed.duration() - head_trim - tail_trim));
}

bool
Item::read_from(Reader& reader)
{
    return reader.read_if_present("source_range", &_source_range)
           && reader.read_if_present("effects", &_effects)
           && reader.read_if_present("markers", &_markers)
           && reader.read_if_present("enabled", &_enabled)
           && reader.read_if_present("color", &_color)
           && Parent::read_from(reader);
}

void
Item::write_to(Writer& writer) const
{
    Parent::write_to(writer);
    writer.write("source_range", _source_range);
    writer.write("effects", _effects);
    writer.write("markers", _markers);
    writer.write("enabled", _enabled);
    writer.write("color", _color);
}

}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS
