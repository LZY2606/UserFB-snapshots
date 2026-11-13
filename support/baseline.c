#include <stdio.h>
#include <string.h>
#include "monocypher.h"
int main(void) {
 unsigned char seed[32]={0},sk[64],pk[32],sig[64];
 const unsigned char msg[]={0,1,2,255,0,17};
 for(unsigned i=0;i<32;i++)seed[i]=(unsigned char)(i+1);
 crypto_eddsa_key_pair(sk,pk,seed);
 crypto_eddsa_sign(sig,sk,msg,sizeof msg);
 if(crypto_eddsa_check(sig,pk,msg,sizeof msg))return 1;
 sig[32]^=1;
 if(crypto_eddsa_check(sig,pk,msg,sizeof msg)==0)return 2;
 crypto_wipe(sk,sizeof sk);
 puts("MONOCYPHER_BASELINE_OK valid accepted altered rejected");return 0;
}
