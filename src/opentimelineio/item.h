// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#pragma once

#include "opentime/timeRange.h"
#include "opentimelineio/color.h"
#include "opentimelineio/composable.h"
#include "opentimelineio/errorStatus.h"
#include "opentimelineio/version.h"

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {

class Effect;
class Marker;

/// @brief An item in the timeline.
class OTIO_API_TYPE Item : public Composable
{
public:
    /// @brief This struct provides the Item schema.
    struct Schema
    {
        static auto constexpr name   = "Item";
        static int constexpr version = 1;
    };

    using Parent = Composable;

    /// @brief Create a new item.
    ///
    /// @param name The name of the item.
    /// @param source_range The source range of the item.
    /// @param metadata The metadata for the item.
    /// @param effects The list of effects for the item. Note that the
    /// the item keeps a retainer to each effect.
    /// @param markers The list of markers for the item. Note that the
    /// the item keeps a retainer to each marker.
    /// @param enabled Whether the item is enabled.
    OTIO_API Item(
        std::string const&              name         = std::string(),
        std::optional<TimeRange> const& source_range = std::nullopt,
        AnyDictionary const&            metadata     = AnyDictionary(),
        std::vector<Effect*> const&     effects      = std::vector<Effect*>(),
        std::vector<Marker*> const&     markers      = std::vector<Marker*>(),
        bool                            enabled      = true,
        std::optional<Color> const&     color        = std::nullopt);

    bool visible() const override;
    bool overlapping() const override;

    /// @brief Return whether the item is enabled.
    bool enabled() const { return _enabled; };

    /// @brief Set whether the item is enabled.
    void set_enabled(bool enabled) { _enabled = enabled; }

    /// @brief Return the source range of the item.
    std::optional<TimeRange> source_range() const noexcept
    {
        return _source_range;
    }

    /// @brief Set the source range of the item.
    void set_source_range(std::optional<TimeRange> const& source_range)
    {
        _source_range = source_range;
    }

    /// @brief Modify the list of effects.
    std::vector<Retainer<Effect>>& effects() noexcept { return _effects; }

    /// @brief Return the list of effects.
    std::vector<Retainer<Effect>> const& effects() const noexcept
    {
        return _effects;
    }

    /// @brief Modify the list of markers.
    std::vector<Retainer<Marker>>& markers() noexcept { return _markers; }

    /// @brief Return the list of markers.
    std::vector<Retainer<Marker>> const& markers() const noexcept
    {
        return _markers;
    }

    RationalTime duration(ErrorStatus* error_status = nullptr) const override;

    /// @brief Return the available range of the item.
    virtual TimeRange
    available_range(ErrorStatus* error_status = nullptr) const;

    /// @brief Return the trimmed range of the item.
    TimeRange trimmed_range(ErrorStatus* error_status = nullptr) const
    {
        return _source_range ? *_source_range : available_range(error_status);
    }

    /// @brief Return the visible range of the item.
    OTIO_API virtual TimeRange
    visible_range(ErrorStatus* error_status = nullptr) const;

    /// @brief Return the trimmed range of the item in the parent's time.
    OTIO_API std::optional<TimeRange>
             trimmed_range_in_parent(ErrorStatus* error_status = nullptr) const;

    /// @brief Return the range of the item in the parent's time.
    OTIO_API TimeRange
    range_in_parent(ErrorStatus* error_status = nullptr) const;

    /// @brief Return the time transformed to another item in the hierarchy.
    OTIO_API RationalTime transformed_time(
        RationalTime time,
        Item const*  to_item,
        ErrorStatus* error_status = nullptr) const;

    /// @brief Return the time range transformed to another item in the hierarchy.
    ///
    /// Both endpoints of the range are transformed and the result is
    /// arranged so that it always has a non-negative duration.  This makes
    /// the operation well defined for items that play backwards.
    OTIO_API TimeRange transformed_time_range(
        TimeRange    time_range,
        Item const*  to_item,
        ErrorStatus* error_status = nullptr) const;

    /// @brief Trim this item's source range by amounts measured in the
    /// parent's time.
    ///
    /// The default implementation assumes that one unit of parent time is one
    /// unit of source time (a 1:1 mapping).  Items that retime their media
    /// override this to convert the parent-time amounts into source lengths.
    ///
    /// @param head_trim The amount to remove from the start of the item in the
    /// parent's time.
    /// @param tail_trim The amount to remove from the end of the item in the
    /// parent's time.
    /// @param error_status The return status.
    OTIO_API virtual void trim_source_range_in_parent_time(
        RationalTime head_trim,
        RationalTime tail_trim,
        ErrorStatus* error_status = nullptr);

    std::optional<Color> color() const noexcept { return _color; }

    /// @brief Set the color of the item.
    void set_color(std::optional<Color> const& color) { _color = color; }

protected:
    virtual ~Item();

    bool read_from(Reader&) override;
    void write_to(Writer&) const override;

    /// @brief Convert a time in this item's local (source) coordinate system
    /// into its parent's coordinate system.
    ///
    /// The default implementation is a 1:1 shift, which is correct for items
    /// that play their media at speed 1.  Retiming items override this (and
    /// _parent_time_to_local_time()) to provide their actual mapping.
    OTIO_API virtual RationalTime _local_time_to_parent_time(
        RationalTime const& time,
        ErrorStatus*        error_status = nullptr) const;

    /// @brief Convert a time in the parent's coordinate system into this
    /// item's local (source) coordinate system.
    ///
    /// This must be the inverse of _local_time_to_parent_time().
    OTIO_API virtual RationalTime _parent_time_to_local_time(
        RationalTime const& time,
        ErrorStatus*        error_status = nullptr) const;

private:
    std::optional<TimeRange>      _source_range;
    std::vector<Retainer<Effect>> _effects;
    std::vector<Retainer<Marker>> _markers;
    std::optional<Color>          _color;
    bool                          _enabled;
};

}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS
