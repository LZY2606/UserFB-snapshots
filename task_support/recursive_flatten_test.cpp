// SPDX-License-Identifier: Apache-2.0
// Tests for flatten_stack_recursive().  Only C++ is used, and every case goes
// through the real production entry point.

#include "check.h"

#include <opentimelineio/clip.h>
#include <opentimelineio/errorStatus.h>
#include <opentimelineio/externalReference.h>
#include <opentimelineio/gap.h>
#include <opentimelineio/marker.h>
#include <opentimelineio/stack.h>
#include <opentimelineio/stackAlgorithm.h>
#include <opentimelineio/timeEffect.h>
#include <opentimelineio/track.h>
#include <opentimelineio/trackAlgorithm.h>
#include <opentimelineio/transition.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace o = opentimelineio::OPENTIMELINEIO_VERSION_NS;
using o::RationalTime;
using o::TimeRange;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static TimeRange
rng(double start, double duration, double rate)
{
    return TimeRange(RationalTime(start, rate), RationalTime(duration, rate));
}

static bool
near(double a, double b, double tol = 1e-9)
{
    return std::fabs(a - b) <= tol * (1.0 + std::fabs(b));
}

static bool
time_near(RationalTime const& t, double value, double rate)
{
    return t.rate() == rate && near(t.value(), value);
}

static bool
add(o::Composition* parent, o::Composable* child)
{
    o::ErrorStatus status;
    bool           ok = parent->append_child(child, &status);
    return ok && !o::is_error(status);
}

static o::Track*
track(std::string const& name)
{
    return new o::Track(name);
}

static o::Stack*
stack(std::string const& name, std::optional<TimeRange> const& source_range =
                                 std::nullopt)
{
    return new o::Stack(name, source_range);
}

static o::Clip*
clip(
    std::string const&              name,
    double                          start,
    double                          duration,
    double                          rate,
    o::MediaReference*              media = nullptr)
{
    return new o::Clip(name, media, rng(start, duration, rate));
}

struct Piece
{
    bool        gap = false;
    std::string name;
    RationalTime start;
    RationalTime duration;
};

static std::vector<Piece>
pieces(o::Track* flat, o::ErrorStatus* error_status = nullptr)
{
    std::vector<Piece> result;
    for (auto const& child: flat->children())
    {
        if (auto gap = dynamic_cast<o::Gap*>(child.value))
        {
            Piece p;
            p.gap      = true;
            p.name     = gap->name();
            p.start    = gap->source_range()->start_time();
            p.duration = gap->source_range()->duration();
            result.push_back(p);
        }
        else if (auto c = dynamic_cast<o::Clip*>(child.value))
        {
            Piece p;
            p.name     = c->name();
            p.start    = c->source_range()->start_time();
            p.duration = c->source_range()->duration();
            result.push_back(p);
        }
        else
        {
            // Only plain clips and gaps are allowed in the output.
            if (error_status)
            {
                *error_status = o::ErrorStatus(
                    o::ErrorStatus::TYPE_MISMATCH,
                    "non Clip/Gap in flattened output",
                    child.value);
            }
            return {};
        }
    }
    return result;
}

static int
count_clips(o::Track* flat)
{
    int n = 0;
    for (auto const& child: flat->children())
    {
        if (dynamic_cast<o::Clip*>(child.value))
        {
            ++n;
        }
    }
    return n;
}

static int
count_gaps(o::Track* flat)
{
    int n = 0;
    for (auto const& child: flat->children())
    {
        if (dynamic_cast<o::Gap*>(child.value))
        {
            ++n;
        }
    }
    return n;
}

// Builds the example from the task description:
//   bottom : B[100,120)
//   top    : Stack source_range=[2,14)
//              Track: Gap 4, A[200,208), Gap 8
// All rates are 24.
struct ExampleTree
{
    o::Stack* scene  = nullptr;
    o::Track* bottom = nullptr;
    o::Clip*  b      = nullptr;
    o::Track* top    = nullptr;
    o::Stack* nest   = nullptr;
    o::Track* inside = nullptr;
    o::Clip*  a      = nullptr;
};

static ExampleTree
build_example()
{
    ExampleTree t;
    t.scene  = stack("scene");
    t.bottom = track("bottom");
    t.b      = clip("B", 100, 20, 24);
    add(t.bottom, t.b);
    t.nest   = stack("nested", rng(2, 12, 24));
    t.inside = track("inside");
    add(t.inside, new o::Gap(RationalTime(4, 24)));
    t.a = clip("A", 200, 8, 24);
    add(t.inside, t.a);
    add(t.inside, new o::Gap(RationalTime(8, 24)));
    add(t.nest, t.inside);
    t.top = track("top");
    add(t.top, t.nest);
    add(t.scene, t.bottom);
    add(t.scene, t.top);
    return t;
}

// An Item type the recursive flattener does not support.
class UnsupportedItem : public o::Item
{
public:
    UnsupportedItem()
        : Item("Unsupported")
    {}
    ~UnsupportedItem() override {}
};

// ---------------------------------------------------------------------------
// visibility
// ---------------------------------------------------------------------------

static int
visibility_example_occlusion()
{
    ExampleTree t = build_example();
    o::ErrorStatus status;
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(t.scene, &status));
    TASK_CHECK(flat && !o::is_error(status));

    TASK_CHECK(time_near(flat->duration(), 20, 24));
    TASK_CHECK(flat->children().size() == 3);

    auto p = pieces(flat);
    TASK_CHECK(p.size() == 3);

    TASK_CHECK(!p[0].gap && p[0].name == "B");
    TASK_CHECK(time_near(p[0].start, 100, 24));
    TASK_CHECK(time_near(p[0].duration, 2, 24));

    TASK_CHECK(!p[1].gap && p[1].name == "A");
    TASK_CHECK(time_near(p[1].start, 200, 24));
    TASK_CHECK(time_near(p[1].duration, 8, 24));

    TASK_CHECK(!p[2].gap && p[2].name == "B");
    TASK_CHECK(time_near(p[2].start, 110, 24));
    TASK_CHECK(time_near(p[2].duration, 10, 24));

    // The nested Stack must not survive as an opaque item.
    for (auto const& child: flat->children())
    {
        TASK_CHECK(dynamic_cast<o::Stack*>(child.value) == nullptr);
        TASK_CHECK(dynamic_cast<o::Track*>(child.value) == nullptr);
    }
    return 0;
}

static int
visibility_outer_crop()
{
    ExampleTree t = build_example();
    t.scene->set_source_range(rng(1, 10, 24));

    o::ErrorStatus status;
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(t.scene, &status));
    TASK_CHECK(flat && !o::is_error(status));

    TASK_CHECK(time_near(flat->duration(), 10, 24));
    auto p = pieces(flat);
    TASK_CHECK(p.size() == 3);

    TASK_CHECK(p[0].name == "B");
    TASK_CHECK(time_near(p[0].start, 101, 24));
    TASK_CHECK(time_near(p[0].duration, 1, 24));

    TASK_CHECK(p[1].name == "A");
    // The outer crop must not pull A's source start back; it stays at 200.
    TASK_CHECK(time_near(p[1].start, 200, 24));
    TASK_CHECK(time_near(p[1].duration, 8, 24));

    TASK_CHECK(p[2].name == "B");
    TASK_CHECK(time_near(p[2].start, 110, 24));
    TASK_CHECK(time_near(p[2].duration, 1, 24));
    return 0;
}

static int
visibility_disabled_branch_reveals_lower()
{
    // Disabled top track: the bottom clip wins everywhere.
    {
        o::Stack* scene  = stack("scene");
        o::Track* bottom = track("bottom");
        add(bottom, clip("B", 100, 10, 24));
        o::Track* top = track("top");
        add(top, clip("A", 200, 10, 24));
        top->set_enabled(false);
        add(scene, bottom);
        add(scene, top);

        o::ErrorStatus status;
        o::SerializableObject::Retainer<o::Track> flat(
            o::flatten_stack_recursive(scene, &status));
        TASK_CHECK(flat && !o::is_error(status));
        auto p = pieces(flat);
        TASK_CHECK(p.size() == 1);
        TASK_CHECK(p[0].name == "B");
        TASK_CHECK(time_near(p[0].start, 100, 24));
        TASK_CHECK(time_near(p[0].duration, 10, 24));
    }

    // A disabled container exposes the lower layers through its whole extent.
    {
        o::Stack* scene  = stack("scene");
        o::Track* bottom = track("bottom");
        add(bottom, clip("B", 100, 10, 24));
        o::Track* top = track("top");
        o::Stack* nest = stack("nest", rng(0, 10, 24));
        o::Track* inner = track("inner");
        add(inner, clip("A", 200, 10, 24));
        inner->set_enabled(false);
        add(nest, inner);
        add(top, nest);
        add(scene, bottom);
        add(scene, top);

        o::ErrorStatus status;
        o::SerializableObject::Retainer<o::Track> flat(
            o::flatten_stack_recursive(scene, &status));
        TASK_CHECK(flat && !o::is_error(status));
        auto p = pieces(flat);
        TASK_CHECK(p.size() == 1);
        TASK_CHECK(p[0].name == "B");
        TASK_CHECK(time_near(p[0].start, 100, 24));
        TASK_CHECK(time_near(p[0].duration, 10, 24));
    }

    // Enabling the inner track again makes A win.
    {
        o::Stack* scene  = stack("scene");
        o::Track* bottom = track("bottom");
        add(bottom, clip("B", 100, 10, 24));
        o::Track* top   = track("top");
        o::Stack* nest  = stack("nest", rng(0, 10, 24));
        o::Track* inner = track("inner");
        add(inner, clip("A", 200, 10, 24));
        add(nest, inner);
        add(top, nest);
        add(scene, bottom);
        add(scene, top);

        o::ErrorStatus status;
        o::SerializableObject::Retainer<o::Track> flat(
            o::flatten_stack_recursive(scene, &status));
        TASK_CHECK(flat && !o::is_error(status));
        auto p = pieces(flat);
        TASK_CHECK(p.size() == 1);
        TASK_CHECK(p[0].name == "A");
        TASK_CHECK(time_near(p[0].start, 200, 24));
    }
    return 0;
}

static int
visibility_all_transparent_and_empty()
{
    // Every layer is transparent: one gap spanning the duration.
    {
        o::Stack* scene = stack("scene", rng(0, 12, 24));
        o::Track* tr    = track("tr");
        add(tr, new o::Gap(RationalTime(12, 24)));
        o::Stack* nest = stack("nest", rng(0, 12, 24));
        o::Track* inner = track("inner");
        o::Clip* c = clip("hidden", 0, 12, 24);
        c->set_enabled(false);
        add(inner, c);
        add(nest, inner);
        add(scene, tr);
        add(scene, nest);

        o::ErrorStatus status;
        o::SerializableObject::Retainer<o::Track> flat(
            o::flatten_stack_recursive(scene, &status));
        TASK_CHECK(flat && !o::is_error(status));
        TASK_CHECK(time_near(flat->duration(), 12, 24));
        TASK_CHECK(flat->children().size() == 1);
        TASK_CHECK(count_gaps(flat) == 1);
        auto p = pieces(flat);
        TASK_CHECK(p.size() == 1 && p[0].gap);
        TASK_CHECK(time_near(p[0].duration, 12, 24));
    }

    // Empty container with no source_range is legal.
    {
        o::Stack*      scene = stack("scene");
        o::ErrorStatus status;
        o::SerializableObject::Retainer<o::Track> flat(
            o::flatten_stack_recursive(scene, &status));
        TASK_CHECK(flat && !o::is_error(status));
        TASK_CHECK(flat->children().empty());
        TASK_CHECK(time_near(flat->duration(), 0, 1));
    }

    // Empty container with a source_range: gaps, not nothing.
    {
        o::Stack*      scene = stack("scene", rng(0, 5, 24));
        o::ErrorStatus status;
        o::SerializableObject::Retainer<o::Track> flat(
            o::flatten_stack_recursive(scene, &status));
        TASK_CHECK(flat && !o::is_error(status));
        TASK_CHECK(time_near(flat->duration(), 5, 24));
        TASK_CHECK(flat->children().size() == 1);
        TASK_CHECK(count_gaps(flat) == 1);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// coordinates
// ---------------------------------------------------------------------------

static int
coordinates_inner_crop()
{
    // Give the nested stack a tighter source_range; A keeps its real source
    // position and the exposed bottom clip is offset by the nested crop.
    ExampleTree t = build_example();
    t.nest->set_source_range(rng(4, 6, 24));

    o::ErrorStatus status;
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(t.scene, &status));
    TASK_CHECK(flat && !o::is_error(status));
    TASK_CHECK(time_near(flat->duration(), 20, 24));

    auto p = pieces(flat);
    TASK_CHECK(p.size() == 2);
    TASK_CHECK(p[0].name == "A");
    TASK_CHECK(time_near(p[0].start, 200, 24));
    TASK_CHECK(time_near(p[0].duration, 6, 24));

    TASK_CHECK(p[1].name == "B");
    TASK_CHECK(time_near(p[1].start, 106, 24));
    TASK_CHECK(time_near(p[1].duration, 14, 24));
    return 0;
}

static int
coordinates_multilayer_alternation()
{
    // Stack -> Track -> Stack -> Track -> Clip, with occlusion at several
    // levels.
    o::Stack* scene  = stack("scene");
    o::Track* bottom = track("bottom");
    add(bottom, clip("B", 100, 20, 24));

    o::Track* top = track("top");
    o::Stack* s2  = stack("s2");
    o::Track* t3  = track("t3");
    add(t3, new o::Gap(RationalTime(3, 24)));
    add(t3, clip("A", 200, 8, 24));
    add(t3, new o::Gap(RationalTime(9, 24)));
    o::Track* t4 = track("t4");
    add(t4, new o::Gap(RationalTime(6, 24)));
    add(t4, clip("C", 300, 2, 24));
    add(t4, new o::Gap(RationalTime(12, 24)));
    add(s2, t3);
    add(s2, t4);
    add(top, s2);
    add(scene, bottom);
    add(scene, top);

    o::ErrorStatus status;
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &status));
    TASK_CHECK(flat && !o::is_error(status));
    TASK_CHECK(time_near(flat->duration(), 20, 24));

    auto p = pieces(flat);
    TASK_CHECK(p.size() == 5);

    TASK_CHECK(p[0].name == "B" && time_near(p[0].start, 100, 24)
               && time_near(p[0].duration, 3, 24));
    TASK_CHECK(p[1].name == "A" && time_near(p[1].start, 200, 24)
               && time_near(p[1].duration, 3, 24));
    TASK_CHECK(p[2].name == "C" && time_near(p[2].start, 300, 24)
               && time_near(p[2].duration, 2, 24));
    TASK_CHECK(p[3].name == "A" && time_near(p[3].start, 205, 24)
               && time_near(p[3].duration, 3, 24));
    TASK_CHECK(p[4].name == "B" && time_near(p[4].start, 111, 24)
               && time_near(p[4].duration, 9, 24));

    // The reappearing A must be a separate copy from the first A.
    o::Clip* first  = nullptr;
    o::Clip* second = nullptr;
    for (auto const& child: flat->children())
    {
        if (auto c = dynamic_cast<o::Clip*>(child.value))
        {
            if (c->name() == "A")
            {
                if (!first)
                {
                    first = c;
                }
                else
                {
                    second = c;
                }
            }
        }
    }
    TASK_CHECK(first && second && first != second);
    return 0;
}

static int
coordinates_mixed_rates()
{
    // B is 24fps, C is 25fps, D is 48kHz.  The overlay is a Track whose gap
    // and clip are expressed at different rates, so the winning window lands
    // part-way through a bottom frame.  Positions must be rescaled by time
    // value, never rounded to frames.
    o::Stack* scene  = stack("scene");
    o::Track* bottom = track("bottom");
    add(bottom, clip("B", 100, 1, 24));
    add(bottom, clip("C", 50, 1, 25));
    add(bottom, clip("D", 1000, 48000, 48000));

    o::Track* top   = track("top");
    o::Track* inner = track("inner");
    add(inner, new o::Gap(rng(0, 12, 48000)));
    // 480 samples @48kHz = 0.25 frame @25fps, starting 12 samples in.
    add(inner, new o::Clip(
                   "M",
                   nullptr,
                   TimeRange(RationalTime(10, 25), RationalTime(480, 48000))));
    add(top, inner);
    add(scene, bottom);
    add(scene, top);

    o::ErrorStatus status;
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &status));
    TASK_CHECK(flat && !o::is_error(status));
    // 1 frame @24 + 1 frame @25 + 1s @48k in seconds, expressed at 48k.
    TASK_CHECK(time_near(flat->duration(), 51920, 48000));

    auto p = pieces(flat);
    TASK_CHECK(p.size() == 5);

    // B before the mask: 12 samples of a 2000-sample 24fps frame.
    TASK_CHECK(p[0].name == "B");
    TASK_CHECK(time_near(p[0].start, 100, 24));
    TASK_CHECK(time_near(p[0].duration, 0.006, 24));

    // M keeps its own 25fps source start and its 0.25-frame duration.
    TASK_CHECK(p[1].name == "M");
    TASK_CHECK(time_near(p[1].start, 10, 25));
    TASK_CHECK(time_near(p[1].duration, 0.25, 25));

    // B resumes 492 samples (0.246 frames) into its source.
    TASK_CHECK(p[2].name == "B");
    TASK_CHECK(time_near(p[2].start, 100.246, 24));
    TASK_CHECK(time_near(p[2].duration, 0.754, 24));

    TASK_CHECK(p[3].name == "C");
    TASK_CHECK(time_near(p[3].start, 50, 25));
    TASK_CHECK(time_near(p[3].duration, 1, 25));

    TASK_CHECK(p[4].name == "D");
    TASK_CHECK(time_near(p[4].start, 1000, 48000));
    TASK_CHECK(time_near(p[4].duration, 48000, 48000));
    return 0;
}

static int
coordinates_zero_duration()
{
    o::Stack*      scene = stack("scene", rng(5, 0, 24));
    o::ErrorStatus status;
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &status));
    TASK_CHECK(flat && !o::is_error(status));
    TASK_CHECK(flat->children().empty());
    TASK_CHECK(near(flat->duration().to_seconds(), 0));
    return 0;
}

static int
coordinates_failure_inputs()
{
    // Transition anywhere in the tree.
    {
        o::Stack* scene = stack("scene");
        o::Track* tr    = track("tr");
        add(tr, clip("A", 0, 10, 24));
        add(tr, new o::Transition(
                    "dissolve",
                    o::Transition::Type::SMPTE_Dissolve,
                    RationalTime(2, 24),
                    RationalTime(2, 24)));
        add(tr, clip("B", 10, 10, 24));
        add(scene, tr);
        o::ErrorStatus status;
        TASK_CHECK(o::flatten_stack_recursive(scene, &status) == nullptr);
        TASK_CHECK(o::is_error(status));
    }

    // TimeEffect on a clip.
    {
        o::Stack* scene = stack("scene");
        o::Track* tr    = track("tr");
        o::Clip*  c     = clip("A", 0, 10, 24);
        c->effects().push_back(new o::TimeEffect("warp", "LinearTimeWarp"));
        add(tr, c);
        add(scene, tr);
        o::ErrorStatus status;
        TASK_CHECK(o::flatten_stack_recursive(scene, &status) == nullptr);
        TASK_CHECK(o::is_error(status));
    }

    // Unsupported item type.
    {
        o::Stack* scene = stack("scene");
        o::Track* tr    = track("tr");
        add(tr, new UnsupportedItem());
        add(scene, tr);
        o::ErrorStatus status;
        TASK_CHECK(o::flatten_stack_recursive(scene, &status) == nullptr);
        TASK_CHECK(o::is_error(status));
    }

    // Non-finite time.
    {
        o::Stack* scene = stack("scene");
        o::Track* tr    = track("tr");
        add(tr, new o::Clip(
                    "A",
                    nullptr,
                    TimeRange(
                        RationalTime(std::numeric_limits<double>::infinity(), 24),
                        RationalTime(1, 24))));
        add(scene, tr);
        o::ErrorStatus status;
        TASK_CHECK(o::flatten_stack_recursive(scene, &status) == nullptr);
        TASK_CHECK(o::is_error(status));
    }

    // Non-positive rate.
    {
        o::Stack* scene = stack("scene");
        o::Track* tr    = track("tr");
        add(tr, new o::Clip("A", nullptr, rng(0, 1, 0)));
        add(scene, tr);
        o::ErrorStatus status;
        TASK_CHECK(o::flatten_stack_recursive(scene, &status) == nullptr);
        TASK_CHECK(o::is_error(status));
    }

    // Negative duration.
    {
        o::Stack* scene = stack("scene");
        o::Track* tr    = track("tr");
        add(tr, new o::Clip("A", nullptr, rng(0, -1, 24)));
        add(scene, tr);
        o::ErrorStatus status;
        TASK_CHECK(o::flatten_stack_recursive(scene, &status) == nullptr);
        TASK_CHECK(o::is_error(status));
    }

    // Null input.
    {
        o::ErrorStatus status;
        TASK_CHECK(o::flatten_stack_recursive(nullptr, &status) == nullptr);
        TASK_CHECK(o::is_error(status));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// markers
// ---------------------------------------------------------------------------

static int
markers_projection_and_split()
{
    ExampleTree t = build_example();
    o::Marker* stack_marker = new o::Marker("stack", rng(0, 12, 24));
    t.nest->markers().push_back(stack_marker);
    o::Marker* clip_marker = new o::Marker("clip", rng(100, 20, 24));
    t.b->markers().push_back(clip_marker);

    o::ErrorStatus status;
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(t.scene, &status));
    TASK_CHECK(flat && !o::is_error(status));

    TASK_CHECK(flat->markers().size() == 3);
    auto const& m = flat->markers();

    TASK_CHECK(m[0]->name() == "clip");
    TASK_CHECK(time_near(m[0]->marked_range().start_time(), 0, 24));
    TASK_CHECK(time_near(m[0]->marked_range().duration(), 2, 24));

    TASK_CHECK(m[1]->name() == "stack");
    TASK_CHECK(time_near(m[1]->marked_range().start_time(), 2, 24));
    TASK_CHECK(time_near(m[1]->marked_range().duration(), 8, 24));

    TASK_CHECK(m[2]->name() == "clip");
    TASK_CHECK(time_near(m[2]->marked_range().start_time(), 10, 24));
    TASK_CHECK(time_near(m[2]->marked_range().duration(), 10, 24));

    // The project markers live on the output track, not on the clips.
    for (auto const& child: flat->children())
    {
        if (auto c = dynamic_cast<o::Clip*>(child.value))
        {
            TASK_CHECK(c->markers().empty());
        }
    }

    // Clones, not the original objects.
    for (auto const& marker: flat->markers())
    {
        TASK_CHECK(marker.value != stack_marker);
        TASK_CHECK(marker.value != clip_marker);
    }
    return 0;
}

static int
markers_points_and_endpoints()
{
    ExampleTree t = build_example();
    // The nested stack's visible leaf coverage in its own coordinates is
    // [4,12).  Point markers at 2, 4, 8, 12 must keep only 4 and 8; the point
    // exactly on the end of the visible interval is dropped.
    t.nest->markers().push_back(new o::Marker("p2", rng(2, 0, 24)));
    t.nest->markers().push_back(new o::Marker("p4", rng(4, 0, 24)));
    t.nest->markers().push_back(new o::Marker("p8", rng(8, 0, 24)));
    t.nest->markers().push_back(new o::Marker("p12", rng(12, 0, 24)));

    o::ErrorStatus status;
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(t.scene, &status));
    TASK_CHECK(flat && !o::is_error(status));

    TASK_CHECK(flat->markers().size() == 2);
    TASK_CHECK(flat->markers()[0]->name() == "p4");
    TASK_CHECK(time_near(flat->markers()[0]->marked_range().start_time(), 2, 24));
    TASK_CHECK(time_near(flat->markers()[0]->marked_range().duration(), 0, 24));

    TASK_CHECK(flat->markers()[1]->name() == "p8");
    TASK_CHECK(time_near(flat->markers()[1]->marked_range().start_time(), 6, 24));
    TASK_CHECK(time_near(flat->markers()[1]->marked_range().duration(), 0, 24));

    // A point on the end of a clip's winning interval is dropped too.
    ExampleTree t2 = build_example();
    // B's source is [100,120); the point at the exclusive end is dropped.
    t2.b->markers().push_back(new o::Marker("endpoint", rng(120, 0, 24)));
    // The point at the start of the visible range is kept once.
    t2.b->markers().push_back(new o::Marker("startpoint", rng(100, 0, 24)));

    o::ErrorStatus status2;
    o::SerializableObject::Retainer<o::Track> flat2(
        o::flatten_stack_recursive(t2.scene, &status2));
    TASK_CHECK(flat2 && !o::is_error(status2));
    TASK_CHECK(flat2->markers().size() == 1);
    TASK_CHECK(flat2->markers()[0]->name() == "startpoint");
    TASK_CHECK(time_near(
        flat2->markers()[0]->marked_range().start_time(),
        0,
        24));
    return 0;
}

static int
markers_order_attributes()
{
    ExampleTree t = build_example();

    o::Marker* first = new o::Marker(
        "first",
        rng(4, 4, 24),
        o::Color::red,
        o::AnyDictionary{ { "tag", std::string("one") } },
        "a comment");
    t.nest->markers().push_back(first);

    // Same program start as the stack marker but owned by the clip, which
    // appears earlier in the preorder tree.
    o::Marker* second = new o::Marker(
        "second",
        rng(100, 2, 24),
        o::Color::blue,
        o::AnyDictionary{ { "tag", std::string("two") } },
        "another comment");
    t.b->markers().push_back(second);

    o::ErrorStatus status;
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(t.scene, &status));
    TASK_CHECK(flat && !o::is_error(status));
    TASK_CHECK(flat->markers().size() == 2);

    // Both start at program time 0/2? The clip marker starts at 0, the stack
    // marker at 2, so ordering is by program start.
    TASK_CHECK(flat->markers()[0]->name() == "second");
    TASK_CHECK(flat->markers()[1]->name() == "first");

    auto const& m0 = *flat->markers()[0];
    TASK_CHECK(m0.comment() == "another comment");
    TASK_CHECK(m0.color().has_value()
               && m0.color()->name() == o::Color::blue.name());
    auto meta0 = m0.metadata();
    TASK_CHECK(meta0.find("tag") != meta0.end());
    TASK_CHECK(std::any_cast<std::string>(meta0.at("tag")) == "two");

    auto const& m1 = *flat->markers()[1];
    TASK_CHECK(m1.comment() == "a comment");
    TASK_CHECK(m1.color().has_value()
               && m1.color()->name() == o::Color::red.name());
    auto meta1 = m1.metadata();
    TASK_CHECK(meta1.find("tag") != meta1.end());
    TASK_CHECK(std::any_cast<std::string>(meta1.at("tag")) == "one");
    return 0;
}

static int
markers_json_reload_and_reedit()
{
    ExampleTree t = build_example();
    t.nest->markers().push_back(new o::Marker("stack", rng(0, 12, 24)));
    t.b->markers().push_back(new o::Marker("clip", rng(100, 20, 24)));

    o::ErrorStatus status;
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(t.scene, &status));
    TASK_CHECK(flat && !o::is_error(status));
    auto before = pieces(flat);
    TASK_CHECK(before.size() == 3);
    TASK_CHECK(flat->markers().size() == 3);

    std::string json = flat->to_json_string(&status);
    TASK_CHECK(!o::is_error(status));
    o::SerializableObject::Retainer<o::SerializableObject> reloaded(
        o::SerializableObject::from_json_string(json, &status));
    TASK_CHECK(reloaded && !o::is_error(status));
    o::Track* reloaded_track = dynamic_cast<o::Track*>(reloaded.value);
    TASK_CHECK(reloaded_track != nullptr);

    auto after = pieces(reloaded_track);
    TASK_CHECK(after.size() == before.size());
    for (size_t i = 0; i < after.size(); ++i)
    {
        TASK_CHECK(after[i].name == before[i].name);
        TASK_CHECK(after[i].start == before[i].start);
        TASK_CHECK(after[i].duration == before[i].duration);
    }
    TASK_CHECK(reloaded_track->markers().size() == 3);

    // Still trimmable.
    o::SerializableObject::Retainer<o::Track> trimmed(
        o::track_trimmed_to_range(reloaded_track, rng(2, 6, 24), &status));
    TASK_CHECK(trimmed && !o::is_error(status));
    TASK_CHECK(time_near(trimmed->duration(), 6, trimmed->duration().rate()));
    TASK_CHECK(near(trimmed->duration().to_seconds(), 0.25));

    // Still flattenable: put it back in a stack and flatten again.
    o::Stack* wrapper = stack("wrapper");
    add(wrapper, reloaded_track);
    o::SerializableObject::Retainer<o::Track> again(
        o::flatten_stack_recursive(wrapper, &status));
    TASK_CHECK(again && !o::is_error(status));
    auto again_pieces = pieces(again);
    TASK_CHECK(again_pieces.size() == before.size());
    for (size_t i = 0; i < again_pieces.size(); ++i)
    {
        TASK_CHECK(again_pieces[i].name == before[i].name);
        TASK_CHECK(again_pieces[i].start == before[i].start);
        TASK_CHECK(again_pieces[i].duration == before[i].duration);
    }

    // The original input tree is untouched.
    TASK_CHECK(t.nest->source_range().has_value());
    TASK_CHECK(time_near(t.nest->source_range()->start_time(), 2, 24));
    TASK_CHECK(t.b->source_range().has_value());
    TASK_CHECK(time_near(t.b->source_range()->start_time(), 100, 24));
    TASK_CHECK(t.b->markers().size() == 1);
    TASK_CHECK(t.nest->markers().size() == 1);
    return 0;
}

static int
markers_copy_independence()
{
    ExampleTree t = build_example();
    o::Marker* input_marker = new o::Marker("stack", rng(0, 12, 24));
    t.nest->markers().push_back(input_marker);

    auto* media = new o::ExternalReference(
        "file:///media/b.mov",
        rng(100, 20, 24),
        o::AnyDictionary{ { "source", std::string("original") } });
    t.b->set_media_reference(media);

    o::ErrorStatus status;
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(t.scene, &status));
    TASK_CHECK(flat && !o::is_error(status));

    // Locate the two projected B pieces.
    std::vector<o::Clip*> b_clips;
    for (auto const& child: flat->children())
    {
        if (auto c = dynamic_cast<o::Clip*>(child.value))
        {
            if (c->name() == "B")
            {
                b_clips.push_back(c);
            }
        }
    }
    TASK_CHECK(b_clips.size() == 2);
    TASK_CHECK(b_clips[0] != b_clips[1]);

    // Separate mutable clips and media references.
    TASK_CHECK(b_clips[0] != t.b);
    TASK_CHECK(b_clips[1] != t.b);
    TASK_CHECK(b_clips[0]->media_reference() != t.b->media_reference());
    TASK_CHECK(b_clips[1]->media_reference() != t.b->media_reference());
    TASK_CHECK(
        b_clips[0]->media_reference() != b_clips[1]->media_reference());
    auto* external =
        dynamic_cast<o::ExternalReference*>(b_clips[0]->media_reference());
    TASK_CHECK(external != nullptr);
    TASK_CHECK(external && external->target_url() == "file:///media/b.mov");
    TASK_CHECK(external->metadata().find("source") != external->metadata().end());

    // Marker is a clone.
    TASK_CHECK(flat->markers().size() == 1);
    TASK_CHECK(flat->markers()[0].value != input_marker);

    // Mutating output does not affect the input.
    b_clips[0]->set_source_range(rng(999, 1, 24));
    b_clips[0]->metadata()["mutated"] = true;
    b_clips[0]->set_name("changed");
    flat->markers()[0]->set_marked_range(rng(0, 1, 24));
    external->set_target_url("file:///changed.mov");

    TASK_CHECK(time_near(t.b->source_range()->start_time(), 100, 24));
    TASK_CHECK(time_near(t.b->source_range()->duration(), 20, 24));
    TASK_CHECK(t.b->name() == "B");
    TASK_CHECK(t.b->metadata().find("mutated") == t.b->metadata().end());
    TASK_CHECK(time_near(input_marker->marked_range().start_time(), 0, 24));
    TASK_CHECK(time_near(input_marker->marked_range().duration(), 12, 24));
    TASK_CHECK(media->target_url() == "file:///media/b.mov");
    TASK_CHECK(time_near(t.b->media_reference()->available_range()->start_time(), 100, 24));
    return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
    { "visibility", "example_occlusion", visibility_example_occlusion },
    { "visibility", "outer_crop", visibility_outer_crop },
    { "visibility", "disabled_branch_reveals_lower", visibility_disabled_branch_reveals_lower },
    { "visibility", "all_transparent_and_empty", visibility_all_transparent_and_empty },

    { "coordinates", "inner_crop", coordinates_inner_crop },
    { "coordinates", "multilayer_alternation", coordinates_multilayer_alternation },
    { "coordinates", "mixed_rates", coordinates_mixed_rates },
    { "coordinates", "zero_duration", coordinates_zero_duration },
    { "coordinates", "failure_inputs", coordinates_failure_inputs },

    { "markers", "projection_and_split", markers_projection_and_split },
    { "markers", "points_and_endpoints", markers_points_and_endpoints },
    { "markers", "order_attributes", markers_order_attributes },
    { "markers", "json_reload_and_reedit", markers_json_reload_and_reedit },
    { "markers", "copy_independence", markers_copy_independence },
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
