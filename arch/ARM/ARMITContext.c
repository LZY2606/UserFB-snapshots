/* Capstone Disassembly Engine */
/* Thumb IT-block continuity and provenance */

#ifdef CAPSTONE_HAS_ARM

#include <string.h>

#include "ARMITContext.h"

static void clear_predicate_state(cs_struct *h)
{
	memset(&h->ITBlock, 0, sizeof(h->ITBlock));
	memset(&h->it_context.source, 0, sizeof(h->it_context.source));
	h->it_context.last_present = false;
	h->it_context.last_is_it = false;
	h->it_context.last_slot = 0;
}

void ARM_it_reset(cs_struct *h)
{
	bool enabled = h->it_context.enabled;

	memset(&h->it_context, 0, sizeof(h->it_context));
	h->it_context.enabled = enabled;
	clear_predicate_state(h);
}

cs_err ARM_it_set_option(cs_struct *h, size_t value)
{
	bool enabled;

	if (value != CS_OPT_OFF && value != CS_OPT_ON)
		return CS_ERR_OPTION;

	enabled = (value == CS_OPT_ON);
	if (h->it_context.enabled == enabled)
		return CS_ERR_OK;

	h->it_context.enabled = enabled;
	ARM_it_reset(h);
	return CS_ERR_OK;
}

void ARM_it_mode_changed(cs_struct *h, cs_mode new_mode)
{
	if (h->mode != new_mode)
		ARM_it_reset(h);
}

void ARM_it_fetch_begin(cs_struct *h, uint64_t address,
	ARM_ITCheckpoint *checkpoint)
{
	ARM_ITContext *it = &h->it_context;

	checkpoint->block = h->ITBlock;
	checkpoint->source = it->source;
	checkpoint->cursor_valid = it->cursor_valid;
	checkpoint->next_pc = it->next_pc;
	checkpoint->last_present = it->last_present;
	checkpoint->last_is_it = it->last_is_it;
	checkpoint->last_slot = it->last_slot;
	checkpoint->contiguous = it->cursor_valid && it->next_pc == address;

	it->last_present = false;
	it->last_is_it = false;
	it->last_slot = 0;

	// A non-contiguous successful fetch starts with no inherited IT state.
	if (!checkpoint->contiguous)
		clear_predicate_state(h);
}

void ARM_it_fetch_rollback(cs_struct *h,
	const ARM_ITCheckpoint *checkpoint)
{
	h->ITBlock = checkpoint->block;
	h->it_context.source = checkpoint->source;
	h->it_context.cursor_valid = checkpoint->cursor_valid;
	h->it_context.next_pc = checkpoint->next_pc;
	h->it_context.last_present = checkpoint->last_present;
	h->it_context.last_is_it = checkpoint->last_is_it;
	h->it_context.last_slot = checkpoint->last_slot;
}

void ARM_it_fetch_commit(cs_struct *h, uint64_t address, uint16_t size)
{
	h->it_context.cursor_valid = true;
	h->it_context.next_pc = address + size;
}

void ARM_it_data_break(cs_struct *h, uint64_t address, size_t size)
{
	clear_predicate_state(h);
	h->it_context.cursor_valid = true;
	h->it_context.next_pc = address + (uint64_t)size;
}

void ARM_it_record_it(cs_struct *h, uint64_t address,
	unsigned firstcond, unsigned mask)
{
	ARM_ITContext *it = &h->it_context;
	ARM_ITSource *source = &it->source;

	if (!it->enabled)
		return;

	source->valid = true;
	source->block_address = address;
	source->mask = (uint8_t)(mask & 0x0fU);
	source->slot_count = (uint8_t)h->ITBlock.size;
	if (source->slot_count == 0)
		source->slot_count = 1;
	source->first_cc = (arm_cc)(firstcond + 1U);

	it->last_present = true;
	it->last_is_it = true;
	it->last_slot = 0;
}

void ARM_it_record_controlled_slot(cs_struct *h)
{
	ARM_ITContext *it = &h->it_context;
	unsigned remaining = h->ITBlock.size;

	if (!it->enabled || remaining == 0)
		return;

	it->last_present = true;
	it->last_is_it = false;

	if (it->source.valid && remaining <= it->source.slot_count)
		it->last_slot = (uint8_t)(it->source.slot_count - remaining + 1U);
}

void ARM_it_fill_detail(cs_struct *h, cs_insn *insn)
{
	cs_arm_it *out;
	ARM_ITContext *it;
	ARM_ITSource *source;

	if (!insn || !insn->detail)
		return;

	out = &insn->detail->arm.it;
	memset(out, 0, sizeof(*out));

	if (!h->it_context.enabled)
		return;

	it = &h->it_context;
	source = &it->source;
	if (!source->valid)
		return;

	if (it->last_is_it && it->last_present) {
		out->present = true;
		out->is_it = true;
		out->block_address = source->block_address;
		out->mask = source->mask;
		out->slot_index = 0;
		out->slot_count = source->slot_count;
		out->first_cc = source->first_cc;
	} else if (it->last_present) {
		out->present = true;
		out->is_it = false;
		out->block_address = source->block_address;
		out->mask = source->mask;
		out->slot_index = it->last_slot;
		out->slot_count = source->slot_count;
		out->first_cc = source->first_cc;
	}
}

#endif
