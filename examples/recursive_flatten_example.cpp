// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

// C++ call example for flatten_stack_recursive().
//
// Builds the canonical multi-layer scene (a bottom clip and a nested stack
// with internal gaps), flattens it recursively into a single editable track,
// and prints the resulting clips, gaps and projected markers.

#include <opentimelineio/clip.h>
#include <opentimelineio/gap.h>
#include <opentimelineio/marker.h>
#include <opentimelineio/stack.h>
#include <opentimelineio/stackAlgorithm.h>
#include <opentimelineio/track.h>

#include <iostream>

namespace o = opentimelineio::OPENTIMELINEIO_VERSION_NS;
using o::RationalTime;
using o::TimeRange;

namespace {

TimeRange
frame_range(double start, double duration, double rate = 24.0)
{
    return TimeRange(
        RationalTime(start, rate), RationalTime(duration, rate));
}

} // namespace

int
main()
{
    o::ErrorStatus error;

    // Bottom layer: clip B covering media [100, 120).
    auto bottom = new o::Track("bottom");
    bottom->append_child(
        new o::Clip("B", nullptr, frame_range(100.0, 20.0)), &error);

    // Nested stack, trimmed to [2, 14), holding a gap, clip A and a gap.
    auto nested = new o::Stack("nested", frame_range(2.0, 12.0));
    auto inside = new o::Track("inside");
    inside->append_child(new o::Gap(RationalTime(4, 24)), &error);
    inside->append_child(
        new o::Clip("A", nullptr, frame_range(200.0, 8.0)), &error);
    inside->append_child(new o::Gap(RationalTime(8, 24)), &error);
    nested->append_child(inside, &error);
    nested->markers().emplace_back(
        new o::Marker("nested_marker", frame_range(0.0, 12.0)));

    auto top = new o::Track("top");
    top->append_child(nested, &error);

    auto scene = new o::Stack("scene");
    scene->append_child(bottom, &error);
    scene->append_child(top, &error);
    if (o::is_error(error))
    {
        std::cerr << "failed to build scene: " << error.full_description
                  << '\n';
        return 1;
    }

    // The caller owns the result through a Retainer.
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &error));
    if (!flat || o::is_error(error))
    {
        std::cerr << "flatten failed: " << error.full_description << '\n';
        return 1;
    }

    std::cout << "flattened duration (frames at 24fps): "
              << flat->duration(&error).to_frames() << '\n';
    for (auto const& child: flat->children())
    {
        if (auto clip = dynamic_cast<o::Clip*>(child.value))
        {
            auto sr = clip->source_range().value();
            std::cout << "Clip " << clip->name() << " media ["
                      << sr.start_time().value() << ", "
                      << sr.end_time_exclusive().value() << ")\n";
        }
        else if (auto gap = dynamic_cast<o::Gap*>(child.value))
        {
            std::cout << "Gap of "
                      << gap->duration(&error).to_frames() << " frames\n";
        }
    }

    std::cout << "projected markers: " << flat->markers().size() << '\n';
    for (auto const& marker: flat->markers())
    {
        auto range = marker->marked_range();
        std::cout << "  Marker " << marker->name() << " ["
                  << range.start_time().value() << ", "
                  << range.end_time_exclusive().value() << ")\n";
    }

    return 0;
}
