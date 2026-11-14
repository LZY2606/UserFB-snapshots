#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "backtrace.h"
#include "internal.h"
#include "basic_fixture.h"
static int errors,frames,symbols;
static void err(void *d,const char *m,int e){(void)d;fprintf(stderr,"error %s (%d)\n",m,e);errors++;}
static int frame(void *d,uintptr_t pc,const char *f,int line,const char *fn){(void)d;
 if(pc!=0x1002||!f||strcmp(f,"/fixture/alpha.c")||line!=7||!fn||strcmp(fn,"funcA"))abort();
 printf("BASELINE_FRAME pc=0x%lx file=%s line=%d function=%s\n",(unsigned long)pc,f,line,fn);frames++;return 0;}
static void symbol(void *d,uintptr_t pc,const char *name,uintptr_t value,uintptr_t size){(void)d;
 if(pc!=0x1002||!name||strcmp(name,"symbolA")||value!=0x1000||size!=16)abort();
 printf("BASELINE_SYMBOL name=%s value=0x%lx size=%lu\n",name,(unsigned long)value,(unsigned long)size);symbols++;}
int main(void){struct backtrace_state *s;fileline fn;int fd;
 if(!task_write_elf(".task-build/basic.elf","funcA","symbolA","alpha.c",0))return 2;
 s=backtrace_create_state(".task-build/basic.elf",0,err,NULL);if(!s)return 3;
 fd=backtrace_open(".task-build/basic.elf",err,NULL,NULL);if(fd<0)return 4;
 if(!backtrace_initialize(s,".task-build/basic.elf",fd,err,NULL,&fn))return 5;
 s->fileline_fn=fn;
 backtrace_pcinfo(s,0x1002,frame,err,NULL);backtrace_syminfo(s,0x1002,symbol,err,NULL);
 if(errors||frames!=1||symbols!=1)return 6;
 puts("BASELINE_ELF_DWARF_OK");return 0;}
