#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <spvm/context.h>
#include <spvm/state.h>
#include <spvm/checkpoint.h>
#include <spvm/spirv.h>
#include "continuation.h"

/* Demonstrates the full checkpoint lifecycle:
 *   1. pause a numeric shader inside a nested call,
 *   2. write the checkpoint bytes to a file and release the whole source
 *      session (state, program and original instruction words),
 *   3. reload the same module from an independent buffer, read the bytes
 *      back and restore the scene without replaying any instruction,
 *   4. continue with both step_opcode and call_function.
 * No shader instruction runs while saving or restoring. */

#define CHECKPOINT_FILE "checkpoint_example.bin"
#define ENTRY 20u

static void pause_at(spvm_state_t s, int opcode, int id)
{
	for (int i = 0; i < 200 && s->code_current; ++i) {
		const spvm_word* pc = s->code_current;
		if ((pc[0] & SpvOpCodeMask) == opcode && pc[2] == id)
			return;
		spvm_state_step_opcode(s);
	}
	assert(!"pause point not reached");
}

static spvm_word* load_fixture(size_t* count)
{
	*count = sizeof(continuation_words) / sizeof(continuation_words[0]);
	spvm_word* words = (spvm_word*)malloc(*count * sizeof(spvm_word));
	memcpy(words, continuation_words, *count * sizeof(spvm_word));
	return words;
}

static void finish(spvm_state_t s)
{
	for (int i = 0; i < 200 && s->code_current; ++i)
		spvm_state_step_opcode(s);
}

int main(void)
{
	spvm_context_t ctx = spvm_context_initialize();

	/* ---- session 1: run to the pause point and write a checkpoint ---- */
	size_t cnt1;
	spvm_word* words1 = load_fixture(&cnt1);
	spvm_program_t prog1 = spvm_program_create(ctx, words1, cnt1);
	spvm_state_t run = spvm_state_create(prog1);
	spvm_state_prepare(run, ENTRY);
	pause_at(run, SpvOpLoad, 43); /* inside inner(), before its Load */

	spvm_byte* blob = NULL;
	size_t blob_size = 0;
	int saved = spvm_checkpoint_save(run, &blob, &blob_size);
	assert(saved && blob && blob_size);

	FILE* f = fopen(CHECKPOINT_FILE, "wb");
	assert(f);
	assert(fwrite(blob, 1, blob_size, f) == blob_size);
	fclose(f);
	free(blob);

	/* the entire source session goes away; the file is all that remains */
	spvm_state_delete(run);
	spvm_program_delete(prog1);
	free(words1);

	/* ---- session 2: reload the module and restore from the file ------ */
	size_t cnt2;
	spvm_word* words2 = load_fixture(&cnt2);
	spvm_program_t prog2 = spvm_program_create(ctx, words2, cnt2);
	spvm_state_t resumed = spvm_state_create(prog2);
	spvm_state_prepare(resumed, ENTRY);

	f = fopen(CHECKPOINT_FILE, "rb");
	assert(f);
	fseek(f, 0, SEEK_END);
	long file_size = ftell(f);
	rewind(f);
	assert(file_size > 0);
	spvm_byte* on_disk = (spvm_byte*)malloc((size_t)file_size);
	assert(fread(on_disk, 1, (size_t)file_size, f) == (size_t)file_size);
	fclose(f);

	assert(spvm_checkpoint_restore(resumed, on_disk, (size_t)file_size) == 1);

	/* the call stack, current function and debug position are back */
	assert(resumed->function_stack_current == 2);
	assert(resumed->current_function == resumed->results + 40);
	assert(resumed->current_line == 27 && resumed->current_column == 3);
	/* nested pointer aliases were rebuilt on this state's own tree */
	assert(resumed->results[41].members == resumed->results[22].members);
	assert(resumed->results[24].members == resumed->results[22].members + 1);
	assert(resumed->results[41].members[0].value.s == 1);

	/* a write through the restored nested alias changes the shared member */
	resumed->results[41].members[0].value.s = 10;
	finish(resumed);
	assert(!resumed->code_current);
	assert(resumed->results[14].members[0].value.s == 20);

	/* ---- another independent restore from the same on-disk bytes ----- */
	spvm_word* words3 = load_fixture(&cnt2);
	spvm_program_t prog3 = spvm_program_create(ctx, words3, cnt2);
	spvm_state_t again = spvm_state_create(prog3);
	spvm_state_prepare(again, ENTRY);
	assert(spvm_checkpoint_restore(again, on_disk, (size_t)file_size) == 1);
	spvm_state_call_function(again);
	assert(!again->code_current);
	/* no mutation this time, so the program produces the normal result */
	assert(again->results[14].members[0].value.s == 11);

	printf("checkpoint %zu bytes; resumed outputs: mutated=%d normal=%d\n",
		(size_t)file_size,
		resumed->results[14].members[0].value.s,
		again->results[14].members[0].value.s);

	free(on_disk);
	spvm_state_delete(again);
	spvm_program_delete(prog3);
	free(words3);
	spvm_state_delete(resumed);
	spvm_program_delete(prog2);
	free(words2);
	spvm_context_deinitialize(ctx);
	remove(CHECKPOINT_FILE);

	puts("CHECKPOINT_EXAMPLE_OK");
	return 0;
}
