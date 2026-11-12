#include <spvm/context.h>
#include <spvm/state.h>
#include <spvm/checkpoint.h>
#include <spvm/result.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "suite.h"
#include "continuation.h"
#include "fixtures.h"

#define CHECK(x) do { UNIT_CHECK(x); ++n; } while (0)

#define TAG_MOD 0x31444f4Du /* "MOD1" */
#define TAG_EXEC 0x31434558u /* "XEC1" */
#define TAG_VAL 0x314c4156u /* "VAL1" */
#define TAG_END 0x31444e45u /* "END1" */

#define CONT_ENTRY 20u
#define CONT_WORDS (sizeof(continuation_words)/sizeof(continuation_words[0]))

static uint32_t le32at(const spvm_byte* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put_le32(spvm_byte* p, uint32_t v)
{
	p[0] = (spvm_byte)(v & 0xFF);
	p[1] = (spvm_byte)((v >> 8) & 0xFF);
	p[2] = (spvm_byte)((v >> 16) & 0xFF);
	p[3] = (spvm_byte)((v >> 24) & 0xFF);
}
static size_t find_tag(const spvm_byte* b, size_t n, uint32_t tag)
{
	for (size_t i = 0; i + 4 <= n; i++)
		if (le32at(b + i) == tag)
			return i;
	return (size_t)-1;
}
static spvm_word* dup_words(const spvm_word* w, size_t cnt)
{
	spvm_word* p = (spvm_word*)malloc(cnt * sizeof(spvm_word));
	memcpy(p, w, cnt * sizeof(spvm_word));
	return p;
}

/* every duplicated module word buffer is registered so a group can free
 * it after the owning program and state are deleted */
static spvm_word** g_words;
static size_t g_words_count;
static spvm_word* track_words(const spvm_word* w, size_t cnt)
{
	spvm_word* p = dup_words(w, cnt);
	g_words = (spvm_word**)realloc(g_words, (g_words_count + 1) * sizeof(spvm_word*));
	g_words[g_words_count++] = p;
	return p;
}
static void free_tracked_words(void)
{
	for (size_t i = 0; i < g_words_count; i++)
		free(g_words[i]);
	free(g_words);
	g_words = NULL;
	g_words_count = 0;
}
static spvm_state_t make_state(spvm_context_t c, const spvm_word* w, size_t cnt, spvm_word entry)
{
	spvm_program_t p = spvm_program_create(c, w, cnt);
	if (!p)
		return NULL;
	spvm_state_t s = spvm_state_create(p);
	spvm_state_prepare(s, entry);
	return s;
}
static void delete_state(spvm_state_t s)
{
	spvm_program_t p = s->owner;
	spvm_state_delete(s);
	spvm_program_delete(p);
}
static void pause_at(spvm_state_t s, int opcode, int id)
{
	for (int i = 0; i < 200 && s->code_current; ++i) {
		const spvm_word* pc = s->code_current;
		if ((pc[0] & SpvOpCodeMask) == opcode && pc[2] == id)
			return;
		spvm_state_step_opcode(s);
	}
}
static void finish_steps(spvm_state_t s)
{
	for (int i = 0; i < 200 && s->code_current; ++i)
		spvm_state_step_opcode(s);
}
static int save_blob(spvm_state_t s, spvm_byte** out, size_t* out_n)
{
	*out = NULL;
	*out_n = 0;
	return spvm_checkpoint_save(s, out, out_n);
}

/* Tree nodes are self-describing: skip one without knowing the module type.
 * Returns the offset just past the node, or 0 on malformed encoding. */
static size_t skip_tree(const spvm_byte* b, size_t n, size_t at)
{
	if (at + 2 > n)
		return 0;
	uint32_t kind = b[at];
	if (kind == 1) { /* scalar: kind, width, u64 */
		if (at + 10 > n)
			return 0;
		return at + 10;
	}
	if (kind < 2 || kind > 5)
		return 0;
	if (at + 5 > n)
		return 0;
	uint32_t count = le32at(b + at + 1);
	size_t cur = at + 5;
	for (uint32_t i = 0; i < count; i++) {
		cur = skip_tree(b, n, cur);
		if (cur == 0)
			return 0;
	}
	return cur;
}

/* Locate the alias record for result id inside the VALUES section. */
static size_t find_alias_record(const spvm_byte* b, size_t n, uint32_t id)
{
	size_t v = find_tag(b, n, TAG_VAL);
	if (v == (size_t)-1)
		return (size_t)-1;
	size_t cur = v + 8; /* tag + payload size */
	uint32_t root_count = le32at(b + cur);
	cur += 4;
	for (uint32_t i = 0; i < root_count; i++) {
		cur += 4; /* result id */
		cur = skip_tree(b, n, cur);
		if (cur == 0)
			return (size_t)-1;
	}
	uint32_t alias_count = le32at(b + cur);
	cur += 4;
	for (uint32_t i = 0; i < alias_count; i++) {
		size_t rec = cur;
		uint32_t rid = le32at(b + cur);
		uint32_t path_len = le32at(b + cur + 8);
		cur += 12 + (size_t)path_len * 4;
		if (cur > n)
			return (size_t)-1;
		if (rid == id)
			return rec;
	}
	return (size_t)-1;
}
/* Byte offset of the first encoded value-tree kind byte. */
static size_t first_tree_kind(const spvm_byte* b, size_t n)
{
	size_t v = find_tag(b, n, TAG_VAL);
	if (v == (size_t)-1)
		return (size_t)-1;
	size_t cur = v + 8;
	uint32_t root_count = le32at(b + cur);
	if (root_count == 0)
		return (size_t)-1;
	cur += 4 + 4; /* count + first root id */
	return cur;
}

/* ================================================================== */
/* relocate: cross-reload migration, aliasing, address stability      */
/* ================================================================== */
static int group_relocate(void)
{
	int n = 0;
	spvm_context_t c = spvm_context_initialize();
	size_t cnt = CONT_WORDS;

	/* --- nested alias write across a full module reload --- */
	spvm_state_t a = make_state(c, track_words(continuation_words, cnt), cnt, CONT_ENTRY);
	pause_at(a, SpvOpLoad, 43);
	spvm_byte* blob = NULL; size_t blob_n = 0;
	CHECK(save_blob(a, &blob, &blob_n) == 1);
	CHECK(blob != NULL && blob_n > 0);

	spvm_state_t b = make_state(c, track_words(continuation_words, cnt), cnt, CONT_ENTRY);
	spvm_member_t host22 = b->results[22].members;
	spvm_member_t host14 = b->results[14].members;
	CHECK(spvm_checkpoint_restore(b, blob, blob_n) == 1);

	/* target keeps its own numeric-tree allocations */
	CHECK(b->results[22].members == host22);
	CHECK(b->results[14].members == host14);
	/* aliases rebuilt onto the target tree, not the source one */
	CHECK(b->results[23].members == b->results[22].members);
	CHECK(b->results[31].members == b->results[23].members);
	CHECK(b->results[41].members == b->results[31].members);
	CHECK(b->results[24].members == b->results[22].members + 1);
	CHECK(b->results[41].members != a->results[41].members);
	CHECK(b->results[41].members[0].value.s == 1);
	CHECK(b->results[24].members[0].value.s == 7);

	/* shared aliases: a write through the deepest parameter is visible
	 * through the access chain and the root variable */
	b->results[41].members[0].value.s = 10;
	CHECK(b->results[23].members[0].value.s == 10);
	CHECK(b->results[22].members[0].value.s == 10);
	/* adjacent member is untouched: the two slots never mix */
	CHECK(b->results[22].members[1].value.s == 7);
	finish_steps(b);
	CHECK(!b->code_current);
	CHECK(b->results[14].members[0].value.s == 20);

	/* --- deterministic encoding: identical scene, identical bytes --- */
	spvm_byte* blob_b = NULL; size_t blob_b_n = 0;
	CHECK(save_blob(a, &blob_b, &blob_b_n) == 1);
	CHECK(blob_b_n == blob_n && memcmp(blob, blob_b, blob_n) == 0);
	CHECK(blob_b != blob); /* independent buffers */
	free(blob_b);

	/* --- snapshots never share writable content --- */
	spvm_state_t y = make_state(c, track_words(continuation_words, cnt), cnt, CONT_ENTRY);
	CHECK(spvm_checkpoint_restore(y, blob, blob_n) == 1);
	CHECK(y->results[22].members[0].value.s == 1); /* mutation did not leak */
	finish_steps(y);
	CHECK(y->results[14].members[0].value.s == 11);

	/* --- restore into an already-run target, then a finished one --- */
	spvm_state_t r = make_state(c, track_words(continuation_words, cnt), cnt, CONT_ENTRY);
	pause_at(r, SpvOpPhi, 45);
	CHECK(r->function_stack_current == 1);
	CHECK(spvm_checkpoint_restore(r, blob, blob_n) == 1);
	CHECK(r->function_stack_current == 2);
	CHECK(r->current_function == r->results + 40);
	CHECK(r->results[41].members == r->results[22].members);
	finish_steps(r);
	CHECK(r->results[14].members[0].value.s == 11);

	spvm_state_t f = make_state(c, track_words(continuation_words, cnt), cnt, CONT_ENTRY);
	finish_steps(f);
	CHECK(!f->code_current);
	CHECK(spvm_checkpoint_restore(f, blob, blob_n) == 1);
	CHECK(f->function_stack_current == 2 && f->code_current != NULL);
	finish_steps(f);
	CHECK(f->results[14].members[0].value.s == 11);

	/* --- source state/program/words fully released before restore --- */
	spvm_byte* keep = NULL; size_t keep_n = 0;
	{
		spvm_word* wa = dup_words(continuation_words, cnt);
		spvm_state_t sa = make_state(c, wa, cnt, CONT_ENTRY);
		pause_at(sa, SpvOpLoad, 43);
		CHECK(save_blob(sa, &keep, &keep_n) == 1);
		delete_state(sa);
		free(wa);
	}
	spvm_state_t z = make_state(c, track_words(continuation_words, cnt), cnt, CONT_ENTRY);
	CHECK(spvm_checkpoint_restore(z, keep, keep_n) == 1);
	z->results[41].members[0].value.s = 10;
	finish_steps(z);
	CHECK(z->results[14].members[0].value.s == 20);
	free(keep);

	/* --- rich types: bool, int32, float32/64, vector, matrix, struct --- */
	{
		size_t tn = sizeof(cp_types_words) / sizeof(cp_types_words[0]);
		spvm_state_t t1 = make_state(c, track_words(cp_types_words, tn), tn, 70);

		t1->results[60].members[0].value.s = 1;   /* v3i output */
		t1->results[60].members[1].value.s = -2;
		t1->results[60].members[2].value.s = 3;
		t1->results[62].members[0].value.u64 = UINT64_C(0x3FF8000000000000); /* f64 1.5 */
		t1->results[63].members[0].value.b = 1;  /* bool */
		/* 2x2 matrix, including a negative zero */
		t1->results[65].members[0].members[0].value.f = 1.25f;
		t1->results[65].members[0].members[1].value.f = -0.0f;
		t1->results[65].members[1].members[0].value.f = 3.5f;
		t1->results[65].members[1].members[1].value.f = 4.5f;
		/* int2 fixed array: scalar array slots carry the value directly */
		t1->results[66].members[0].value.s = -12345;
		t1->results[66].members[1].value.s = 6789;
		/* private array of two structs */
		t1->results[61].members[0].members[0].value.s = 5;
		t1->results[61].members[0].members[1].members[0].value.f = 1.5f;
		t1->results[61].members[0].members[1].members[1].value.f = 2.5f;
		t1->results[61].members[1].members[0].value.s = -7;
		t1->results[61].members[1].members[1].members[0].value.f = -0.0f;
		t1->results[61].members[1].members[1].members[1].value.f = 4.0f;

		spvm_byte* tb = NULL; size_t tb_n = 0;
		CHECK(save_blob(t1, &tb, &tb_n) == 1);
		spvm_byte* tb2 = NULL; size_t tb2_n = 0;
		CHECK(save_blob(t1, &tb2, &tb2_n) == 1);
		CHECK(tb_n == tb2_n && memcmp(tb, tb2, tb_n) == 0);
		free(tb2);

		spvm_state_t t2 = make_state(c, track_words(cp_types_words, tn), tn, 70);
		/* capture the target's own tree addresses before restore */
		spvm_member_t host61 = t2->results[61].members;
		spvm_member_t host65 = t2->results[65].members;
		spvm_member_t host66 = t2->results[66].members;
		CHECK(spvm_checkpoint_restore(t2, tb, tb_n) == 1);
		/* tree allocations remain the target's own */
		CHECK(t2->results[61].members == host61);
		CHECK(t2->results[65].members == host65);
		CHECK(t2->results[66].members == host66);
		/* bit-exact values, negative zero and f64 included */
		CHECK(t2->results[60].members[0].value.u == 1u);
		CHECK(t2->results[60].members[1].value.u == (uint32_t)-2);
		CHECK(t2->results[60].members[2].value.u == 3u);
		CHECK(t2->results[62].members[0].value.u64 == UINT64_C(0x3FF8000000000000));
		CHECK(t2->results[63].members[0].value.b == 1);
		CHECK(t2->results[65].members[0].members[0].value.u == 0x3fa00000u);
		CHECK(t2->results[65].members[0].members[1].value.u == 0x80000000u);
		CHECK(t2->results[65].members[1].members[0].value.u == 0x40600000u);
		CHECK(t2->results[65].members[1].members[1].value.u == 0x40900000u);
		CHECK(t2->results[66].members[0].value.s == -12345);
		CHECK(t2->results[66].members[1].value.s == 6789);
		CHECK(t2->results[61].members[0].members[0].value.s == 5);
		CHECK(t2->results[61].members[0].members[1].members[1].value.u == 0x40200000u);
		CHECK(t2->results[61].members[1].members[0].value.s == -7);
		CHECK(t2->results[61].members[1].members[1].members[0].value.u == 0x80000000u);
		free(tb);
		delete_state(t1);
		delete_state(t2);
	}

	delete_state(a); delete_state(b); delete_state(y);
	delete_state(r); delete_state(f); delete_state(z);
	free(blob);
	free_tracked_words();
	spvm_context_deinitialize(c);
	return n;
}

/* ================================================================== */
/* continuation: call/debug resume, phi, prepare/terminal checkpoints */
/* ================================================================== */
static int group_continuation(void)
{
	int n = 0;
	spvm_context_t c = spvm_context_initialize();
	size_t cnt = CONT_WORDS;

	spvm_state_t ref = make_state(c, track_words(continuation_words, cnt), cnt, CONT_ENTRY);
	spvm_state_t paused = make_state(c, track_words(continuation_words, cnt), cnt, CONT_ENTRY);
	pause_at(paused, SpvOpLoad, 43);
	spvm_byte* blob = NULL; size_t blob_n = 0;
	CHECK(save_blob(paused, &blob, &blob_n) == 1);

	/* nested pause scene */
	CHECK(paused->function_stack_current == 2);
	CHECK(paused->current_function == paused->results + 40);
	CHECK(paused->current_line == 27 && paused->current_column == 3);
	CHECK(paused->current_file != NULL);

	/* checkpoint right after prepare: resume point is the function entry */
	spvm_state_t prep = make_state(c, track_words(continuation_words, cnt), cnt, CONT_ENTRY);
	spvm_byte* pblob = NULL; size_t pblob_n = 0;
	CHECK(save_blob(prep, &pblob, &pblob_n) == 1);
	spvm_state_t from_prep = make_state(c, track_words(continuation_words, cnt), cnt, CONT_ENTRY);
	CHECK(spvm_checkpoint_restore(from_prep, pblob, pblob_n) == 1);
	CHECK(from_prep->function_stack_current == 0);
	CHECK(from_prep->current_function == from_prep->results + 20);
	spvm_state_call_function(from_prep);
	CHECK(!from_prep->code_current);
	CHECK(from_prep->results[14].members[0].value.s == 11);

	/* resumed state, opcode stepping, compared against the reference */
	spvm_state_t res = make_state(c, track_words(continuation_words, cnt), cnt, CONT_ENTRY);
	CHECK(spvm_checkpoint_restore(res, blob, blob_n) == 1);
	CHECK(res->instruction_count == paused->instruction_count);
	CHECK(res->function_stack_current == 2);
	CHECK(res->current_line == 27 && res->current_column == 3);
	CHECK(res->current_file != NULL);
	CHECK(strcmp(res->current_file, paused->current_file) == 0);

	const spvm_word watch[] = { 22, 33, 34, 35, 45, 25, 26, 27, 14 };
	for (int round = 0; round < 3; round++) {
		long res_pc;
		if (round == 0)
			res_pc = res->code_current ? (long)(res->code_current - res->owner->code) : -1;
		else if (round == 1) {
			pause_at(res, SpvOpPhi, 45);
			res_pc = (long)(res->code_current - res->owner->code);
		}
		else {
			finish_steps(res);
			res_pc = -1;
		}
		/* align by PC, not instruction count: OpLine/OpNoLine advance
		 * the PC without incrementing the counted instruction number */
		if (res_pc < 0)
			finish_steps(ref);
		else
			for (int guard = 0; guard < 200 && ref->code_current; ++guard) {
				long p = (long)(ref->code_current - ref->owner->code);
				if (p == res_pc)
					break;
				spvm_state_step_opcode(ref);
			}
		long ref_pc = ref->code_current ? (long)(ref->code_current - ref->owner->code) : -1;
		CHECK(ref_pc == res_pc);
		/* once aligned at the same instruction, the counted number and
		 * the whole call/debug scene must match exactly */
		CHECK(ref->instruction_count == res->instruction_count);
		CHECK(ref->function_stack_current == res->function_stack_current);
		CHECK(ref->current_line == res->current_line);
		CHECK(ref->current_column == res->current_column);
		CHECK((ref->current_file == NULL) == (res->current_file == NULL));
		if (ref->current_file && res->current_file)
			CHECK(strcmp(ref->current_file, res->current_file) == 0);
		for (size_t k = 0; k < sizeof(watch)/sizeof(watch[0]); k++) {
			spvm_word id = watch[k];
			if (ref->results[id].members && res->results[id].members)
				CHECK(ref->results[id].members[0].value.u64 ==
					res->results[id].members[0].value.u64);
		}
		if (round == 1) {
			/* stopped at the merge block, predecessor still recorded */
			CHECK(res->function_stack_cfg[1] == 39);
			CHECK(res->function_stack_cfg_parent[1] == 37);
		}
	}
	CHECK(!res->code_current);
	CHECK(res->results[14].members[0].value.s == 11);
	CHECK(ref->results[14].members[0].value.s == 11);

	/* same checkpoint resumed via call_function must match */
	spvm_state_t res2 = make_state(c, track_words(continuation_words, cnt), cnt, CONT_ENTRY);
	CHECK(spvm_checkpoint_restore(res2, blob, blob_n) == 1);
	spvm_state_call_function(res2);
	CHECK(!res2->code_current);
	CHECK(res2->results[14].members[0].value.s == 11);

	/* branch modification through a nested alias, both finish styles */
	spvm_state_t br = make_state(c, track_words(continuation_words, cnt), cnt, CONT_ENTRY);
	CHECK(spvm_checkpoint_restore(br, blob, blob_n) == 1);
	br->results[41].members[0].value.s = 10;
	finish_steps(br);
	CHECK(br->results[14].members[0].value.s == 20);

	spvm_state_t br2 = make_state(c, track_words(continuation_words, cnt), cnt, CONT_ENTRY);
	CHECK(spvm_checkpoint_restore(br2, blob, blob_n) == 1);
	br2->results[41].members[0].value.s = 10;
	spvm_state_call_function(br2);
	CHECK(br2->results[14].members[0].value.s == 20);

	/* terminal checkpoint: finished scene survives reload */
	spvm_byte* tblob = NULL; size_t tblob_n = 0;
	CHECK(save_blob(br, &tblob, &tblob_n) == 1);
	spvm_state_t done = make_state(c, track_words(continuation_words, cnt), cnt, CONT_ENTRY);
	CHECK(spvm_checkpoint_restore(done, tblob, tblob_n) == 1);
	CHECK(!done->code_current);
	CHECK(done->results[14].members[0].value.s == 20);
	/* a finished scene stays finished: there is no PC to step from */
	CHECK(done->function_stack_current == (spvm_word)-1);
	free(tblob);

	/* interleaved checkpoints: repeated restores stay independent */
	spvm_state_t inter = make_state(c, track_words(continuation_words, cnt), cnt, CONT_ENTRY);
	CHECK(spvm_checkpoint_restore(inter, blob, blob_n) == 1);
	CHECK(spvm_checkpoint_restore(inter, pblob, pblob_n) == 1);
	CHECK(inter->function_stack_current == 0);
	CHECK(spvm_checkpoint_restore(inter, blob, blob_n) == 1);
	CHECK(inter->results[22].members[0].value.s == 1);
	finish_steps(inter);
	CHECK(inter->results[14].members[0].value.s == 11);

	delete_state(ref); delete_state(paused); delete_state(prep);
	delete_state(from_prep); delete_state(res); delete_state(res2);
	delete_state(br); delete_state(br2); delete_state(done); delete_state(inter);
	free(blob); free(pblob);
	free_tracked_words();
	spvm_context_deinitialize(c);
	return n;
}

/* ================================================================== */
/* reject: unsupported modules and corrupted blobs never commit       */
/* ================================================================== */
/* Runs one rejection scenario and returns 1 only when the corrupted blob
 * is refused AND the target still completes the normal program untouched.
 * No UNIT_CHECK is taken here, so callers count their own assertions. */
static int reject_one(spvm_context_t c, const spvm_byte* blob, size_t blob_n,
	int mode, size_t arg)
{
	size_t cnt = CONT_WORDS;

	if (mode == 9) {
		/* target module with a debug-string payload word changed: the
		 * numeric program still runs identically, but the full-module
		 * checksum no longer matches */
		spvm_word* mw = track_words(continuation_words, cnt);
		mw[arg] ^= 0x1u;
		spvm_state_t other = make_state(c, mw, cnt, CONT_ENTRY);
		int refused = spvm_checkpoint_restore(other, blob, blob_n) == 0;
		finish_steps(other);
		int intact = other->results[14].members[0].value.s == 11;
		delete_state(other);
		return refused && intact;
	}
	if (mode == 10) {
		/* target built from a different eligible module */
		size_t tn = sizeof(cp_types_words) / sizeof(cp_types_words[0]);
		spvm_state_t other = make_state(c, track_words(cp_types_words, tn), tn, 70);
		int refused = spvm_checkpoint_restore(other, blob, blob_n) == 0;
		delete_state(other);
		return refused;
	}

	spvm_state_t t = make_state(c, track_words(continuation_words, cnt), cnt, CONT_ENTRY);
	spvm_byte* bad;
	size_t bad_n;

	if (mode == 6) { /* trailing garbage */
		bad_n = blob_n + 1;
		bad = (spvm_byte*)malloc(bad_n);
		memcpy(bad, blob, blob_n);
		bad[blob_n] = 0x5A;
	}
	else if (mode == 5) { /* truncation */
		bad_n = arg;
		bad = (spvm_byte*)malloc(blob_n ? blob_n : 1);
		memcpy(bad, blob, bad_n);
	}
	else {
		bad_n = blob_n;
		bad = (spvm_byte*)malloc(blob_n);
		memcpy(bad, blob, blob_n);
	}

	switch (mode) {
	case 0: bad[0] ^= 0xFFu; break;                          /* bad magic */
	case 1: put_le32(bad + 4, 99u); break;                   /* version */
	case 2: { /* module checksum */
		size_t m = find_tag(bad, bad_n, TAG_MOD);
		bad[m + 16] ^= 0x77u;
		break;
	}
	case 3: { /* PC: out of range or pushed off its instruction boundary */
		size_t x = find_tag(bad, bad_n, TAG_EXEC);
		if (arg == 0)
			put_le32(bad + x + 12, 0x7FFFFFFFu);
		else
			bad[x + 12] = (spvm_byte)(bad[x + 12] + 1);
		break;
	}
	case 4: { /* frame[1] function id invalid; frames begin after
	             tag(4)+size(4)+flags(4)+8 fields(32)+2 flags+depth(4)=50 */
		size_t x = find_tag(bad, bad_n, TAG_EXEC);
		put_le32(bad + x + 50 + 20 + 4, 0x7FFFFFFFu);
		break;
	}
	case 7: { /* gigantic root count */
		size_t v = find_tag(bad, bad_n, TAG_VAL);
		put_le32(bad + v + 8, 0x7FFFFFFFu);
		break;
	}
	case 8: { /* wrong value-tree kind */
		size_t k = first_tree_kind(bad, bad_n);
		bad[k] = 99;
		break;
	}
	case 11: { /* invalid alias result id */
		size_t a = find_alias_record(bad, bad_n, 41);
		put_le32(bad + a, 0x7FFFFFFFu);
		break;
	}
	case 12: { /* alias member index out of bounds */
		size_t a = find_alias_record(bad, bad_n, 41);
		put_le32(bad + a + 12, 99u);
		break;
	}
	default: break;
	}

	int refused = spvm_checkpoint_restore(t, bad, bad_n) == 0;
	/* target untouched: it still completes the normal program */
	finish_steps(t);
	int finished = !t->code_current;
	int intact = t->results[14].members[0].value.s == 11;

	free(bad);
	delete_state(t);
	return refused && finished && intact;
}

static int group_reject(void)
{
	int n = 0;
	spvm_context_t c = spvm_context_initialize();
	size_t cnt = CONT_WORDS;

	spvm_state_t s = make_state(c, track_words(continuation_words, cnt), cnt, CONT_ENTRY);
	pause_at(s, SpvOpLoad, 43);
	spvm_byte* blob = NULL; size_t blob_n = 0;
	CHECK(save_blob(s, &blob, &blob_n) == 1);

	/* argument validation */
	CHECK(spvm_checkpoint_restore(NULL, blob, blob_n) == 0);
	CHECK(spvm_checkpoint_restore(s, NULL, blob_n) == 0);

	/* a rejected restore must not move any target field: snapshot the
	 * scene and tree addresses, feed a corrupt blob, compare afterwards */
	{
		spvm_state_t v = make_state(c, track_words(continuation_words, cnt), cnt, CONT_ENTRY);
		pause_at(v, SpvOpLoad, 43);
		spvm_member_t m22 = v->results[22].members;
		spvm_member_t m41 = v->results[41].members;
		spvm_source pc = v->code_current;
		spvm_word depth = v->function_stack_current;
		spvm_word inst = v->instruction_count;
		int line = v->current_line;
		spvm_byte* bad = (spvm_byte*)malloc(blob_n);
		memcpy(bad, blob, blob_n);
		put_le32(bad + 4, 99u); /* unsupported version */
		CHECK(spvm_checkpoint_restore(v, bad, blob_n) == 0);
		CHECK(v->results[22].members == m22);
		CHECK(v->results[41].members == m41);
		CHECK(v->code_current == pc);
		CHECK(v->function_stack_current == depth);
		CHECK(v->instruction_count == inst);
		CHECK(v->current_line == line);
		/* the rejected state still executes the normal program */
		finish_steps(v);
		CHECK(v->results[14].members[0].value.s == 11);
		free(bad);
		delete_state(v);
	}
	{
		spvm_byte* d = (spvm_byte*)0x1; size_t z = 1;
		CHECK(spvm_checkpoint_save(s, NULL, &z) == 0 && z == 0);
		CHECK(spvm_checkpoint_save(s, &d, NULL) == 0 && d == NULL);
		CHECK(spvm_checkpoint_save(NULL, &d, &z) == 0);
	}

	/* corrupted blobs */
	CHECK(reject_one(c, blob, blob_n, 0, 0) >= 1);  /* bad magic */
	CHECK(reject_one(c, blob, blob_n, 1, 0) >= 1);  /* unknown version */
	CHECK(reject_one(c, blob, blob_n, 2, 0) >= 1);  /* module hash */
	CHECK(reject_one(c, blob, blob_n, 3, 0) >= 1);  /* PC out of range */
	CHECK(reject_one(c, blob, blob_n, 3, 1) >= 1);  /* PC off boundary */
	CHECK(reject_one(c, blob, blob_n, 4, 0) >= 1);  /* invalid frame */
	CHECK(reject_one(c, blob, blob_n, 5, 0) >= 1);  /* truncations */
	CHECK(reject_one(c, blob, blob_n, 5, 1) >= 1);
	CHECK(reject_one(c, blob, blob_n, 5, 7) >= 1);
	CHECK(reject_one(c, blob, blob_n, 5, blob_n / 2) >= 1);
	CHECK(reject_one(c, blob, blob_n, 5, blob_n - 1) >= 1);
	CHECK(reject_one(c, blob, blob_n, 6, 0) >= 1);  /* trailing garbage */
	CHECK(reject_one(c, blob, blob_n, 7, 0) >= 1);  /* huge count */
	CHECK(reject_one(c, blob, blob_n, 8, 0) >= 1);  /* bad tree shape */
	CHECK(reject_one(c, blob, blob_n, 9, 18) >= 1); /* changed module word */
	CHECK(reject_one(c, blob, blob_n, 9, 19) >= 1);
	CHECK(reject_one(c, blob, blob_n, 10, 0) >= 1); /* different module */
	CHECK(reject_one(c, blob, blob_n, 11, 0) >= 1); /* bad alias id */
	CHECK(reject_one(c, blob, blob_n, 12, 0) >= 1); /* bad member index */

	/* ineligible modules: save fails with NULL/0 outputs */
	{
		size_t rn = sizeof(cp_runtime_words) / sizeof(cp_runtime_words[0]);
		spvm_state_t rs = make_state(c, track_words(cp_runtime_words, rn), rn, 10);
		spvm_byte* d = (spvm_byte*)0x1; size_t z = 7;
		CHECK(spvm_checkpoint_save(rs, &d, &z) == 0);
		CHECK(d == NULL && z == 0);
		delete_state(rs);
	}
	{
		size_t in = sizeof(cp_image_words) / sizeof(cp_image_words[0]);
		spvm_state_t is = make_state(c, track_words(cp_image_words, in), in, 10);
		spvm_byte* d = NULL; size_t z = 0;
		CHECK(spvm_checkpoint_save(is, &d, &z) == 0);
		CHECK(d == NULL && z == 0);
		delete_state(is);
	}
	{
		size_t an = sizeof(cp_atomic_words) / sizeof(cp_atomic_words[0]);
		spvm_state_t as = make_state(c, track_words(cp_atomic_words, an), an, 10);
		spvm_byte* d = NULL; size_t z = 0;
		CHECK(spvm_checkpoint_save(as, &d, &z) == 0);
		CHECK(d == NULL && z == 0);
		/* restore also refuses unsupported target modules */
		CHECK(spvm_checkpoint_restore(as, blob, blob_n) == 0);
		delete_state(as);
	}

	delete_state(s);
	free(blob);
	free_tracked_words();
	spvm_context_deinitialize(c);
	return n;
}

int unit_run_group(const char* name)
{
	if (strcmp(name, "relocate") == 0)
		return group_relocate();
	if (strcmp(name, "continuation") == 0)
		return group_continuation();
	if (strcmp(name, "reject") == 0)
		return group_reject();
	return 0;
}
