#include <tinycrypt/aes.h>
#include <tinycrypt/ctr_mode.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static void hex(unsigned char *out,const char *in,size_t n){size_t i;unsigned v;for(i=0;i<n;i++){if(sscanf(in+2*i,"%2x",&v)!=1)abort();out[i]=(unsigned char)v;}}
int main(void){
 struct tc_aes_key_sched_struct sched;
 unsigned char key[16],plain[64],out[64],want[64],ctr[16];
 hex(key,"000102030405060708090a0b0c0d0e0f",16);
 hex(plain,"00112233445566778899aabbccddeeff",16);
 hex(want,"69c4e0d86a7b0430d8cdb78070b4c55a",16);
 if(!tc_aes128_set_encrypt_key(&sched,key)||!tc_aes_encrypt(out,plain,&sched)||memcmp(out,want,16))return 1;
 hex(key,"2b7e151628aed2a6abf7158809cf4f3c",16);
 hex(ctr,"f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",16);
 hex(plain,"6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710",64);
 hex(want,"874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee",64);
 if(!tc_aes128_set_encrypt_key(&sched,key)||!tc_ctr_mode(out,64,plain,64,ctr,&sched)||memcmp(out,want,64))return 2;
 puts("TINYCRYPT_FIPS_AES_AND_NIST_CTR_OK");return 0;
}
