#ifndef TASK_IMA_FIXTURE_H
#define TASK_IMA_FIXTURE_H
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { unsigned char bytes[512]; size_t n; sf_count_t pos; size_t bytes_read; } task_memory;
static void task_u16(task_memory* b,unsigned v){b->bytes[b->n++]=(unsigned char)v;b->bytes[b->n++]=(unsigned char)(v>>8);}
static void task_u32(task_memory* b,uint32_t v){task_u16(b,v&65535);task_u16(b,v>>16);}
static void task_tag(task_memory* b,const char* s){memcpy(b->bytes+b->n,s,4);b->n+=4;}
static void task_patch32(task_memory* b,size_t p,uint32_t v){unsigned k;for(k=0;k<4;++k)b->bytes[p+k]=(unsigned char)(v>>(8*k));}
static void task_block(task_memory* b,int variant){unsigned k;task_u16(b,variant?2000:1000);task_u16(b,0);task_u16(b,(unsigned)(variant?-2000:-1000));task_u16(b,0);for(k=0;k<4;++k)b->bytes[b->n++]=0x11;for(k=0;k<4;++k)b->bytes[b->n++]=0x99;}
static void task_ima(task_memory* b,int segmented){size_t list=0;memset(b,0,sizeof(*b));task_tag(b,"RIFF");task_u32(b,0);task_tag(b,"WAVE");task_tag(b,"fmt ");task_u32(b,20);task_u16(b,17);task_u16(b,2);task_u32(b,8000);task_u32(b,14222);task_u16(b,16);task_u16(b,4);task_u16(b,2);task_u16(b,9);
 if(segmented){task_tag(b,"LIST");list=b->n;task_u32(b,0);task_tag(b,"wavl");}
 task_tag(b,"data");task_u32(b,16);task_block(b,0);
 if(segmented){task_tag(b,"slnt");task_u32(b,4);task_u32(b,5);task_tag(b,"data");task_u32(b,16);task_block(b,1);task_patch32(b,list,(uint32_t)(b->n-list-4));}
 task_patch32(b,4,(uint32_t)(b->n-8));
}
static sf_count_t task_len(void* u){return (sf_count_t)((task_memory*)u)->n;}
static sf_count_t task_seek(sf_count_t n,int whence,void* u){task_memory* b=(task_memory*)u;sf_count_t base=whence==SEEK_SET?0:whence==SEEK_CUR?b->pos:(sf_count_t)b->n;if(n<-base||n>(sf_count_t)b->n-base)return -1;b->pos=base+n;return b->pos;}
static sf_count_t task_read(void* p,sf_count_t n,void* u){task_memory* b=(task_memory*)u;sf_count_t rem=(sf_count_t)b->n-b->pos;if(n>rem)n=rem;if(n<0)return 0;memcpy(p,b->bytes+(size_t)b->pos,(size_t)n);b->pos+=n;b->bytes_read+=(size_t)n;return n;}
static sf_count_t task_write(const void* p,sf_count_t n,void* u){(void)p;(void)n;(void)u;return 0;}
static sf_count_t task_tell(void* u){return ((task_memory*)u)->pos;}
static SNDFILE* task_open(task_memory* b,SF_INFO* info){SF_VIRTUAL_IO io={task_len,task_seek,task_read,task_write,task_tell};memset(info,0,sizeof(*info));b->pos=0;return sf_open_virtual(&io,SFM_READ,info,b);}
#endif
