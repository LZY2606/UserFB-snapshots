#include "libkeccak.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
 struct libkeccak_spec spec; struct libkeccak_state s;
 unsigned char out[32]; char hex[65];
 libkeccak_spec_sha3(&spec,256);
 assert(libkeccak_state_initialise(&s,&spec)==0);
 assert(libkeccak_digest(&s,NULL,0,0,LIBKECCAK_SHA3_SUFFIX,out)==0);
 libkeccak_behex_lower(hex,out,32);
 assert(strcmp(hex,"a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a")==0);
 libkeccak_state_fast_destroy(&s);
 assert(libkeccak_state_initialise(&s,&spec)==0);
 assert(libkeccak_update(&s,"a",1)==0);
 assert(libkeccak_digest(&s,"bc",2,0,LIBKECCAK_SHA3_SUFFIX,out)==0);
 libkeccak_behex_lower(hex,out,32);
 assert(strcmp(hex,"3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532")==0);
 libkeccak_state_fast_destroy(&s);
 puts("KECCAK_BASELINE_OK empty abc split-update"); return 0;
}
