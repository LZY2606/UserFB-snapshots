//
// Copyright (C) 2026 Google, Inc.
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
//    Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//
//    Redistributions in binary form must reproduce the above
//    copyright notice, this list of conditions and the following
//    disclaimer in the documentation and/or other materials provided
//    with the distribution.
//
//    Neither the name of Google Inc. nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//

//
// Optional constant-argument specialization pass over the typed IR of a
// single, already linked, compilation unit.
//
// Calls to user functions whose 'const in' int/uint/bool scalar parameters
// are fed by plain constant arguments are rewritten to call a specialized
// clone of the callee in which those parameters are removed and their uses
// replaced by the constant. The original function is kept for callers with
// dynamic arguments. This is purely an IR-to-IR transform; it neither
// re-parses source text nor runs any external optimizer.
//

#pragma once

#ifndef GLSLANG_CONSTANT_ARGUMENT_SPECIALIZATION_H
#define GLSLANG_CONSTANT_ARGUMENT_SPECIALIZATION_H

namespace glslang {

class TIntermediate;

// Counters describing what one call to SpecializeConstantArguments() did.
// Both are reset to zero at the start of every call.
struct SpecializationStats {
    unsigned createdVersions = 0;  // specialized function versions created by this call
    unsigned rewrittenCalls = 0;   // call sites repointed at a (new or existing) version
};

// Specialize constant arguments of user-function calls reachable from the
// entry point of 'intermediate', which must be a successfully linked
// single-stage intermediate.
//
// 'maxVersions' is the total number of specialized versions the intermediate
// may hold after the call, counting versions left behind by earlier calls.
// Once the budget is spent, existing versions are still reused, but no new
// ones are created; a budget of zero leaves the IR unchanged.
void SpecializeConstantArguments(TIntermediate& intermediate, unsigned maxVersions,
                                 SpecializationStats& stats);

} // end namespace glslang

#endif // GLSLANG_CONSTANT_ARGUMENT_SPECIALIZATION_H
