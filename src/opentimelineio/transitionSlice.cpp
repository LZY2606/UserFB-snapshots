// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#include "opentimelineio/transitionSlice.h"

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {

TransitionSlice::TransitionSlice(
    std::string const&              name,
    Clip*                           outgoing,
    Clip*                           incoming,
    RationalTime                    full_duration,
    RationalTime                    outgoing_start,
    RationalTime                    incoming_start,
    std::optional<TimeRange> const& source_range,
    AnyDictionary const&            metadata)
    : Parent(
          name,
          source_range
              ? source_range
              : std::optional<TimeRange>(TimeRange(
                    RationalTime(0, full_duration.rate()), full_duration)),
          metadata)
    , _outgoing(outgoing)
    , _incoming(incoming)
    , _full_duration(full_duration)
    , _outgoing_start(outgoing_start)
    , _incoming_start(incoming_start)
{}

TransitionSlice::~TransitionSlice()
{}

TimeRange
TransitionSlice::available_range(ErrorStatus* /* error_status */) const
{
    return TimeRange(
        RationalTime(0, _full_duration.rate()), _full_duration);
}

TransitionSlice::Sample
TransitionSlice::sample_at(RationalTime elapsed, ErrorStatus* error_status)
    const
{
    Sample result{ RationalTime(), RationalTime(), 0.0 };

    if (_full_duration.is_invalid_time() || _full_duration.value() <= 0)
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::INVALID_TIME_RANGE,
                "TransitionSlice requires a positive full dissolve duration "
                "to be sampled",
                this);
        }
        return result;
    }

    const TimeRange visible = trimmed_range(error_status);
    if (is_error(error_status))
    {
        return result;
    }

    const RationalTime zero(0, elapsed.rate());
    if (elapsed < zero || elapsed >= visible.duration())
    {
        if (error_status)
        {
            *error_status = ErrorStatus(
                ErrorStatus::INVALID_TIME_RANGE,
                "TransitionSlice sample elapsed time is outside "
                "[0, duration)",
                this);
        }
        return result;
    }

    // Position on the timeline of the complete dissolve; both sides run
    // at normal speed from their media time at the full dissolve start.
    const RationalTime full_elapsed = visible.start_time() + elapsed;

    result.outgoing_time = _outgoing_start + full_elapsed;
    result.incoming_time = _incoming_start + full_elapsed;
    result.incoming_weight =
        full_elapsed.value_rescaled_to(_full_duration.rate())
        / _full_duration.value();
    return result;
}

bool
TransitionSlice::read_from(Reader& reader)
{
    return reader.read("outgoing", &_outgoing)
           && reader.read("incoming", &_incoming)
           && reader.read("full_duration", &_full_duration)
           && reader.read("outgoing_start", &_outgoing_start)
           && reader.read("incoming_start", &_incoming_start)
           && Parent::read_from(reader);
}

void
TransitionSlice::write_to(Writer& writer) const
{
    Parent::write_to(writer);
    writer.write("outgoing", _outgoing);
    writer.write("incoming", _incoming);
    writer.write("full_duration", _full_duration);
    writer.write("outgoing_start", _outgoing_start);
    writer.write("incoming_start", _incoming_start);
}

}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS
