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

/// @brief Recursively flatten an arbitrarily nested stack to a single track.
///
/// Unlike flatten_stack(), nested Stack and Track objects are fully resolved
/// into their visible leaf clips instead of being copied as opaque items.
/// Disabled objects, Gaps and transparent regions of nested containers are
/// treated as empty. The returned track has the same duration as the input;
/// transparent regions are filled with Gap objects.
///
/// The caller owns the returned object according to the usual Retainer rules.
///
/// @param input The stack to flatten.
/// @param error_status The return status.
OTIO_API Track* flatten_stack_recursive(
    Stack*       input,
    ErrorStatus* error_status = nullptr);

}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS
