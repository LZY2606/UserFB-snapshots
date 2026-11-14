//
// Copyright (C) 2026 The Khronos Group Inc.
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
//    Neither the name of 3Dlabs Inc. Ltd. nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
// OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
//

//
// Optional constant-argument specialization pass operating on the typed IR
// of a successfully linked single-stage intermediate.
//
// Calls to user functions that pass plain compile-time constant nodes for
// "const in" int/uint/bool scalar parameters can be redirected to a
// specialized clone of the callee in which those parameters are replaced by
// the constant and dropped from the parameter list.  The original function
// is kept for remaining dynamic call sites.  Nothing about the default
// compilation path changes; the pass only runs when explicitly invoked.
//

#ifndef GLSLANG_MACHINEINDEPENDENT_CONSTANTARGUMENTSPECIALIZATION_H
#define GLSLANG_MACHINEINDEPENDENT_CONSTANTARGUMENTSPECIALIZATION_H

namespace glslang {

class TIntermediate;

// Outcome of a single SpecializeConstantArguments() call.  Both counters
// are reset to zero at the start of every call.
struct SpecializationStats {
    unsigned createdVersions = 0;  // specialized function versions created by this call
    unsigned rewrittenCalls = 0;   // call sites redirected to a specialized version
};

// Specialize user-function calls whose "const in" int/uint/bool scalar
// arguments are plain constant nodes.
//
//  - 'intermediate' must hold a successfully linked single-stage AST.
//  - 'maxVersions' is the total number of specialized versions the
//    intermediate may hold, including ones created by earlier calls;
//    it is not a per-function limit.  A budget of zero leaves the IR
//    unchanged.  Once the budget is spent, calls matching an already
//    existing version are still rewritten to reuse it; all other calls
//    are left alone.
//  - 'stats' is zeroed on entry and reports what this call did.
//
// Scanning starts at the entry point and follows the call graph in the
// original child order of each function body; functions unreachable from
// the entry point are not expanded.  Generated versions have stable,
// non-conflicting names, so repeating a call with the same budget creates
// no additional copies, and raising the budget continues where a previous
// call stopped.
void SpecializeConstantArguments(TIntermediate& intermediate, unsigned maxVersions, SpecializationStats& stats);

} // namespace glslang

#endif // GLSLANG_MACHINEINDEPENDENT_CONSTANTARGUMENTSPECIALIZATION_H
