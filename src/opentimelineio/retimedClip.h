// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#pragma once

#include "opentimelineio/clip.h"
#include "opentimelineio/version.h"

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {

/// @brief A clip that participates in editorial layout at a playback rate.
///
/// Unlike a Clip carrying a LinearTimeWarp effect (which only describes how a
/// renderer should sample the media), a RetimedClip changes the duration the
/// clip occupies in its parent.  Its source_range is still a half-open interval
/// in the media, but its duration() is the source length divided by the
/// absolute value of the playback rate.
///
/// A positive playback rate plays forward from the source start; a negative
/// playback rate plays backward from the exclusive source end.  Time mapping
/// is defined on continuous boundaries, so fractional rates and mixed time
/// rates are preserved without rounding.
class OTIO_API_TYPE RetimedClip : public Clip
{
public:
    /// @brief This struct provides the RetimedClip schema.
    struct Schema
    {
        static auto constexpr name   = "RetimedClip";
        static int constexpr version = 1;
    };

    using Parent = Clip;

    /// @brief Create a new retimed clip.
    ///
    /// @param name The name of the clip.
    /// @param media_reference The media reference for the clip.
    /// @param source_range The half-open source range in the media.
    /// @param playback_rate The playback rate.  Must be finite and non-zero.
    /// @param metadata The metadata for the clip.
    /// @param effects The list of effects for the clip.
    /// @param markers The list of markers for the clip.
    /// @param active_media_reference_key The active media reference.
    /// @param color The item color.
    OTIO_API RetimedClip(
        std::string const&              name            = std::string(),
        MediaReference*                 media_reference = nullptr,
        std::optional<TimeRange> const& source_range    = std::nullopt,
        double                          playback_rate   = 1.0,
        AnyDictionary const&            metadata        = AnyDictionary(),
        std::vector<Effect*> const&     effects       = std::vector<Effect*>(),
        std::vector<Marker*> const&     markers       = std::vector<Marker*>(),
        std::string const& active_media_reference_key = Clip::default_media_key,
        std::optional<Color> const& color             = std::nullopt);

    /// @brief Return the playback rate.
    double playback_rate() const noexcept { return _playback_rate; }

    /// @brief Set the playback rate.
    ///
    /// Only finite, non-zero values are accepted.  On failure the previous
    /// value is retained, the error status is filled in (when non-null), and
    /// false is returned.
    OTIO_API bool
    set_playback_rate(double playback_rate, ErrorStatus* error_status = nullptr);

    /// @brief Return the duration in the parent: source length / |rate|.
    OTIO_API RationalTime
    duration(ErrorStatus* error_status = nullptr) const override;

    /// @brief Return the visible range in source coordinates.
    ///
    /// Handles supplied by the parent are measured in program time and are
    /// converted to source lengths.  For a reversed clip the head and tail
    /// directions are swapped.
    OTIO_API TimeRange
    visible_range(ErrorStatus* error_status = nullptr) const override;

protected:
    virtual ~RetimedClip();

    bool read_from(Reader&) override;
    void write_to(Writer&) const override;

    RationalTime _local_time_to_parent_time(
        RationalTime const& time,
        ErrorStatus*        error_status = nullptr) const override;

    RationalTime _parent_time_to_local_time(
        RationalTime const& time,
        ErrorStatus*        error_status = nullptr) const override;

    void trim_source_range_in_parent_time(
        RationalTime head_trim,
        RationalTime tail_trim,
        ErrorStatus* error_status = nullptr) override;

private:
    /// @brief Validate a candidate playback rate.
    static bool _valid_playback_rate(double playback_rate) noexcept;

    /// @brief Scale a time's value by factor, preserving its rate.
    static RationalTime
    _scaled_by(RationalTime const& time, double factor) noexcept
    {
        return RationalTime(time.value() * factor, time.rate());
    }

    double _playback_rate;
};

}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS
