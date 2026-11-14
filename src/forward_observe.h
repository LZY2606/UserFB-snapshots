#ifndef SPVM_FORWARD_OBSERVE_H
#define SPVM_FORWARD_OBSERVE_H

/* Internal observer interface used by the interpreter loop.  This is not part
 * of the public API (users include <spvm/forward.h>). */

#include <spvm/types.h>
#include <spvm/spirv.h>

/* Called just before an instruction executes.  `operands` points at the first
 * operand word (result type for instructions that have one).  Records a
 * differential error when the instruction is an unsupported floating-point
 * computation.  No-op when no forward context is bound to the state. */
void spvm_forward_observe_pre(struct spvm_state* state, SpvOp opcode,
	spvm_word word_count, spvm_source operands);

/* Called just after an instruction executed.  Propagates tangents. */
void spvm_forward_observe_post(struct spvm_state* state, SpvOp opcode,
	spvm_word word_count, spvm_source operands);

/* Called by spvm_state_pop_function_stack after the numeric return value has
 * been copied into the caller's result slot. */
void spvm_forward_observe_return(struct spvm_state* state, spvm_word store_id,
	spvm_word value_id);

/* Called by spvm_state_prepare: a new run starts from a clean derivative
 * slate (numeric values and host bindings are untouched). */
void spvm_forward_observe_prepare(struct spvm_state* state);

/* Called by spvm_state_delete so a bound context does not dangle. */
void spvm_forward_observe_state_deleted(struct spvm_state* state);

#endif
