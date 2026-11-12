#include "srtp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define OK(x) do{int e=(x);if(e){fprintf(stderr,"line %d status %d\n",__LINE__,e);exit(1);}}while(0)
int main(void){
 srtp_t tx=NULL,rx=NULL;srtp_policy_t p=NULL;uint8_t key[16],salt[14];
 uint32_t plain[4]={0},wire[32]={0},out[32]={0};size_t n=sizeof(wire),m=sizeof(out);
 unsigned char *b=(unsigned char*)plain;b[0]=0x80;b[1]=96;b[3]=77;b[8]=0x11;b[9]=0x22;b[10]=0x33;b[11]=0x44;b[12]=9;
 for(int i=0;i<16;i++)key[i]=(uint8_t)(i+1);for(int i=0;i<14;i++)salt[i]=(uint8_t)(i+21);
 OK(srtp_init());OK(srtp_policy_create(&p));
 OK(srtp_policy_set_ssrc(p,(srtp_ssrc_t){ssrc_specific,0x11223344}));
 OK(srtp_policy_set_profile(p,srtp_profile_aes128_cm_sha1_80));
 OK(srtp_policy_add_key(p,key,sizeof(key),salt,sizeof(salt),NULL,0));
 OK(srtp_policy_set_window_size(p,128));OK(srtp_create(&tx,p));OK(srtp_create(&rx,p));
 OK(srtp_protect(tx,(uint8_t*)plain,sizeof(plain),(uint8_t*)wire,&n,0));
 OK(srtp_unprotect(rx,(uint8_t*)wire,n,(uint8_t*)out,&m));if(m!=sizeof(plain)||memcmp(out,plain,m))return 2;
 m=sizeof(out);int before=srtp_unprotect(rx,(uint8_t*)wire,n,(uint8_t*)out,&m);
 if(before!=srtp_err_status_replay_fail)return 3;
 OK(srtp_stream_update(rx,p));m=sizeof(out);
 int after=srtp_unprotect(rx,(uint8_t*)wire,n,(uint8_t*)out,&m);
 printf("UPDATE_DUPLICATE before=%d after=%d\n",before,after);
 /*
  * The policy update must preserve the RTP replay state: an already
  * authenticated packet is still a replay afterwards, exactly as it was
  * before the update.  This used to assert the buggy acceptance (status
  * srtp_err_status_ok); it now pins the corrected behavior.
  */
 if(before!=srtp_err_status_replay_fail)return 3;
 if(after!=srtp_err_status_replay_fail)return 4;
 OK(srtp_dealloc(tx));OK(srtp_dealloc(rx));srtp_policy_destroy(p);OK(srtp_shutdown());
 puts("SRTP_REPLAY_UPDATE_FIXED");return 0;
}
