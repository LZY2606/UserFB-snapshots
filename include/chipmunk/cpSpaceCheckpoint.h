/* Copyright (c) 2013 Scott Lembcke and Howling Moon Software
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/// @defgroup cpSpaceCheckpoint cpSpaceCheckpoint
/// In-process checkpoints for branching deterministic simulations.
///
/// A checkpoint captures the full simulation state of a regular cpSpace at a
/// single instant (normally between two calls to cpSpaceStep()) so that several
/// input sequences can be tried out and the space can be wound back to the
/// captured instant. Checkpoints live entirely in process memory; they are not
/// serialized to disk and cannot be shared between spaces.
///
/// @section cpsc_lifecycle Lifecycle
///
///   - cpSpaceCheckpointNew() allocates a checkpoint and returns NULL on failure
///     (out of memory, or a space configuration that is not supported).
///   - cpSpaceCheckpointRestore() rewinds the space to the captured instant. It
///     returns cpFalse and leaves the simulation state untouched if the
///     checkpoint cannot be applied.
///   - cpSpaceCheckpointFree() releases a checkpoint. It does not require the
///     originating space to still exist and never touches the space or any
///     other checkpoint.
///
/// Any number of checkpoints may coexist. Branching a simulation never modifies
/// an existing checkpoint, and releasing one checkpoint does not affect the
/// others.
///
/// @section cpsc_support Support scope
///
/// Checkpoints are only supported by spaces created with cpSpaceNew(). Spaces
/// created with cpHastySpaceNew(), spaces switched to a custom spatial index
/// (cpSpaceUseSpatialHash()), and constraints whose implementation class is
/// not one of the built-in Chipmunk joint types cannot be checkpointed;
/// cpSpaceCheckpointNew() returns NULL for them.
///
/// The structure of the space is fixed for the lifetime of a checkpoint. After
/// a checkpoint is taken it is an error to add or remove bodies, shapes or
/// constraints, to change a body's type, mass, moment or center of gravity, a
/// shape's geometry, material (elasticity, friction, surface velocity),
/// collision type, filter or sensor flag, a constraint's parameters, collision
/// handler functions or user data, or any space parameter. Such changes
/// invalidate every checkpoint of the space; cpSpaceCheckpointRestore() then
/// refuses the checkpoint even if the original objects and values are put back
/// afterwards. The dynamic state that is meant to vary between branches
/// (body position, velocity, angle, angular velocity, accumulated force and
/// torque, and the internal solver/contact state) may be changed freely.
///
/// Checkpoints do not deep-copy callback function pointers or userData
/// pointers. Side effects performed inside application callbacks are the
/// application's own responsibility; no collision handler, constraint handler
/// or post-step callback is invoked while taking or restoring a checkpoint.
/// @{

#ifndef CP_SPACE_CHECKPOINT_H
#define CP_SPACE_CHECKPOINT_H

#ifdef __cplusplus
extern "C" {
#endif

/// Opaque handle to a captured cpSpace simulation state.
typedef struct cpSpaceCheckpoint cpSpaceCheckpoint;

/// Capture the current simulation state of @c space.
/// Checkpoints may only be taken while the space is unlocked and no post-step
/// callbacks are queued. Returns NULL on allocation failure or when the space
/// configuration is not supported.
CP_EXPORT cpSpaceCheckpoint *cpSpaceCheckpointNew(cpSpace *space);

/// Rewind @c space to the instant captured by @c checkpoint.
/// The checkpoint must belong to @c space, the space must be unlocked with an
/// empty post-step queue, and the space's structure, fixed object parameters
/// and collision handlers must still match what was captured. Returns cpFalse
/// without modifying the current simulation state on any mismatch, cpTrue on a
/// successful restore.
CP_EXPORT cpBool cpSpaceCheckpointRestore(cpSpace *space, const cpSpaceCheckpoint *checkpoint);

/// Destroy a checkpoint and release all of its memory.
/// Safe to call after the originating space has been freed; never touches the
/// space or any other checkpoint.
CP_EXPORT void cpSpaceCheckpointFree(cpSpaceCheckpoint *checkpoint);

/// @}

#ifdef __cplusplus
}
#endif

#endif
