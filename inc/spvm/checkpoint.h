#ifndef __SPIRV_VM_CHECKPOINT_H__
#define __SPIRV_VM_CHECKPOINT_H__

#include <spvm/types.h>
#include <spvm/state.h>

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

/* Serializes the current numeric execution scene of `state` into a
 * self-contained little-endian byte buffer.
 *
 * No shader instructions are executed and no host callbacks are invoked,
 * neither while saving nor while restoring. The buffer never contains host
 * pointers, struct memory images, padding, or uninitialized stack capacity.
 * It only describes numeric state supported by the scalar/vector/matrix/
 * array/struct executor (see doc/checkpoint-format.md). Modules using
 * images, samplers, runtime arrays, physical pointers, workgroup storage,
 * atomics, barriers, derivative groups or geometry streams are rejected.
 *
 * On success returns 1 and stores a malloc'd buffer in *data and its size in
 * *size; the caller owns the buffer and must release it with free(). On
 * failure returns 0 and, whenever an output argument is non-NULL, stores
 * NULL / 0 there.
 *
 * A state may be captured after spvm_state_prepare(), after any completed
 * spvm_state_step_opcode(), or after the function has returned. */
int spvm_checkpoint_save(spvm_state_t state, spvm_byte** data, size_t* size);

/* Restores a checkpoint produced by spvm_checkpoint_save() into `state`,
 * which must be attached to a reload of the exact same SPIR-V module (the
 * full instruction stream is checksum matched). The target state may be a
 * freshly created object for the reloaded module or an object that has
 * already executed; its address and owning program are never changed.
 *
 * The blob is fully validated before any target field is modified: on
 * failure the target's values, aliases, execution position and host bindings
 * are left untouched and 0 is returned. Host bindings (user_data, analyzer,
 * callbacks, extension function tables) are never stored nor replaced.
 * Numeric extensions such as GLSL.std.450 stay registered by the caller.
 * Returns 1 on success. */
int spvm_checkpoint_restore(spvm_state_t state, const spvm_byte* data, size_t size);

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // __SPIRV_VM_CHECKPOINT_H__
