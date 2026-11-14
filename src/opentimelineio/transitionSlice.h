// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#pragma once

#include "opentimelineio/clip.h"
#include "opentimelineio/item.h"
#include "opentimelineio/version.h"

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {

/// @brief The remaining portion of a dissolve transition after a trim cut
/// into the middle of it.
///
/// A TransitionSlice behaves like a regular Item on the timeline: it
/// occupies actual program time (its source_range duration). Its
/// source_range addresses the timeline of the *complete* dissolve it was
/// cut from, so sampling always uses the linear progress of the full
/// dissolve rather than restarting the blend from zero.
///
/// The slice keeps private deep-copy snapshots of the two participating
/// clips, the full dissolve length, and the media time of each participant
/// at the start of the full dissolve.
class OTIO_API_TYPE TransitionSlice : public Item
{
public:
    /// @brief This struct provides the TransitionSlice schema.
    struct Schema
    {
        static auto constexpr name   = "TransitionSlice";
        static int constexpr version = 1;
    };

    using Parent = Item;

    /// @brief A single sample of the preserved blend.
    struct Sample
    {
        RationalTime outgoing_time;   ///< Media time of the outgoing clip.
        RationalTime incoming_time;   ///< Media time of the incoming clip.
        double       incoming_weight; ///< Linear blend weight of the incoming clip, in [0, 1].
    };

    /// @brief Create a new transition slice.
    ///
    /// This constructor can natively describe a complete dissolve: pass the
    /// two participating clips, the full dissolve length and the media time
    /// of each side at the start of the full dissolve. If source_range is
    /// not given, the slice covers the complete dissolve.
    ///
    /// @param name The name of the slice.
    /// @param outgoing Snapshot of the outgoing clip. Note that the slice
    /// keeps a retainer to the clip.
    /// @param incoming Snapshot of the incoming clip. Note that the slice
    /// keeps a retainer to the clip.
    /// @param full_duration The duration of the complete dissolve. Must be
    /// positive for sampling to be valid.
    /// @param outgoing_start Media time of the outgoing clip at the start
    /// of the complete dissolve.
    /// @param incoming_start Media time of the incoming clip at the start
    /// of the complete dissolve.
    /// @param source_range The kept interval on the timeline of the
    /// complete dissolve. Defaults to the complete dissolve.
    /// @param metadata The metadata for the slice.
    OTIO_API TransitionSlice(
        std::string const&              name           = std::string(),
        Clip*                           outgoing       = nullptr,
        Clip*                           incoming       = nullptr,
        RationalTime                    full_duration  = RationalTime(),
        RationalTime                    outgoing_start = RationalTime(),
        RationalTime                    incoming_start = RationalTime(),
        std::optional<TimeRange> const& source_range   = std::nullopt,
        AnyDictionary const&            metadata       = AnyDictionary());

    /// @brief Return the snapshot of the outgoing clip.
    Clip* outgoing() const noexcept { return _outgoing; }

    /// @brief Return the snapshot of the incoming clip.
    Clip* incoming() const noexcept { return _incoming; }

    /// @brief Return the duration of the complete dissolve.
    RationalTime full_duration() const noexcept { return _full_duration; }

    /// @brief Return the media time of the outgoing clip at the start of
    /// the complete dissolve.
    RationalTime outgoing_start() const noexcept { return _outgoing_start; }

    /// @brief Return the media time of the incoming clip at the start of
    /// the complete dissolve.
    RationalTime incoming_start() const noexcept { return _incoming_start; }

    /// @brief The available range is the complete dissolve.
    OTIO_API TimeRange
    available_range(ErrorStatus* error_status = nullptr) const override;

    /// @brief Sample the preserved blend.
    ///
    /// @param elapsed Time from the zero point of the currently visible
    /// (kept) portion. Must lie in [0, duration()).
    /// @param error_status Receives an error if the full dissolve length is
    /// not positive or elapsed is out of range.
    /// @return The media times of both participants (running at normal
    /// speed) and the incoming blend weight, which follows the linear
    /// progress of the complete dissolve.
    OTIO_API Sample sample_at(
        RationalTime elapsed,
        ErrorStatus* error_status = nullptr) const;

protected:
    virtual ~TransitionSlice();

    bool read_from(Reader&) override;
    void write_to(Writer&) const override;

private:
    Retainer<Clip> _outgoing;
    Retainer<Clip> _incoming;
    RationalTime   _full_duration;
    RationalTime   _outgoing_start;
    RationalTime   _incoming_start;
};

}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS
