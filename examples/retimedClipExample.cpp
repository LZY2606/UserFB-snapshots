// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project
//
// Example: build a timeline with a reversed, speed-changed RetimedClip and
// inspect how it lays out in its parent track.
//
//   source range [100, 148) @ 24 fps, playback rate -2
//   10-frame gap before the clip
//   -> the source boundaries 148, 136 and 100 map to program times 10, 16, 34
//
// Build (from the repository root):
//   cmake -S . -B build -DOTIO_CXX_EXAMPLES=ON ...
//   cmake --build build --target retimed_clip_example
//
// or compile directly against the built static library:
//   c++ -std=c++20 -I src -I src/deps/Imath/src \
//       examples/retimedClipExample.cpp \
//       build/src/opentimelineio/libopentimelineio.a ...

#include <opentimelineio/errorStatus.h>
#include <opentimelineio/externalReference.h>
#include <opentimelineio/gap.h>
#include <opentimelineio/retimedClip.h>
#include <opentimelineio/track.h>

#include <iostream>

namespace otio = opentimelineio::OPENTIMELINEIO_VERSION_NS;
using otio::RationalTime;
using otio::TimeRange;

int
main()
{
    constexpr double rate_hz = 24.0;

    otio::ErrorStatus error_status;

    // A track with 10 frames of leader, then our retimed clip.
    auto* track = new otio::Track("example-track");

    auto* media = new otio::ExternalReference(
        "media.mov",
        TimeRange(RationalTime(0, rate_hz), RationalTime(1000, rate_hz)));

    // Source range is a half-open interval in the media: frames [100, 148).
    auto* clip = new otio::RetimedClip(
        "reverse-fast",
        media,
        TimeRange(RationalTime(100, rate_hz), RationalTime(48, rate_hz)),
        /*playback_rate=*/-2.0);

    if (!track->append_child(new otio::Gap(RationalTime(10, rate_hz)), &error_status)
        || !track->append_child(clip, &error_status))
    {
        std::cerr << "failed to build track: " << error_status.full_description
                  << "\n";
        return 1;
    }

    // The clip occupies 48 / |-2| = 24 program frames.
    RationalTime duration = clip->duration(&error_status);

    std::cout << "playback_rate = " << clip->playback_rate() << "\n";
    std::cout << "program duration = " << duration.value() << " @ "
              << duration.rate() << " fps\n";

    // Map three source boundaries into the parent track's time.  Playing
    // backwards starts from the exclusive end of the source range.
    for (double source_value: { 148.0, 136.0, 100.0 })
    {
        RationalTime source_time(source_value, rate_hz);
        RationalTime parent_time =
            clip->transformed_time(source_time, track, &error_status);
        std::cout << "source " << source_value << " -> parent "
                  << parent_time.value() << "\n";
    }

    // Mapping a whole range back the other way keeps a non-negative length.
    TimeRange program_range(RationalTime(14, rate_hz), RationalTime(8, rate_hz));
    TimeRange source_range =
        track->transformed_time_range(program_range, clip, &error_status);
    std::cout << "program [14, 22) -> source ["
              << source_range.start_time().value() << ", "
              << source_range.end_time_exclusive().value() << ")\n";

    bool ok = !otio::is_error(&error_status);
    if (!ok)
    {
        std::cerr << error_status.full_description << "\n";
        return 1;
    }

    return 0;
}
