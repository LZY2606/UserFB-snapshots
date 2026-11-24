/*
 * Task regression for optional ARM/Thumb continuous IT context.
 *
 * This translation unit is linked with task_support/driver.c, which provides
 * main() and runs the Origin, Continuity and Isolation cases below.
 * The only analysis-program entry is therefore a registered task case.
 */
#include "../task_support/check.h"

#include <capstone/capstone.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FIXED_CODE_LEN 8
#define ROW_CAPACITY 32

static const uint8_t fixed_code[FIXED_CODE_LEN] = {
	0x0c, 0xbf, 0x01, 0x20, 0x02, 0x21, 0x03, 0x22
};

/* A value copy: cs_disasm output may be freed before comparisons/reporting. */
typedef struct thumb_it_row {
	uint64_t address;
	uint8_t bytes[24];
	uint16_t size;
	char mnemonic[CS_MNEMONIC_SIZE];
	char op_str[CS_MNEMONIC_SIZE];
	int cc;
	bool update_flags;
	bool present;
	bool is_it;
	uint64_t block_address;
	uint8_t mask;
	uint8_t slot_index;
	uint8_t slot_count;
	int first_cc;
} thumb_it_row;

typedef struct thumb_it_sink {
	thumb_it_row rows[ROW_CAPACITY];
	size_t count;
} thumb_it_sink;

static csh open_thumb_handle(bool it_context)
{
	csh handle = 0;
	if (cs_open(CS_ARCH_ARM, CS_MODE_THUMB, &handle) != CS_ERR_OK)
		return 0;
	if (cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK) {
		cs_close(&handle);
		return 0;
	}
	if (it_context &&
		cs_option(handle, CS_OPT_ARM_IT_CONTEXT, CS_OPT_ON) != CS_ERR_OK) {
		cs_close(&handle);
		return 0;
	}
	return handle;
}

static void write_tsv_header(FILE *out)
{
	fputs("address\tbytes\tsize\tmnemonic\top_str\tcc\tupdate_flags\t"
	      "it_present\tit_is_it\tit_block_address\tit_mask\t"
	      "it_slot_index\tit_slot_count\tit_first_cc\n", out);
}

/*
 * The shared consumer.  It can write the canonical TSV and/or retain semantic
 * rows for comparison of complete and chunked disassembly paths.
 */
static int thumb_it_consume(FILE *out, const cs_insn *insns, size_t count,
	thumb_it_sink *sink)
{
	size_t i;

	if (!insns || count == 0)
		return -1;
	if (sink && sink->count + count > ROW_CAPACITY)
		return -1;

	for (i = 0; i < count; ++i) {
		const cs_insn *insn = &insns[i];
		const cs_arm *arm;
		const cs_arm_it *it;
		char bytes[3 * sizeof(insn->bytes)];
		size_t at = 0;
		unsigned int j;

		if (!insn->detail)
			return -2;
		arm = &insn->detail->arm;
		it = &arm->it;

		bytes[0] = '\0';
		for (j = 0; j < insn->size && j < sizeof(insn->bytes); ++j) {
			int written = snprintf(bytes + at, sizeof(bytes) - at,
				at == 0 ? "%02x" : " %02x", insn->bytes[j]);
			if (written < 0 || (size_t)written >= sizeof(bytes) - at)
				return -3;
			at += (size_t)written;
		}

		if (out) {
			int written = fprintf(out,
				"0x%llx\t%s\t%u\t%s\t%s\t%u\t%u\t%u\t%u\t"
				"0x%llx\t0x%02x\t%u\t%u\t%u\n",
				(unsigned long long)insn->address, bytes,
				(unsigned)insn->size, insn->mnemonic, insn->op_str,
				(unsigned)arm->cc, (unsigned)arm->update_flags,
				(unsigned)it->present, (unsigned)it->is_it,
				(unsigned long long)it->block_address,
				(unsigned)it->mask, (unsigned)it->slot_index,
				(unsigned)it->slot_count, (unsigned)it->first_cc);
			if (written < 0)
				return -4;
		}

		if (sink) {
			thumb_it_row *row = &sink->rows[sink->count++];
			memset(row, 0, sizeof(*row));
			row->address = insn->address;
			memcpy(row->bytes, insn->bytes, sizeof(row->bytes));
			row->size = insn->size;
			snprintf(row->mnemonic, sizeof(row->mnemonic), "%s",
				insn->mnemonic);
			snprintf(row->op_str, sizeof(row->op_str), "%s",
				insn->op_str);
			row->cc = arm->cc;
			row->update_flags = arm->update_flags;
			row->present = it->present;
			row->is_it = it->is_it;
			row->block_address = it->block_address;
			row->mask = it->mask;
			row->slot_index = it->slot_index;
			row->slot_count = it->slot_count;
			row->first_cc = it->first_cc;
		}
	}

	return 0;
}

static int rows_equal(const thumb_it_row *a, const thumb_it_row *b)
{
	return a->address == b->address &&
		a->size == b->size &&
		memcmp(a->bytes, b->bytes, sizeof(a->bytes)) == 0 &&
		strcmp(a->mnemonic, b->mnemonic) == 0 &&
		strcmp(a->op_str, b->op_str) == 0 &&
		a->cc == b->cc &&
		a->update_flags == b->update_flags &&
		a->present == b->present &&
		a->is_it == b->is_it &&
		a->block_address == b->block_address &&
		a->mask == b->mask &&
		a->slot_index == b->slot_index &&
		a->slot_count == b->slot_count &&
		a->first_cc == b->first_cc;
}

static int sinks_equal(const thumb_it_sink *a, const thumb_it_sink *b)
{
	size_t i;
	TASK_CHECK(a->count == b->count);
	for (i = 0; i < a->count; ++i)
		TASK_CHECK(rows_equal(&a->rows[i], &b->rows[i]));
	return 0;
}

/* Exact semantic contract for 0c bf 01 20 02 21 03 22. */
static int check_fixed_semantics(const cs_insn *ins, size_t n,
	uint64_t base)
{
	const cs_arm *arm;
	const cs_arm_it *it;

	TASK_CHECK(n == 4);
	TASK_CHECK(ins[0].detail && ins[1].detail && ins[2].detail && ins[3].detail);

	TASK_CHECK(ins[0].id == ARM_INS_IT);
	TASK_CHECK(ins[0].address == base);
	TASK_CHECK(ins[0].size == 2);
	TASK_CHECK(strcmp(ins[0].mnemonic, "ite") == 0);
	arm = &ins[0].detail->arm;
	it = &arm->it;
	TASK_CHECK(it->present && it->is_it);
	TASK_CHECK(it->block_address == base);
	TASK_CHECK(it->mask == 0x0c);
	TASK_CHECK(it->slot_index == 0);
	TASK_CHECK(it->slot_count == 2);
	TASK_CHECK(it->first_cc == ARM_CC_EQ);
	TASK_CHECK(arm->cc == ARM_CC_EQ);

	TASK_CHECK(ins[1].id == ARM_INS_MOV);
	TASK_CHECK(ins[1].address == base + 2);
	TASK_CHECK(ins[1].size == 2);
	TASK_CHECK(strcmp(ins[1].mnemonic, "moveq") == 0);
	arm = &ins[1].detail->arm;
	it = &arm->it;
	TASK_CHECK(arm->cc == ARM_CC_EQ);
	TASK_CHECK(!arm->update_flags);
	TASK_CHECK(it->present && !it->is_it);
	TASK_CHECK(it->block_address == base);
	TASK_CHECK(it->mask == 0x0c);
	TASK_CHECK(it->slot_index == 1);
	TASK_CHECK(it->slot_count == 2);
	TASK_CHECK(it->first_cc == ARM_CC_EQ);

	TASK_CHECK(ins[2].id == ARM_INS_MOV);
	TASK_CHECK(ins[2].address == base + 4);
	TASK_CHECK(ins[2].size == 2);
	TASK_CHECK(strcmp(ins[2].mnemonic, "movne") == 0);
	arm = &ins[2].detail->arm;
	it = &arm->it;
	TASK_CHECK(arm->cc == ARM_CC_NE);
	TASK_CHECK(!arm->update_flags);
	TASK_CHECK(it->present && !it->is_it);
	TASK_CHECK(it->block_address == base);
	TASK_CHECK(it->mask == 0x0c);
	TASK_CHECK(it->slot_index == 2);
	TASK_CHECK(it->slot_count == 2);
	TASK_CHECK(it->first_cc == ARM_CC_EQ);

	TASK_CHECK(ins[3].id == ARM_INS_MOV);
	TASK_CHECK(ins[3].address == base + 6);
	TASK_CHECK(ins[3].size == 2);
	TASK_CHECK(strcmp(ins[3].mnemonic, "movs") == 0);
	arm = &ins[3].detail->arm;
	it = &arm->it;
	TASK_CHECK(arm->cc == ARM_CC_AL);
	TASK_CHECK(arm->update_flags);
	TASK_CHECK(!it->present && !it->is_it);
	TASK_CHECK(it->block_address == 0);
	TASK_CHECK(it->mask == 0);
	TASK_CHECK(it->slot_index == 0);
	TASK_CHECK(it->slot_count == 0);
	TASK_CHECK(it->first_cc == 0);

	return 0;
}

static int read_file_text(const char *path, char *buffer, size_t capacity)
{
	FILE *in = fopen(path, "r");
	size_t length;

	if (!in)
		return -1;
	length = fread(buffer, 1, capacity - 1, in);
	if (ferror(in)) {
		fclose(in);
		return -2;
	}
	buffer[length] = '\0';
	fclose(in);
	return (int)length;
}

/* Origin group: full batch -> shared consumer -> analysis/thumb-it.tsv. */
static int origin_writes_and_checks_tsv(void)
{
	csh handle = open_thumb_handle(true);
	cs_insn *ins = NULL;
	size_t count;
	thumb_it_sink sink;
	FILE *out;
	char text[8192];
	int rc;

	TASK_CHECK(handle != 0);
	count = cs_disasm(handle, fixed_code, sizeof(fixed_code), 0x1000,
		0, &ins);
	TASK_CHECK(count == 4);
	TASK_CHECK(check_fixed_semantics(ins, count, 0x1000) == 0);

	memset(&sink, 0, sizeof(sink));
	out = fopen("analysis/thumb-it.tsv", "w");
	TASK_CHECK(out != NULL);
	write_tsv_header(out);
	rc = thumb_it_consume(out, ins, count, &sink);
	TASK_CHECK(fclose(out) == 0);
	TASK_CHECK(rc == 0);
	TASK_CHECK(sink.count == 4);
	cs_free(ins, count);
	cs_close(&handle);

	rc = read_file_text("analysis/thumb-it.tsv", text, sizeof(text));
	TASK_CHECK(rc > 0);
	TASK_CHECK(strstr(text,
		"0x1000\t0c bf\t2\tite\teq\t1\t0\t1\t1\t"
		"0x1000\t0x0c\t0\t2\t1\n") != NULL);
	TASK_CHECK(strstr(text,
		"0x1002\t01 20\t2\tmoveq\tr0, #1\t1\t0\t1\t0\t"
		"0x1000\t0x0c\t1\t2\t1\n") != NULL);
	TASK_CHECK(strstr(text,
		"0x1004\t02 21\t2\tmovne\tr1, #2\t2\t0\t1\t0\t"
		"0x1000\t0x0c\t2\t2\t1\n") != NULL);
	TASK_CHECK(strstr(text,
		"0x1006\t03 22\t2\tmovs\tr2, #3\t15\t1\t0\t0\t"
		"0x0\t0x00\t0\t0\t0\n") != NULL);
	TASK_CHECK(strstr(text, "it_block_address") != NULL);

	return 0;
}

static int full_batch_sink(uint64_t base, thumb_it_sink *sink)
{
	csh handle = open_thumb_handle(true);
	cs_insn *ins = NULL;
	size_t count;
	int rc;

	TASK_CHECK(handle != 0);
	count = cs_disasm(handle, fixed_code, sizeof(fixed_code), base, 0, &ins);
	TASK_CHECK(count == 4);
	TASK_CHECK(check_fixed_semantics(ins, count, base) == 0);
	memset(sink, 0, sizeof(*sink));
	rc = thumb_it_consume(NULL, ins, count, sink);
	TASK_CHECK(rc == 0);
	cs_free(ins, count);
	cs_close(&handle);
	return 0;
}

static size_t test_skipdata_callback(const uint8_t *code, size_t code_size,
	size_t offset, void *user_data)
{
	(void)code;
	(void)code_size;
	(void)offset;
	(void)user_data;
	return 2;
}

/* Continuity group: mixed entries, moved buffers, gaps, truncation, skipdata. */
static int continuity_across_entries(void)
{
	thumb_it_sink expected, chunked;
	csh handle = open_thumb_handle(true);
	cs_insn *it_only = NULL, *rest = NULL, *one = NULL;
	size_t n;
	const uint8_t *cursor;
	size_t remaining;
	uint64_t pc;
	uint8_t moved_tail[6];
	static const uint8_t mixed[8] = {
		0x0c, 0xbf, 0x4f, 0xf0, 0x00, 0x01, 0x02, 0x21
	};
	static const uint8_t skip_code[6] = {
		0x0c, 0xbf, 0x00, 0xf0, 0x03, 0x22
	};
	cs_opt_skipdata skip_setup;
	FILE *gap_out;
	cs_insn *gap = NULL;
	size_t gap_count;

	TASK_CHECK(full_batch_sink(0x1000, &expected) == 0);
	TASK_CHECK(handle != 0);

	/* cs_disasm count=1 consumes only the IT instruction. */
	n = cs_disasm(handle, fixed_code, sizeof(fixed_code), 0x1000, 1,
		&it_only);
	TASK_CHECK(n == 1);
	TASK_CHECK(it_only != NULL);
	TASK_CHECK(it_only[0].id == ARM_INS_IT);
	memset(&chunked, 0, sizeof(chunked));
	TASK_CHECK(thumb_it_consume(NULL, it_only, 1, &chunked) == 0);
	cs_free(it_only, 1);

	/* cs_disasm_iter takes the first controlled MOV. */
	one = cs_malloc(handle);
	TASK_CHECK(one != NULL);
	cursor = fixed_code + 2;
	remaining = sizeof(fixed_code) - 2;
	pc = 0x1002;
	TASK_CHECK(cs_disasm_iter(handle, &cursor, &remaining, &pc, one));
	TASK_CHECK(cursor == fixed_code + 4);
	TASK_CHECK(remaining == 4);
	TASK_CHECK(pc == 0x1004);
	TASK_CHECK(one->detail->arm.cc == ARM_CC_EQ);
	TASK_CHECK(one->detail->arm.it.slot_index == 1);
	TASK_CHECK(thumb_it_consume(NULL, one, 1, &chunked) == 0);
	cs_free(one, 1);

	/* A count=0 batch reads the remaining NE and AL instructions. */
	n = cs_disasm(handle, fixed_code + 4, 4, 0x1004, 0, &rest);
	TASK_CHECK(n == 2);
	TASK_CHECK(thumb_it_consume(NULL, rest, n, &chunked) == 0);
	cs_free(rest, n);
	TASK_CHECK(sinks_equal(&expected, &chunked) == 0);

	/* Storage may move; identity is the original IT virtual address. */
	n = cs_disasm(handle, fixed_code, 2, 0x1000, 1, &it_only);
	TASK_CHECK(n == 1);
	cs_free(it_only, 1);
	memcpy(moved_tail, fixed_code + 2, sizeof(moved_tail));
	one = cs_malloc(handle);
	TASK_CHECK(one != NULL);
	cursor = moved_tail;
	remaining = sizeof(moved_tail);
	pc = 0x1002;
	TASK_CHECK(cs_disasm_iter(handle, &cursor, &remaining, &pc, one));
	TASK_CHECK(cursor == moved_tail + 2);
	TASK_CHECK(pc == 0x1004);
	TASK_CHECK(one->detail->arm.it.present);
	TASK_CHECK(!one->detail->arm.it.is_it);
	TASK_CHECK(one->detail->arm.it.block_address == 0x1000);
	TASK_CHECK(one->detail->arm.it.slot_index == 1);
	cs_free(one, 1);

	/* A PC gap starts a fresh, non-predicated decode. */
	gap_count = cs_disasm(handle, fixed_code + 4, 4, 0x1800, 0, &gap);
	TASK_CHECK(gap_count == 2);
	TASK_CHECK(gap[0].address == 0x1800);
	TASK_CHECK(strcmp(gap[0].mnemonic, "movs") == 0);
	TASK_CHECK(gap[0].detail->arm.cc == ARM_CC_AL);
	TASK_CHECK(gap[0].detail->arm.update_flags);
	TASK_CHECK(!gap[0].detail->arm.it.present);
	TASK_CHECK(gap[0].detail->arm.it.block_address == 0);
	TASK_CHECK(gap[1].detail->arm.cc == ARM_CC_AL);
	gap_out = fopen("analysis/thumb-it-disconnect.tsv", "w");
	TASK_CHECK(gap_out != NULL);
	write_tsv_header(gap_out);
	TASK_CHECK(thumb_it_consume(gap_out, gap, gap_count, NULL) == 0);
	TASK_CHECK(fclose(gap_out) == 0);
	cs_free(gap, gap_count);

	/* Mixed 16-bit IT + 32-bit slot + 16-bit slot. */
	n = cs_disasm(handle, mixed, sizeof(mixed), 0x4000, 1, &it_only);
	TASK_CHECK(n == 1);
	TASK_CHECK(it_only[0].detail->arm.it.slot_count == 2);
	cs_free(it_only, 1);
	one = cs_malloc(handle);
	TASK_CHECK(one != NULL);

	/* Empty input does not advance the cursor or consume the slot. */
	cursor = mixed + 2;
	remaining = 0;
	pc = 0x4002;
	TASK_CHECK(!cs_disasm_iter(handle, &cursor, &remaining, &pc, one));
	TASK_CHECK(cursor == mixed + 2);
	TASK_CHECK(remaining == 0);
	TASK_CHECK(pc == 0x4002);

	/* Half of a 32-bit instruction also fails without consuming the slot. */
	cursor = mixed + 2;
	remaining = 2;
	pc = 0x4002;
	TASK_CHECK(!cs_disasm_iter(handle, &cursor, &remaining, &pc, one));
	TASK_CHECK(cursor == mixed + 2);
	TASK_CHECK(remaining == 2);
	TASK_CHECK(pc == 0x4002);

	/* Completing the bytes at the same PC yields slot 1/EQ. */
	{
		uint8_t complete32[4];
		memcpy(complete32, mixed + 2, sizeof(complete32));
		cursor = complete32;
		remaining = sizeof(complete32);
		pc = 0x4002;
		TASK_CHECK(cs_disasm_iter(handle, &cursor, &remaining, &pc, one));
		TASK_CHECK(cursor == complete32 + 4);
		TASK_CHECK(remaining == 0);
		TASK_CHECK(pc == 0x4006);
		TASK_CHECK(one->size == 4);
		TASK_CHECK(strstr(one->mnemonic, "moveq") == one->mnemonic);
		TASK_CHECK(!one->detail->arm.update_flags);
		TASK_CHECK(one->detail->arm.cc == ARM_CC_EQ);
		TASK_CHECK(one->detail->arm.it.slot_index == 1);
		TASK_CHECK(one->detail->arm.it.block_address == 0x4000);
	}
	cs_free(one, 1);

	n = cs_disasm(handle, mixed + 6, 2, 0x4006, 1, &rest);
	TASK_CHECK(n == 1);
	TASK_CHECK(rest[0].size == 2);
	TASK_CHECK(rest[0].detail->arm.cc == ARM_CC_NE);
	TASK_CHECK(rest[0].detail->arm.it.slot_index == 2);
	TASK_CHECK(rest[0].detail->arm.it.block_address == 0x4000);
	cs_free(rest, n);

	/* The same failed-fetch rollback works through a bounded batch call. */
	n = cs_disasm(handle, mixed, 4, 0x4000, 0, &it_only);
	TASK_CHECK(n == 1);
	TASK_CHECK(it_only[0].id == ARM_INS_IT);
	cs_free(it_only, n);
	n = cs_disasm(handle, mixed + 2, 6, 0x4002, 0, &rest);
	TASK_CHECK(n == 2);
	TASK_CHECK(rest[0].size == 4);
	TASK_CHECK(rest[0].detail->arm.cc == ARM_CC_EQ);
	TASK_CHECK(rest[0].detail->arm.it.slot_index == 1);
	TASK_CHECK(rest[1].detail->arm.cc == ARM_CC_NE);
	TASK_CHECK(rest[1].detail->arm.it.slot_index == 2);
	cs_free(rest, n);

	/* skipdata returns a real data item, breaks IT, and uses no slot. */
	memset(&skip_setup, 0, sizeof(skip_setup));
	skip_setup.callback = test_skipdata_callback;
	skip_setup.mnemonic = ".byte";
	TASK_CHECK(cs_option(handle, CS_OPT_SKIPDATA_SETUP,
		(size_t)&skip_setup) == CS_ERR_OK);
	TASK_CHECK(cs_option(handle, CS_OPT_SKIPDATA, CS_OPT_ON) == CS_ERR_OK);
	n = cs_disasm(handle, skip_code, 2, 0x5000, 1, &it_only);
	TASK_CHECK(n == 1);
	cs_free(it_only, 1);
	one = cs_malloc(handle);
	cursor = skip_code + 2;
	remaining = 2;
	pc = 0x5002;
	TASK_CHECK(cs_disasm_iter(handle, &cursor, &remaining, &pc, one));
	TASK_CHECK(one->id == 0);
	TASK_CHECK(one->size == 2);
	TASK_CHECK(strcmp(one->mnemonic, ".byte") == 0);
	TASK_CHECK(cursor == skip_code + 4);
	TASK_CHECK(remaining == 0);
	TASK_CHECK(pc == 0x5004);
	TASK_CHECK(!one->detail->arm.it.present);
	cs_free(one, 1);

	one = cs_malloc(handle);
	cursor = skip_code + 4;
	remaining = 2;
	pc = 0x5004;
	TASK_CHECK(cs_disasm_iter(handle, &cursor, &remaining, &pc, one));
	TASK_CHECK(one->detail->arm.cc == ARM_CC_AL);
	TASK_CHECK(one->detail->arm.update_flags);
	TASK_CHECK(!one->detail->arm.it.present);
	cs_free(one, 1);

	cs_close(&handle);
	return 0;
}

static int check_independent_handle(uint64_t base, uint64_t expected_source)
{
	csh handle = open_thumb_handle(true);
	cs_insn *it_insn = NULL, *mov_insn = NULL;
	const uint8_t *cursor;
	size_t remaining;
	uint64_t pc;
	size_t n;

	TASK_CHECK(handle != 0);
	n = cs_disasm(handle, fixed_code, 2, base, 1, &it_insn);
	TASK_CHECK(n == 1);
	cs_free(it_insn, 1);

	mov_insn = cs_malloc(handle);
	cursor = fixed_code + 2;
	remaining = 2;
	pc = base + 2;
	TASK_CHECK(cs_disasm_iter(handle, &cursor, &remaining, &pc, mov_insn));
	TASK_CHECK(mov_insn->detail->arm.it.block_address == expected_source);
	TASK_CHECK(mov_insn->detail->arm.it.slot_index == 1);
	cs_free(mov_insn, 1);
	cs_close(&handle);
	return 0;
}

/* Isolation group: defaults/off, handles, base PC, toggles, mode and endian. */
static int isolation_options_handles_base_endian(void)
{
	csh off = open_thumb_handle(false);
	csh first = 0, second = 0;
	cs_insn *ins = NULL, *one = NULL;
	size_t n;
	const uint8_t *cursor;
	size_t remaining;
	uint64_t pc;
	static const uint8_t be_code[8] = {
		0xbf, 0x0c, 0x20, 0x01, 0x21, 0x02, 0x22, 0x03
	};
	csh be;
	thumb_it_sink base_sink;

	/* A new handle at base 0x2000 moves source identity with input PC. */
	TASK_CHECK(full_batch_sink(0x2000, &base_sink) == 0);
	TASK_CHECK(base_sink.rows[0].block_address == 0x2000);
	TASK_CHECK(base_sink.rows[1].block_address == 0x2000);
	TASK_CHECK(base_sink.rows[1].address == 0x2002);

	/* Handles do not share continuity or source identity. */
	TASK_CHECK(cs_open(CS_ARCH_ARM, CS_MODE_THUMB, &first) == CS_ERR_OK);
	TASK_CHECK(cs_option(first, CS_OPT_DETAIL, CS_OPT_ON) == CS_ERR_OK);
	TASK_CHECK(cs_option(first, CS_OPT_ARM_IT_CONTEXT, CS_OPT_ON) == CS_ERR_OK);
	TASK_CHECK(cs_open(CS_ARCH_ARM, CS_MODE_THUMB, &second) == CS_ERR_OK);
	TASK_CHECK(cs_option(second, CS_OPT_DETAIL, CS_OPT_ON) == CS_ERR_OK);
	TASK_CHECK(cs_option(second, CS_OPT_ARM_IT_CONTEXT, CS_OPT_ON) == CS_ERR_OK);
	n = cs_disasm(first, fixed_code, 2, 0x1000, 1, &ins);
	TASK_CHECK(n == 1);
	cs_free(ins, 1);
	n = cs_disasm(second, fixed_code, 2, 0x2000, 1, &ins);
	TASK_CHECK(n == 1);
	cs_free(ins, 1);
	TASK_CHECK(check_independent_handle(0x1000, 0x1000) == 0);
	one = cs_malloc(second);
	cursor = fixed_code + 2;
	remaining = 2;
	pc = 0x2002;
	TASK_CHECK(cs_disasm_iter(second, &cursor, &remaining, &pc, one));
	TASK_CHECK(one->detail->arm.it.block_address == 0x2000);
	cs_free(one, 1);
	cs_close(&first);
	cs_close(&second);

	/* Default/off preserves ordinary decode fields but never exposes it. */
	TASK_CHECK(off != 0);
	n = cs_disasm(off, fixed_code, sizeof(fixed_code), 0x1000, 0, &ins);
	TASK_CHECK(n == 4);
	TASK_CHECK(ins[1].detail->arm.cc == ARM_CC_EQ);
	TASK_CHECK(!ins[1].detail->arm.update_flags);
	TASK_CHECK(ins[2].detail->arm.cc == ARM_CC_NE);
	TASK_CHECK(!ins[2].detail->arm.update_flags);
	TASK_CHECK(ins[3].detail->arm.cc == ARM_CC_AL);
	TASK_CHECK(ins[3].detail->arm.update_flags);
	TASK_CHECK(!ins[0].detail->arm.it.present);
	TASK_CHECK(!ins[1].detail->arm.it.present);
	TASK_CHECK(!ins[2].detail->arm.it.present);
	TASK_CHECK(!ins[3].detail->arm.it.present);
	cs_free(ins, n);

	/* Old cs_disasm-reset/cs_disasm_iter-sticky split remains when off. */
	n = cs_disasm(off, fixed_code, 2, 0x1000, 1, &ins);
	TASK_CHECK(n == 1);
	cs_free(ins, 1);
	one = cs_malloc(off);
	cursor = fixed_code + 2;
	remaining = 2;
	pc = 0x1002;
	TASK_CHECK(cs_disasm_iter(off, &cursor, &remaining, &pc, one));
	TASK_CHECK(one->detail->arm.cc == ARM_CC_EQ);
	TASK_CHECK(!one->detail->arm.it.present);
	cs_free(one, 1);
	n = cs_disasm(off, fixed_code + 4, 2, 0x1004, 1, &ins);
	TASK_CHECK(n == 1);
	TASK_CHECK(strcmp(ins[0].mnemonic, "movs") == 0);
	TASK_CHECK(ins[0].detail->arm.cc == ARM_CC_AL);
	TASK_CHECK(ins[0].detail->arm.update_flags);
	TASK_CHECK(!ins[0].detail->arm.it.present);
	cs_free(ins, 1);
	cs_close(&off);

	/* Repeating ON is a no-op; OFF clears the live predicate stack. */
	TASK_CHECK(cs_open(CS_ARCH_ARM, CS_MODE_THUMB, &first) == CS_ERR_OK);
	TASK_CHECK(cs_option(first, CS_OPT_DETAIL, CS_OPT_ON) == CS_ERR_OK);
	TASK_CHECK(cs_option(first, CS_OPT_ARM_IT_CONTEXT, CS_OPT_ON) == CS_ERR_OK);
	n = cs_disasm(first, fixed_code, 2, 0x1000, 1, &ins);
	TASK_CHECK(n == 1);
	cs_free(ins, 1);
	TASK_CHECK(cs_option(first, CS_OPT_ARM_IT_CONTEXT, CS_OPT_ON) == CS_ERR_OK);
	one = cs_malloc(first);
	cursor = fixed_code + 2;
	remaining = 2;
	pc = 0x1002;
	TASK_CHECK(cs_disasm_iter(first, &cursor, &remaining, &pc, one));
	TASK_CHECK(one->detail->arm.it.slot_index == 1);
	cs_free(one, 1);
	TASK_CHECK(cs_option(first, CS_OPT_ARM_IT_CONTEXT, CS_OPT_OFF) == CS_ERR_OK);
	one = cs_malloc(first);
	cursor = fixed_code + 4;
	remaining = 2;
	pc = 0x1004;
	TASK_CHECK(cs_disasm_iter(first, &cursor, &remaining, &pc, one));
	TASK_CHECK(one->detail->arm.cc == ARM_CC_AL);
	TASK_CHECK(!one->detail->arm.it.present);
	cs_free(one, 1);
	cs_close(&first);

	/* Same mode value does not reset; a real mode change does. */
	TASK_CHECK(cs_open(CS_ARCH_ARM, CS_MODE_THUMB, &first) == CS_ERR_OK);
	TASK_CHECK(cs_option(first, CS_OPT_DETAIL, CS_OPT_ON) == CS_ERR_OK);
	TASK_CHECK(cs_option(first, CS_OPT_ARM_IT_CONTEXT, CS_OPT_ON) == CS_ERR_OK);
	n = cs_disasm(first, fixed_code, 2, 0x1000, 1, &ins);
	TASK_CHECK(n == 1);
	cs_free(ins, 1);
	TASK_CHECK(cs_option(first, CS_OPT_MODE, CS_MODE_THUMB) == CS_ERR_OK);
	one = cs_malloc(first);
	cursor = fixed_code + 2;
	remaining = 2;
	pc = 0x1002;
	TASK_CHECK(cs_disasm_iter(first, &cursor, &remaining, &pc, one));
	TASK_CHECK(one->detail->arm.it.slot_index == 1);
	cs_free(one, 1);

	n = cs_disasm(first, fixed_code, 2, 0x1000, 1, &ins);
	TASK_CHECK(n == 1);
	cs_free(ins, 1);
	TASK_CHECK(cs_option(first, CS_OPT_MODE, CS_MODE_ARM) == CS_ERR_OK);
	TASK_CHECK(cs_option(first, CS_OPT_MODE, CS_MODE_THUMB) == CS_ERR_OK);
	n = cs_disasm(first, fixed_code + 2, 2, 0x1002, 1, &ins);
	TASK_CHECK(n == 1);
	TASK_CHECK(ins[0].detail->arm.cc == ARM_CC_AL);
	TASK_CHECK(ins[0].detail->arm.update_flags);
	TASK_CHECK(!ins[0].detail->arm.it.present);
	cs_free(ins, 1);
	cs_close(&first);

	/* IT state/source progress while DETAIL is off; reopening detail shows it. */
	{
		csh detail_handle = open_thumb_handle(true);
		cs_insn *detail_insn;

		TASK_CHECK(detail_handle != 0);
		n = cs_disasm(detail_handle, fixed_code, 2, 0x1000, 1, &ins);
		TASK_CHECK(n == 1);
		cs_free(ins, 1);

		detail_insn = cs_malloc(detail_handle);
		TASK_CHECK(detail_insn != NULL);
		TASK_CHECK(cs_option(detail_handle, CS_OPT_DETAIL,
			CS_OPT_OFF) == CS_ERR_OK);
		cursor = fixed_code + 2;
		remaining = 2;
		pc = 0x1002;
		TASK_CHECK(cs_disasm_iter(detail_handle, &cursor, &remaining,
			&pc, detail_insn));

		TASK_CHECK(cs_option(detail_handle, CS_OPT_DETAIL,
			CS_OPT_ON) == CS_ERR_OK);
		cursor = fixed_code + 4;
		remaining = 2;
		pc = 0x1004;
		TASK_CHECK(cs_disasm_iter(detail_handle, &cursor, &remaining,
			&pc, detail_insn));
		TASK_CHECK(detail_insn->detail != NULL);
		TASK_CHECK(detail_insn->detail->arm.cc == ARM_CC_NE);
		TASK_CHECK(!detail_insn->detail->arm.update_flags);
		TASK_CHECK(detail_insn->detail->arm.it.present);
		TASK_CHECK(detail_insn->detail->arm.it.slot_index == 2);
		TASK_CHECK(detail_insn->detail->arm.it.block_address == 0x1000);
		TASK_CHECK(detail_insn->detail->arm.it.first_cc == ARM_CC_EQ);
		cs_free(detail_insn, 1);
		cs_close(&detail_handle);
	}

	/* Big-endian Thumb uses the same PC/source relationship. */
	TASK_CHECK(cs_open(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_BIG_ENDIAN,
		&be) == CS_ERR_OK);
	TASK_CHECK(cs_option(be, CS_OPT_DETAIL, CS_OPT_ON) == CS_ERR_OK);
	TASK_CHECK(cs_option(be, CS_OPT_ARM_IT_CONTEXT, CS_OPT_ON) == CS_ERR_OK);
	n = cs_disasm(be, be_code, sizeof(be_code), 0x1000, 0, &ins);
	TASK_CHECK(n == 4);
	TASK_CHECK(check_fixed_semantics(ins, n, 0x1000) == 0);
	TASK_CHECK(ins[1].bytes[0] == 0x20 && ins[1].bytes[1] == 0x01);
	cs_free(ins, n);
	cs_close(&be);

	return 0;
}

const struct task_case task_cases[] = {
	{ "Origin", "thumb_it_tsv_consumer", origin_writes_and_checks_tsv },
	{ "Continuity", "batch_iter_buffer_gap_truncation_skipdata",
	  continuity_across_entries },
	{ "Isolation", "default_off_handles_toggle_mode_base_endian",
	  isolation_options_handles_base_endian },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
