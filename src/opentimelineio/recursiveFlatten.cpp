// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#include "opentimelineio/stackAlgorithm.h"

#include "opentimelineio/clip.h"
#include "opentimelineio/effect.h"
#include "opentimelineio/gap.h"
#include "opentimelineio/marker.h"
#include "opentimelineio/timeEffect.h"
#include "opentimelineio/transition.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {

namespace {

// Timing tolerance, in seconds.  All geometry is kept as RationalTime-derived
// second values and is never quantized to frames; this epsilon only absorbs
// floating point noise when the same conceptual boundary is reached through
// different arithmetic paths.
constexpr double TIME_EPSILON_S = 1.0e-10;

struct NodeInfo
{
    // Program (output track) coordinates of the portion of this item that
    // survives every ancestor source_range trim.
    double window_start_s = 0.0;
    double window_end_s   = 0.0;

    // Program time corresponding to local time zero on this item.
    double origin_s = 0.0;
};

struct LeafSegment
{
    Clip*                  clip = nullptr;
    double                 start_s = 0.0;
    double                 end_s   = 0.0;
    double                 origin_s = 0.0;
    double                 media_rate = 1.0;
    size_t                 order = 0;
    std::vector<uint32_t>  stack_path;
    std::vector<Item*>     chain;
};

struct MarkerRecord
{
    Item*   owner  = nullptr;
    Marker* marker = nullptr;
    size_t  order  = 0;
};

struct WinRun
{
    double       start_s = 0.0;
    double       end_s   = 0.0;
    LeafSegment* leaf    = nullptr; // nullptr means transparent
};

struct FlattenContext
{
    ErrorStatus*             error_status = nullptr;
    std::map<Item*, NodeInfo> nodes;
    std::vector<LeafSegment>  leaves;
    std::vector<MarkerRecord> markers;
    size_t                    order_counter = 0;
    bool                      failed = false;
};

static bool
_time_is_finite(RationalTime t)
{
    return std::isfinite(t.value()) && std::isfinite(t.rate())
           && t.rate() > 0.0;
}

// Rebuild a RationalTime at `rate` from a second value.  Times that are
// algebraically whole frames at this rate can pick up tiny floating point
// noise from the second-based geometry; collapse that noise back to the exact
// frame value.  Genuinely fractional (e.g. cross-rate) results are left
// untouched, so nothing is quantized to frames during rate conversion.
static RationalTime
_time_at_rate(double seconds, double rate)
{
    double value = seconds * rate;
    double nearest = std::round(value);
    if (std::fabs(value - nearest) < 1.0e-7)
    {
        value = nearest;
    }
    return RationalTime(value, rate);
}

static void
_fail(FlattenContext* ctx, ErrorStatus::Outcome outcome, std::string const& msg)
{
    if (!ctx->failed && ctx->error_status)
    {
        *ctx->error_status = ErrorStatus(outcome, msg);
    }
    ctx->failed = true;
}

// Recursively validate the tree and collect leaf picture segments, marker
// records and per-node timing info.
//
// placement_s is the program time at which this item's visible (trimmed)
// content starts, i.e. where its local source_range start maps to.  The
// ancestor trim window [ancestor_start_s, ancestor_end_s) is already in
// program coordinates.
static bool
_collect(
    FlattenContext*           ctx,
    Composable*               node,
    double                    placement_s,
    double                    ancestor_start_s,
    double                    ancestor_end_s,
    std::vector<uint32_t>     stack_path,
    std::vector<Item*>        chain)
{
    if (ctx->failed)
    {
        return false;
    }

    if (dynamic_cast<Transition*>(node))
    {
        _fail(
            ctx,
            ErrorStatus::TYPE_MISMATCH,
            "flatten_stack_recursive: transitions are not supported");
        return false;
    }

    Item* item = dynamic_cast<Item*>(node);
    if (!item)
    {
        _fail(
            ctx,
            ErrorStatus::TYPE_MISMATCH,
            "flatten_stack_recursive: unsupported child type");
        return false;
    }

    for (auto const& effect: item->effects())
    {
        if (dynamic_cast<TimeEffect*>(effect.value))
        {
            _fail(
                ctx,
                ErrorStatus::TYPE_MISMATCH,
                "flatten_stack_recursive: time effects are not supported");
            return false;
        }
    }

    RationalTime duration = item->duration(ctx->error_status);
    if (is_error(ctx->error_status) || !_time_is_finite(duration)
        || duration.value() < 0.0)
    {
        _fail(
            ctx,
            ErrorStatus::INVALID_TIME_RANGE,
            "flatten_stack_recursive: invalid or negative item duration");
        return false;
    }

    RationalTime trim_start(0, duration.rate());
    RationalTime trim_end = duration;
    if (auto sr = item->source_range())
    {
        if (!_time_is_finite(sr->start_time())
            || !_time_is_finite(sr->duration())
            || sr->duration().value() < 0.0)
        {
            _fail(
                ctx,
                ErrorStatus::INVALID_TIME_RANGE,
                "flatten_stack_recursive: invalid source_range");
            return false;
        }
        trim_start = sr->start_time();
        trim_end   = trim_start + sr->duration();
    }

    for (auto const& marker_retainer: item->markers())
    {
        Marker* marker = marker_retainer.value;
        TimeRange const& mr = marker->marked_range();
        if (!_time_is_finite(mr.start_time())
            || !_time_is_finite(mr.duration()) || mr.duration().value() < 0.0)
        {
            _fail(
                ctx,
                ErrorStatus::INVALID_TIME_RANGE,
                "flatten_stack_recursive: invalid marker marked_range");
            return false;
        }
        ctx->markers.push_back(
            MarkerRecord{ item, marker, ctx->order_counter++ });
    }

    double const trim_start_s = trim_start.to_seconds();
    double const trim_end_s   = trim_end.to_seconds();

    // Local time zero maps to this program time.
    double const origin_s = placement_s - trim_start_s;

    double window_start_s =
        std::max(ancestor_start_s, placement_s);
    double window_end_s =
        std::min(ancestor_end_s, origin_s + trim_end_s);
    if (window_end_s < window_start_s)
    {
        window_end_s = window_start_s;
    }

    ctx->nodes[item] =
        NodeInfo{ window_start_s, window_end_s, origin_s };

    chain.push_back(item);

    // Disabled items contribute no picture; their descendants can never win.
    if (!item->enabled())
    {
        return true;
    }

    if (auto clip = dynamic_cast<Clip*>(item))
    {
        LeafSegment seg;
        seg.clip       = clip;
        seg.start_s    = window_start_s;
        seg.end_s      = window_end_s;
        seg.origin_s   = origin_s;
        seg.media_rate = duration.rate();
        seg.order      = ctx->order_counter++;
        seg.stack_path = std::move(stack_path);
        seg.chain      = std::move(chain);
        ctx->leaves.push_back(std::move(seg));
        return true;
    }

    if (dynamic_cast<Gap*>(item))
    {
        // Gaps are always transparent.
        return true;
    }

    if (auto track = dynamic_cast<Track*>(item))
    {
        // Children are laid out sequentially in the track's local time,
        // starting at local zero (which maps to the track's origin).
        double cursor_s = 0.0;
        for (auto const& child: track->children())
        {
            RationalTime child_duration =
                child->duration(ctx->error_status);
            if (is_error(ctx->error_status)
                || !_time_is_finite(child_duration)
                || child_duration.value() < 0.0)
            {
                _fail(
                    ctx,
                    ErrorStatus::INVALID_TIME_RANGE,
                    "flatten_stack_recursive: invalid child duration");
                return false;
            }

            // The child's visible content begins at the track's local time
            // `cursor`, which is program time origin_s + cursor.
            double child_placement_s = origin_s + cursor_s;
            if (!_collect(
                    ctx,
                    child.value,
                    child_placement_s,
                    window_start_s,
                    window_end_s,
                    stack_path,
                    chain))
            {
                return false;
            }
            cursor_s += child_duration.to_seconds();
        }
        return true;
    }

    if (auto stack = dynamic_cast<Stack*>(item))
    {
        // Every stack child starts at the stack's local zero (see
        // Stack::range_of_child_at_index); any trim on the child shifts the
        // child's own origin, which is handled inside _collect.  Later
        // children stack on top of earlier ones, so the child index extends
        // the priority path.
        uint32_t child_index = 0;
        for (auto const& child: stack->children())
        {
            std::vector<uint32_t> child_path = stack_path;
            child_path.push_back(child_index);
            if (!_collect(
                    ctx,
                    child.value,
                    origin_s,
                    window_start_s,
                    window_end_s,
                    std::move(child_path),
                    chain))
            {
                return false;
            }
            ++child_index;
        }
        return true;
    }

    _fail(
        ctx,
        ErrorStatus::TYPE_MISMATCH,
        "flatten_stack_recursive: unsupported item type");
    return false;
}

static bool
_path_greater(
    std::vector<uint32_t> const& lhs,
    std::vector<uint32_t> const& rhs)
{
    return std::lexicographical_compare(
        rhs.begin(), rhs.end(), lhs.begin(), lhs.end());
}

static LeafSegment*
_pick_winner(
    std::vector<LeafSegment>& leaves,
    double                    start_s,
    double                    end_s)
{
    LeafSegment* winner = nullptr;
    for (auto& leaf: leaves)
    {
        if (leaf.end_s - leaf.start_s <= TIME_EPSILON_S)
        {
            continue;
        }
        if (leaf.start_s > start_s + TIME_EPSILON_S
            || leaf.end_s < end_s - TIME_EPSILON_S)
        {
            continue;
        }
        if (!winner || _path_greater(leaf.stack_path, winner->stack_path)
            || (leaf.stack_path == winner->stack_path && leaf.order > winner->order))
        {
            winner = &leaf;
        }
    }
    return winner;
}

struct OutputMarker
{
    double                               start_s;
    SerializableObject::Retainer<Marker> marker;
};

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
                "flatten_stack_recursive: input stack is null");
        }
        return nullptr;
    }

    FlattenContext ctx;
    ctx.error_status = error_status;

    RationalTime output_duration = input->duration(error_status);
    if (is_error(error_status) || !_time_is_finite(output_duration)
        || output_duration.value() < 0.0)
    {
        if (error_status && !is_error(error_status))
        {
            *error_status = ErrorStatus(
                ErrorStatus::INVALID_TIME_RANGE,
                "flatten_stack_recursive: invalid input duration");
        }
        return nullptr;
    }

    RationalTime root_trim_start(0, output_duration.rate());
    if (auto sr = input->source_range())
    {
        if (!_time_is_finite(sr->start_time())
            || !_time_is_finite(sr->duration())
            || sr->duration().value() < 0.0)
        {
            if (error_status)
            {
                *error_status = ErrorStatus(
                    ErrorStatus::INVALID_TIME_RANGE,
                    "flatten_stack_recursive: invalid root source_range");
            }
            return nullptr;
        }
        root_trim_start = sr->start_time();
    }

    double const output_rate    = output_duration.rate();
    double const total_length_s = output_duration.to_seconds();

    // The output holds the root's trimmed content, rebased so its visible
    // start lands at output time zero.  Local time zero on the root therefore
    // maps to -root_trim_start.
    double const root_origin_s = -root_trim_start.to_seconds();

    ctx.nodes[input] = NodeInfo{
        0.0,
        total_length_s,
        root_origin_s
    };
    for (auto const& marker_retainer: input->markers())
    {
        Marker* marker = marker_retainer.value;
        TimeRange const& mr = marker->marked_range();
        if (!_time_is_finite(mr.start_time())
            || !_time_is_finite(mr.duration()) || mr.duration().value() < 0.0)
        {
            if (error_status)
            {
                *error_status = ErrorStatus(
                    ErrorStatus::INVALID_TIME_RANGE,
                    "flatten_stack_recursive: invalid marker marked_range");
            }
            return nullptr;
        }
        ctx.markers.push_back(
            MarkerRecord{ input, marker, ctx.order_counter++ });
    }

    {
        uint32_t child_index = 0;
        for (auto const& child: input->children())
        {
            std::vector<uint32_t> stack_path;
            stack_path.push_back(child_index);
            std::vector<Item*> chain;
            chain.push_back(input);
            if (!_collect(
                    &ctx,
                    child.value,
                    /*placement=*/root_origin_s,
                    0.0,
                    total_length_s,
                    std::move(stack_path),
                    std::move(chain)))
            {
                return nullptr;
            }
            ++child_index;
        }
    }

    if (ctx.failed)
    {
        return nullptr;
    }

    // Collect every cut boundary, then decide visibility on each resulting
    // interval.  Boundaries are kept in seconds and never rounded to frames.
    std::vector<double> boundaries;
    boundaries.reserve(2 + ctx.leaves.size() * 2);
    boundaries.push_back(0.0);
    boundaries.push_back(total_length_s);
    for (auto const& leaf: ctx.leaves)
    {
        if (leaf.end_s - leaf.start_s > TIME_EPSILON_S)
        {
            boundaries.push_back(leaf.start_s);
            boundaries.push_back(leaf.end_s);
        }
    }
    std::sort(boundaries.begin(), boundaries.end());
    std::vector<double> cuts;
    for (double b: boundaries)
    {
        if (cuts.empty() || b - cuts.back() > TIME_EPSILON_S)
        {
            cuts.push_back(b);
        }
    }

    std::vector<WinRun> runs;
    for (size_t i = 0; i + 1 < cuts.size(); ++i)
    {
        double const start_s = cuts[i];
        double const end_s   = cuts[i + 1];
        if (end_s - start_s <= TIME_EPSILON_S)
        {
            continue;
        }
        LeafSegment* winner = _pick_winner(ctx.leaves, start_s, end_s);
        if (!runs.empty() && runs.back().leaf == winner
            && std::fabs(runs.back().end_s - start_s) <= TIME_EPSILON_S)
        {
            runs.back().end_s = end_s;
        }
        else
        {
            runs.push_back(WinRun{ start_s, end_s, winner });
        }
    }

    // All geometry is known and validated; only now do we build the output.
    // The retainer destroys the track automatically if anything below fails,
    // so a failed call never hands back a half-built result.
    SerializableObject::Retainer<Track> track_holder(new Track);
    Track* flat_track = track_holder.value;
    flat_track->set_name("Flattened");

    double gap_start_s = 0.0;
    bool   have_gap    = false;

    for (WinRun const& run: runs)
    {
        if (!run.leaf)
        {
            if (!have_gap)
            {
                gap_start_s = run.start_s;
                have_gap    = true;
            }
            continue;
        }

        if (have_gap)
        {
            double const gap_length_s = run.start_s - gap_start_s;
            if (gap_length_s > TIME_EPSILON_S)
            {
                auto gap = new Gap(
                    _time_at_rate(gap_length_s, output_rate));
                flat_track->append_child(gap, error_status);
                if (is_error(error_status))
                {
                    return nullptr;
                }
            }
            have_gap = false;
        }

        LeafSegment const* leaf = run.leaf;
        Clip* clone = dynamic_cast<Clip*>(leaf->clip->clone(error_status));
        if (!clone || is_error(error_status))
        {
            return nullptr;
        }
        RationalTime media_start =
            _time_at_rate(run.start_s - leaf->origin_s, leaf->media_rate);
        RationalTime media_duration =
            _time_at_rate(run.end_s - run.start_s, leaf->media_rate);
        clone->set_source_range(TimeRange(media_start, media_duration));
        // Markers are projected onto the output Track instead.
        clone->markers().clear();
        flat_track->append_child(clone, error_status);
        if (is_error(error_status))
        {
            return nullptr;
        }
    }

    if (have_gap)
    {
        double const gap_length_s = total_length_s - gap_start_s;
        if (gap_length_s > TIME_EPSILON_S)
        {
            auto gap = new Gap(
                _time_at_rate(gap_length_s, output_rate));
            flat_track->append_child(gap, error_status);
            if (is_error(error_status))
            {
                return nullptr;
            }
        }
    }

    // Project markers.  Every marker is expressed in its owner's local
    // coordinates; only portions actually won by leaf pictures descended from
    // that owner survive, clipped to the owner's own trim.
    std::vector<OutputMarker> output_markers;
    for (MarkerRecord const& record: ctx.markers)
    {
        auto node_it = ctx.nodes.find(record.owner);
        if (node_it == ctx.nodes.end())
        {
            continue;
        }
        NodeInfo const& info = node_it->second;

        TimeRange const& marked = record.marker->marked_range();
        double const m_start_s =
            info.origin_s + marked.start_time().to_seconds();
        double const m_end_s =
            info.origin_s
            + marked.end_time_exclusive().to_seconds();

        double const win_start_s =
            std::max(m_start_s, info.window_start_s);
        double const win_end_s = std::min(m_end_s, info.window_end_s);
        if (win_end_s <= win_start_s + TIME_EPSILON_S
            && marked.duration().value() > 0.0)
        {
            continue;
        }

        if (marked.duration().value() == 0.0)
        {
            // Point marker: kept once, only inside a half-open visible run.
            double const point_s = m_start_s;
            if (point_s < info.window_start_s - TIME_EPSILON_S
                || point_s >= info.window_end_s - TIME_EPSILON_S)
            {
                continue;
            }
            bool visible = false;
            for (WinRun const& run: runs)
            {
                if (!run.leaf)
                {
                    continue;
                }
                bool const descended = std::find(
                                           run.leaf->chain.begin(),
                                           run.leaf->chain.end(),
                                           record.owner)
                                       != run.leaf->chain.end();
                if (!descended)
                {
                    continue;
                }
                if (point_s >= run.start_s - TIME_EPSILON_S
                    && point_s < run.end_s - TIME_EPSILON_S)
                {
                    visible = true;
                    break;
                }
            }
            if (!visible)
            {
                continue;
            }
            Marker* clone =
                dynamic_cast<Marker*>(record.marker->clone(error_status));
            if (!clone || is_error(error_status))
            {
                return nullptr;
            }
            clone->set_marked_range(TimeRange(
                _time_at_rate(point_s, output_rate),
                RationalTime(0.0, output_rate)));
            output_markers.push_back(
                OutputMarker{ point_s, SerializableObject::Retainer<Marker>(
                                           clone) });
            continue;
        }

        // Range marker: split along each continuous interval on which a
        // leaf descended from the owner actually wins.  Adjacent runs won by
        // different (but both descendant) leaves are one continuous interval
        // and stay a single marker; a transparent run or a foreign leaf is a
        // split boundary.
        std::optional<double> vis_start;
        double                vis_end = 0.0;
        for (WinRun const& run: runs)
        {
            bool const descended =
                run.leaf
                && std::find(
                       run.leaf->chain.begin(),
                       run.leaf->chain.end(),
                       record.owner)
                       != run.leaf->chain.end();
            if (!descended)
            {
                if (vis_start)
                {
                    double part_start_s = std::max(win_start_s, *vis_start);
                    double part_end_s   = std::min(win_end_s, vis_end);
                    if (part_end_s - part_start_s > TIME_EPSILON_S)
                    {
                        Marker* clone = dynamic_cast<Marker*>(
                            record.marker->clone(error_status));
                        if (!clone || is_error(error_status))
                        {
                            return nullptr;
                        }
                        clone->set_marked_range(TimeRange(
                            _time_at_rate(part_start_s, output_rate),
                            _time_at_rate(
                                part_end_s - part_start_s, output_rate)));
                        output_markers.push_back(OutputMarker{
                            part_start_s,
                            SerializableObject::Retainer<Marker>(clone) });
                    }
                    vis_start.reset();
                }
                continue;
            }
            if (!vis_start)
            {
                vis_start = run.start_s;
            }
            vis_end = run.end_s;
        }
        if (vis_start)
        {
            double part_start_s = std::max(win_start_s, *vis_start);
            double part_end_s   = std::min(win_end_s, vis_end);
            if (part_end_s - part_start_s > TIME_EPSILON_S)
            {
                Marker* clone = dynamic_cast<Marker*>(
                    record.marker->clone(error_status));
                if (!clone || is_error(error_status))
                {
                    return nullptr;
                }
                clone->set_marked_range(TimeRange(
                    _time_at_rate(part_start_s, output_rate),
                    _time_at_rate(part_end_s - part_start_s, output_rate)));
                output_markers.push_back(OutputMarker{
                    part_start_s,
                    SerializableObject::Retainer<Marker>(clone) });
            }
        }
    }

    // Program start first; the collection order is already tree preorder, and
    // a stable sort keeps that order when two markers share a start.
    std::stable_sort(
        output_markers.begin(),
        output_markers.end(),
        [](OutputMarker const& lhs, OutputMarker const& rhs) {
            return lhs.start_s < rhs.start_s - TIME_EPSILON_S;
        });

    for (OutputMarker const& om: output_markers)
    {
        flat_track->markers().emplace_back(om.marker);
    }

    // Hand ownership to the caller, who holds the result under the usual
    // Retainer rules.
    return track_holder.take_value();
}

}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS
