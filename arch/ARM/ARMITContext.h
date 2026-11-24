/* Capstone Disassembly Engine */
/* Thumb IT-block continuity and provenance */

#ifndef CS_ARM_IT_CONTEXT_H
#define CS_ARM_IT_CONTEXT_H

#include "../../cs_priv.h"

#ifdef CAPSTONE_HAS_ARM

cs_err ARM_it_set_option(cs_struct *h, size_t value);
void ARM_it_mode_changed(cs_struct *h, cs_mode new_mode);
void ARM_it_reset(cs_struct *h);

void ARM_it_fetch_begin(cs_struct *h, uint64_t address,
	ARM_ITCheckpoint *checkpoint);
void ARM_it_fetch_rollback(cs_struct *h,
	const ARM_ITCheckpoint *checkpoint);
void ARM_it_fetch_commit(cs_struct *h, uint64_t address, uint16_t size);
void ARM_it_data_break(cs_struct *h, uint64_t address, size_t size);

void ARM_it_record_it(cs_struct *h, uint64_t address,
	unsigned firstcond, unsigned mask);
void ARM_it_record_controlled_slot(cs_struct *h);

void ARM_it_fill_detail(cs_struct *h, cs_insn *insn);

#else

static inline void ARM_it_fetch_begin(cs_struct *h, uint64_t address,
	ARM_ITCheckpoint *checkpoint) { (void)h; (void)address; (void)checkpoint; }
static inline void ARM_it_fetch_rollback(cs_struct *h,
	const ARM_ITCheckpoint *checkpoint) { (void)h; (void)checkpoint; }
static inline void ARM_it_fetch_commit(cs_struct *h, uint64_t address,
	uint16_t size) { (void)h; (void)address; (void)size; }
static inline void ARM_it_data_break(cs_struct *h, uint64_t address,
	size_t size) { (void)h; (void)address; (void)size; }

#endif

#endif
