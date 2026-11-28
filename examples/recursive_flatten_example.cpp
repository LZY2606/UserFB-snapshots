// SPDX-License-Identifier: Apache-2.0
// Example: flatten a nested multi-camera scene into a single editable track.

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

static TimeRange
frames(double start, double duration, double rate)
{
    return TimeRange(RationalTime(start, rate), RationalTime(duration, rate));
}

static bool
append(o::Composition* parent, o::Composable* child)
{
    o::ErrorStatus status;
    return parent->append_child(child, &status) && !o::is_error(status);
}

int
main()
{
    constexpr double rate = 24.0;

    // The scene stacks two layers: a base camera and a nested overlay.
    auto scene  = new o::Stack("scene");
    auto bottom = new o::Track("bottom");
    append(bottom, new o::Clip("B", nullptr, frames(100, 20, rate)));

    // The nested Stack holds a Track with Gap / A / Gap.  Its source_range
    // selects a 12-frame window of that 20-frame composition.
    auto nested = new o::Stack("nested", frames(2, 12, rate));
    auto inside = new o::Track("inside");
    append(inside, new o::Gap(RationalTime(4, rate)));
    auto camera_a = new o::Clip("A", nullptr, frames(200, 8, rate));
    append(inside, camera_a);
    append(inside, new o::Gap(RationalTime(8, rate)));
    append(nested, inside);

    auto top = new o::Track("top");
    append(top, nested);
    append(scene, bottom);
    append(scene, top);

    // Markers are projected from their owner's coordinates onto the result.
    nested->markers().push_back(new o::Marker("overlay", frames(0, 12, rate)));
    auto camera_b = dynamic_cast<o::Clip*>(bottom->children()[0].value);
    camera_b->markers().push_back(new o::Marker("b-roll", frames(100, 20, rate)));

    // Flatten the whole hierarchy, nested Stacks included, into one track.
    o::ErrorStatus status;
    o::SerializableObject::Retainer<o::Track> flat(
        o::flatten_stack_recursive(scene, &status));
    if (!flat || o::is_error(status))
    {
        std::cerr << "flatten failed: " << status.full_description << "\n";
        return 1;
    }

    std::cout << "duration: " << flat->duration().value() << " @ "
              << flat->duration().rate() << "\n";
    for (auto const& child: flat->children())
    {
        if (auto clip = dynamic_cast<o::Clip*>(child.value))
        {
            auto source = clip->source_range();
            std::cout << "clip " << clip->name() << " source ["
                      << source->start_time().value() << ", "
                      << source->end_time_exclusive().value() << ")\n";
        }
        else if (auto gap = dynamic_cast<o::Gap*>(child.value))
        {
            std::cout << "gap  " << gap->duration().value() << " frames\n";
        }
    }
    for (auto const& marker: flat->markers())
    {
        auto range = marker->marked_range();
        std::cout << "marker " << marker->name() << " ["
                  << range.start_time().value() << ", "
                  << range.end_time_exclusive().value() << ")\n";
    }

    // The result serializes, round-trips, and trims like any other track.
    std::string json = flat->to_json_string(&status);
    if (o::is_error(status))
    {
        std::cerr << "serialization failed\n";
        return 1;
    }
    std::cout << "serialized " << json.size() << " bytes\n";
    return 0;
}
