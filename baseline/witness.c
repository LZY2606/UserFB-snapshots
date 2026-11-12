#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hiredis.h"
int main(void) {
 redisReader *r=redisReaderCreate();void *p=NULL;const char *x="%1\r\n+k\r\n*2\r\n:7\r\n$3\r\nfoo\r\n+NEXT\r\n";
 for(size_t i=0;i<strlen(x);i++){
  assert(redisReaderFeed(r,x+i,1)==REDIS_OK);assert(redisReaderGetReply(r,&p)==REDIS_OK);
  if(p){redisReply *v=p;if(v->type==REDIS_REPLY_MAP){assert(v->elements==2&&v->element[1]->elements==2&&v->element[1]->element[0]->integer==7);puts("BASELINE_FIXED_MAP=7,foo");}else{assert(v->type==REDIS_REPLY_STATUS&&!strcmp(v->str,"NEXT"));puts("BASELINE_PIPELINE_NEXT=OK");}freeReplyObject(p);p=NULL;}
 }
 redisReaderFree(r);r=redisReaderCreate();x="$?\r\n;3\r\nfoo\r\n;0\r\n";
 assert(redisReaderFeed(r,x,strlen(x))==REDIS_OK);int status=redisReaderGetReply(r,&p);
 printf("LEGACY_STREAM_STATUS=%d\n",status);if(p)freeReplyObject(p);redisReaderFree(r);return 0;
}
