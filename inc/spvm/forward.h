#ifndef __SPIRV_VM_FORWARD_H__
#define __SPIRV_VM_FORWARD_H__

/*
 * Forward-mode automatic differentiation for SPIRV-VM.
 *
 * A forward context is bound to exactly one existing spvm_state.  It runs
 * alongside the ordinary opcode executor: the primal (numeric) results are
 * produced by the normal VM, while analytic directional derivatives are
 * propagated for up to four tangent directions at once.  No finite differencing
 * and no replay of the shader is performed.
 *
 * Usage:
 *     spvm_state_t state = spvm_state_create(prog);
 *     spvm_forward_t fwd = spvm_forward_create(state, 2);
 *     spvm_state_prepare(state, spvm_state_get_result_location(state, "main"));
 *     float seed = 1.0f;
 *     spvm_forward_seed(fwd, result_id, path, path_len, 0, seed);
 *     spvm_state_call_function(state);      // or step_opcode() repeatedly
 *     float out;
 *     spvm_forward_get(fwd, out_id, path, path_len, 0, &out);
 *     spvm_forward_clear(fwd);
 *     spvm_forward_delete(fwd);
 */

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#include <spvm/types.h>
#include <spvm/state.h>

#define SPVM_FORWARD_MAX_DIRECTIONS 4
#define SPVM_FORWARD_NO_ERROR 0

typedef struct spvm_forward spvm_forward;
typedef spvm_forward* spvm_forward_t;

/* Create a forward context for `state` with `direction_count` tangent
 * directions (must be 1..4).  Only one context may be bound to a state at a
 * time.  Returns NULL on failure (bad arguments or already bound). */
spvm_forward_t spvm_forward_create(spvm_state_t state, spvm_word direction_count);

/* Destroy the context.  The state and its numeric values remain valid and
 * ordinary execution (step_opcode / call_function / ...) keeps working. */
void spvm_forward_delete(spvm_forward_t fwd);

/* Remove all seeds, all derivatives and any differential error.  Numeric
 * values, the program counter and host bindings are untouched.  Also called
 * implicitly by spvm_state_prepare() for the bound state. */
void spvm_forward_clear(spvm_forward_t fwd);

/* Seed the scalar float32 leaf addressed by `result_id` plus a member index
 * path.  A scalar is addressed by path length 0 (its implicit member 0).  The
 * path descends through members of vector/array/struct values.
 * Returns 0 on success, non-zero on failure (constant leaf, invalid path,
 * out-of-range direction, no context).  On failure nothing is modified. */
int spvm_forward_seed(spvm_forward_t fwd, spvm_word result_id,
	const spvm_word* path, spvm_word path_len, spvm_word direction, float value);

/* Read the derivative of the scalar float32 leaf addressed by
 * `result_id` + path for the given direction.  Returns 0 on success and stores
 * the value in *out; non-zero on failure (no context, invalid path, direction
 * out of range, or a differential error is pending). */
int spvm_forward_get(spvm_forward_t fwd, spvm_word result_id,
	const spvm_word* path, spvm_word path_len, spvm_word direction, float* out);

/* Read the pending differential error.  Returns 0 if no error.  When non-zero,
 * *instruction receives the instruction counter at which the error was
 * detected (may be NULL).  Derivatives cannot be read while an error is set.
 * The error is cleared by spvm_forward_clear(). */
int spvm_forward_error(spvm_forward_t fwd, spvm_word* instruction);

/* Return the number of tangent directions the context was created with
 * (0 if fwd is NULL). */
spvm_word spvm_forward_direction_count(spvm_forward_t fwd);

/* Return the bound state (or NULL). */
spvm_state_t spvm_forward_state(spvm_forward_t fwd);

/* Query the forward context bound to a state, or NULL if none. */
spvm_forward_t spvm_forward_get_context(spvm_state_t state);

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // __SPIRV_VM_FORWARD_H__
