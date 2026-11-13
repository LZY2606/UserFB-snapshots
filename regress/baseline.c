#include "kafel.h"
#include "repeated.h"
#include "interpreter.h"
#include <linux/audit.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) {
  const char *policy="ALLOW { SYSCALL[123](a,b,c) { ((a & b) | (b & c)) == 0x100000001 } } DEFAULT ERRNO(7)";
  struct sock_fprog p={0}; kafel_ctxt_t c=kafel_ctxt_create();
  interpreter_ctxt_t i={0}; struct seccomp_data d={0};
  kafel_set_target_archs(c,KAFEL_TARGET_ARCH_X86_64);
  kafel_set_input_string(c,policy);
  if(kafel_compile(c,&p)) {fprintf(stderr,"%s",kafel_error_msg(c)); return 1;}
  d.arch=AUDIT_ARCH_X86_64; d.nr=123; d.args[0]=d.args[1]=0x100000001ULL;
  assert(interpreter_run(&i,&p,&d) && i.result==SECCOMP_RET_ALLOW);
  d.args[0]=1;
  assert(interpreter_run(&i,&p,&d) && i.result==(SECCOMP_RET_ERRNO|7));
  d.arch=0;
  assert(interpreter_run(&i,&p,&d) && i.result==SECCOMP_RET_KILL);
  printf("KAFEL_BASELINE_OK compiled=%u highword priority arch\n",p.len);
  free(p.filter);
  char *text=repeated_policy(6);kafel_set_input_string(c,text);
  if(kafel_compile(c,&p)) {fprintf(stderr,"%s",kafel_error_msg(c));return 1;}
  printf("KAFEL_REPEATED_START instructions=%u\n",p.len);
  free(p.filter);free(text);kafel_ctxt_destroy(&c);return 0;
}
