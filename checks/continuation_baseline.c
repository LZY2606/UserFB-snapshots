#include <spvm/context.h>
#include <spvm/state.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "continuation.h"

static void pause_at(spvm_state_t s, int opcode, int id) {
 for (int i=0; i<200 && s->code_current; ++i) {
  const spvm_word *pc=s->code_current;
  if ((pc[0]&SpvOpCodeMask)==opcode && pc[2]==id) return;
  spvm_state_step_opcode(s);
 }
 assert(!"fixture pause was not reached");
}
static void finish(spvm_state_t s) {
 int n=0;
 while (s->code_current && ++n<200) spvm_state_step_opcode(s);
 assert(!s->code_current);
}
int main(void) {
 spvm_context_t c=spvm_context_initialize();
 size_t count=sizeof(continuation_words)/sizeof(*continuation_words);
 spvm_program_t p=spvm_program_create(c,continuation_words,count);
 spvm_state_t a=spvm_state_create(p), b=spvm_state_create(p);
 spvm_state_prepare(a,20); spvm_state_prepare(b,20);
 assert(b->results[22].members && b->results[23].members==NULL);
 assert(b->results[31].members==NULL && b->results[41].members==NULL);
 puts("FRESH_TARGET_OWNS_ROOT_BUT_HAS_NO_ALIASES");
 pause_at(a,SpvOpLoad,43); pause_at(b,SpvOpLoad,43);
 assert(a->function_stack_current==2 && a->current_function==a->results+40);
 assert(a->current_line==27 && a->current_column==3 && a->current_file);
 assert(a->results[23].members==a->results[22].members);
 assert(a->results[31].members==a->results[23].members);
 assert(a->results[41].members==a->results[31].members);
 assert(a->results[24].members==a->results[22].members+1);
 assert(a->results[41].members!=b->results[41].members);
 printf("NESTED_PAUSE depth=%d line=%d first=%d sibling=%d\n",
  a->function_stack_current,a->current_line,
  a->results[41].members[0].value.s,a->results[24].members[0].value.s);
 puts("ACCESSCHAIN_PARAMETER_ALIAS_OK");
 a->results[41].members[0].value.s=10;
 assert(b->results[41].members[0].value.s==1);
 finish(a);
 assert(a->results[14].members[0].value.s==20);
 pause_at(b,SpvOpPhi,45);
 assert(b->function_stack_current==1);
 assert(b->function_stack_cfg[1]==39 && b->function_stack_cfg_parent[1]==37);
 printf("PHI_PAUSE current=%d predecessor=%d\n",b->function_stack_cfg[1],b->function_stack_cfg_parent[1]);
 finish(b);
 assert(b->results[14].members[0].value.s==11);
 printf("CONTINUATION_RESULTS normal=%d branch=%d\n",b->results[14].members[0].value.s,a->results[14].members[0].value.s);
 spvm_state_delete(a);spvm_state_delete(b);spvm_program_delete(p);spvm_context_deinitialize(c);
 puts("CONTINUATION_BASELINE_OK");
 return 0;
}
