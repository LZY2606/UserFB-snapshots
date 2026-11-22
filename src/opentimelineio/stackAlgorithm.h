// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#pragma once

#include "opentimelineio/stack.h"
#include "opentimelineio/track.h"
#include "opentimelineio/version.h"

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {

/// @brief Flatten a stack down to a single track.
OTIO_API Track*
flatten_stack(Stack* in_stack, ErrorStatus* error_status = nullptr);

/// @brief Flatten a list of tracks down to a single track.
OTIO_API Track* flatten_stack(
    std::vector<Track*> const& tracks,
    ErrorStatus*               error_status = nullptr);

/// @brief Recursively flatten nested stacks, tracks, clips and gaps down to
/// a single editable track of Clips and Gaps.
///
/// The returned track is owned by the caller under the usual Retainer rules.
/// On failure (unsupported child type, time effect, transition, invalid
/// time) nullptr is returned and error_status is set; no partial result is
/// produced.
OTIO_API Track* flatten_stack_recursive(
    Stack*       input,
    ErrorStatus* error_status = nullptr);

}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS
