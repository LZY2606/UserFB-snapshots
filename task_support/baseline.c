#include <mpg123.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static mpg123_handle *open_audio(const char *path){
 int e;mpg123_handle *h=mpg123_new("generic",&e);if(!h)exit(2);
 if(mpg123_param(h,MPG123_ADD_FLAGS,MPG123_QUIET|MPG123_FORCE_STEREO,0)
 ||mpg123_param(h,MPG123_REMOVE_FLAGS,MPG123_GAPLESS|MPG123_NO_FRANKENSTEIN,0)
 ||mpg123_param(h,MPG123_FORCE_RATE,48000,0)||mpg123_format_none(h)
 ||mpg123_format(h,48000,MPG123_STEREO,MPG123_ENC_SIGNED_16)||mpg123_open(h,path))exit(3);
 return h;
}
static size_t decode(const char *path,unsigned char **result,size_t block){
 mpg123_handle *h=open_audio(path);size_t used=0,cap=1048576;unsigned char *p=malloc(cap);if(!p)exit(4);
 int rc=MPG123_OK;
 for(int calls=0;calls<100000;calls++){
  if(used+block>cap){cap*=2;p=realloc(p,cap);if(!p)exit(5);}
  size_t n=0;rc=mpg123_read(h,p+used,block,&n);used+=n;
  if(rc==MPG123_NEW_FORMAT){long r;int ch,en;if(mpg123_getformat(h,&r,&ch,&en)||r!=48000||ch!=2||en!=MPG123_ENC_SIGNED_16)exit(6);}
  else if(rc==MPG123_DONE)break;
  else if(rc!=MPG123_OK)exit(7);
 }
 mpg123_delete(h);if(rc!=MPG123_DONE||!used||used%4)exit(8);*result=p;return used;
}
int main(void){
 if(mpg123_init())return 1;
 unsigned char *a,*b;size_t n=decode("task_support/mixed.mp2",&a,3108),m=decode("task_support/mixed.mp2",&b,148);
 if(n!=m||memcmp(a,b,n))return 9;
 size_t nonzero=0;for(size_t i=0;i<n;i++)nonzero+=a[i]!=0;if(nonzero<1000)return 10;
 printf("BASELINE_DECODE output_frames=%zu chunk_equal=1 nonzero_bytes=%zu\n",n/4,nonzero);
 mpg123_handle *h=open_audio("task_support/mixed.mp2");int rc=mpg123_scan(h);if(rc!=MPG123_OK)return 11;
 int64_t len=mpg123_length64(h);printf("MIXED_SCAN length=%lld decoded=%zu difference=%lld\n",(long long)len,n/4,(long long)(len-(int64_t)n/4));
 size_t got=0;unsigned char buf[512];int64_t target=(int64_t)n/4*3/4,at=mpg123_seek64(h,target,SEEK_SET);
 rc=mpg123_read(h,buf,sizeof buf,&got);if(rc==MPG123_NEW_FORMAT)rc=mpg123_read(h,buf,sizeof buf,&got);
 int equal=at==target&&got==sizeof buf&&(size_t)target*4+got<=n&&!memcmp(buf,a+target*4,got);
 printf("MIXED_SEEK target=%lld returned=%lld read_status=%d pcm_equal=%d\n",(long long)target,(long long)at,rc,equal);
 mpg123_delete(h);free(a);free(b);mpg123_exit();puts("BASELINE_OK");return 0;
}
