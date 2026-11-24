// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#include "opentimelineio/retimedClip.h"
#include "opentimelineio/composition.h"

#include <cmath>

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {

RetimedClip::RetimedClip(
    std::string const&              name,
    MediaReference*                 media_reference,
    std::optional<TimeRange> const& source_range,
    double                          playback_rate,
    AnyDictionary const&            metadata,
    std::vector<Effect*> const&     effects,
    std::vector<Marker*> const&     markers,
    std::string const&              active_media_reference_key,
    std::optional<Color> const&     color)
    : Parent(
          name,
          media_reference,
          source_range,
          metadata,
          effects,
          markers,
          active_media_reference_key,
          color)
    , _playback_rate(_valid_playback_rate(playback_rate) ? playback_rate : 1.0)
{}

RetimedClip::~RetimedClip()
{}

bool
RetimedClip::_valid_playback_rate(double playback_rate) noexcept
{
    return std::isfinite(playback_rate) && playback_rate != 0.0;
}

bool
RetimedClip::set_playback_rate(
    double      playback_rate,
    ErrorStatus* error_status)
{
    if (!_valid_playback_rate(playback_rate))
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::INVALID_TIME_RANGE,
                "playback_rate must be a finite, non-zero number",
                this);
        }
        return false;
    }

    _playback_rate = playback_rate;
    return true;
}

RationalTime
RetimedClip::duration(ErrorStatus* error_status) const
{
    TimeRange trimmed = trimmed_range(error_status);
    if (is_error(error_status))
    {
        return RationalTime();
    }

    double const factor = 1.0 / std::fabs(_playback_rate);
    return RationalTime(
        trimmed.duration().value() * factor,
        trimmed.duration().rate());
}

RationalTime
RetimedClip::_local_time_to_parent_time(
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

    TimeRange source = trimmed_range(error_status);
    if (is_error(error_status))
    {
        return time;
    }

    // Forward playback anchors the source start at the parent start; reverse
    // playback anchors the exclusive source end at the parent start.
    RationalTime const anchor = _playback_rate > 0
                                    ? source.start_time()
                                    : source.end_time_exclusive();

    return parent_range.start_time()
           + _scaled_by(time - anchor, 1.0 / _playback_rate);
}

RationalTime
RetimedClip::_parent_time_to_local_time(
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

    TimeRange source = trimmed_range(error_status);
    if (is_error(error_status))
    {
        return time;
    }

    RationalTime const anchor = _playback_rate > 0
                                    ? source.start_time()
                                    : source.end_time_exclusive();

    return anchor
           + _scaled_by(
               time - parent_range.start_time(),
               _playback_rate);
}

void
RetimedClip::trim_source_range_in_parent_time(
    RationalTime head_trim,
    RationalTime tail_trim,
    ErrorStatus* error_status)
{
    TimeRange source = trimmed_range(error_status);
    if (is_error(error_status))
    {
        return;
    }

    double const   magnitude = std::fabs(_playback_rate);
    RationalTime   start     = source.start_time();
    RationalTime   end       = source.end_time_exclusive();
    RationalTime   head_src  = _scaled_by(head_trim, magnitude);
    RationalTime   tail_src  = _scaled_by(tail_trim, magnitude);

    if (_playback_rate > 0)
    {
        // Program head trims the source start; program tail trims the end.
        start = start + head_src;
        end   = end - tail_src;
    }
    else
    {
        // Reversed: the program head is played from the source end, so it
        // trims the end; the program tail trims the source start.
        end   = end - head_src;
        start = start + tail_src;
    }

    set_source_range(TimeRange::range_from_start_end_time(start, end));
}

TimeRange
RetimedClip::visible_range(ErrorStatus* error_status) const
{
    TimeRange source = trimmed_range(error_status);
    if (is_error(error_status))
    {
        return source;
    }

    auto parent = this->parent();
    if (!parent)
    {
        return source;
    }

    auto head_tail = parent->handles_of_child(this, error_status);
    if (is_error(error_status))
    {
        return source;
    }

    double const magnitude = std::fabs(_playback_rate);

    std::optional<RationalTime> head;
    std::optional<RationalTime> tail;
    if (head_tail.first)
    {
        head = _scaled_by(*head_tail.first, magnitude);
    }
    if (head_tail.second)
    {
        tail = _scaled_by(*head_tail.second, magnitude);
    }

    // The handles are given in program time.  Convert them to source lengths,
    // and for reverse playback swap which source boundary they extend.
    std::optional<RationalTime> start_extension;
    std::optional<RationalTime> end_extension;
    if (_playback_rate > 0)
    {
        start_extension = head;
        end_extension   = tail;
    }
    else
    {
        start_extension = tail;
        end_extension   = head;
    }

    RationalTime start = source.start_time();
    RationalTime end   = source.end_time_exclusive();
    if (start_extension)
    {
        start = start - *start_extension;
    }
    if (end_extension)
    {
        end = end + *end_extension;
    }

    return TimeRange::range_from_start_end_time(start, end);
}

bool
RetimedClip::read_from(Reader& reader)
{
    bool ok = reader.read_if_present("playback_rate", &_playback_rate);
    if (!ok)
    {
        return false;
    }

    if (!_valid_playback_rate(_playback_rate))
    {
        reader.error(ErrorStatus(
            ErrorStatus::MALFORMED_SCHEMA,
            "RetimedClip playback_rate must be a finite, non-zero number",
            this));
        return false;
    }

    return Parent::read_from(reader);
}

void
RetimedClip::write_to(Writer& writer) const
{
    Parent::write_to(writer);
    writer.write("playback_rate", _playback_rate);
}

}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS
