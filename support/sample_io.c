#include "config.h"
#include "backtrace.h"
#include "sample_io.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
static uint64_t le(const unsigned char*p,int n){uint64_t v=0;for(int i=0;i<n;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static int span(size_t sz,uint64_t off,uint64_t n){return off<=sz && n<=sz-off;}
static void error(void*d,const char*s,int e){(void)d;fprintf(stderr,"libbacktrace: %s (%d)\n",s,e);}
int sample_open(const char*path,struct sample_object*o){
 memset(o,0,sizeof *o);FILE*f=fopen(path,"rb");if(!f)return 0;
 if(fseek(f,0,SEEK_END)||ftell(f)<64){fclose(f);return 0;}o->size=(size_t)ftell(f);rewind(f);
 o->bytes=malloc(o->size);if(!o->bytes){fclose(f);return 0;}
 if(fread(o->bytes,1,o->size,f)!=o->size){fclose(f);return 0;}fclose(f);
 unsigned char*b=o->bytes;if(memcmp(b,"\177ELF\2\1\1",7))return 0;
 uint64_t sh=le(b+40,8),sn=le(b+60,2),si=le(b+62,2),se=le(b+58,2);
 if(se!=64||si>=sn||!span(o->size,sh,sn*se))return 0;
 unsigned char*s=b+sh+si*se;uint64_t so=le(s+24,8),sz=le(s+32,8);if(!span(o->size,so,sz))return 0;
 const char*names[DEBUG_MAX]={".debug_info",".debug_line",".debug_abbrev",".debug_ranges",".debug_str",".debug_addr",".debug_str_offsets",".debug_line_str",".debug_rnglists"};
 for(uint64_t i=0;i<sn;i++){
  s=b+sh+i*se;uint64_t no=le(s,4),off=le(s+24,8),len=le(s+32,8);
  if(no>=sz||!memchr(b+so+no,0,sz-no)||!span(o->size,off,len))return 0;
  for(int j=0;j<DEBUG_MAX;j++)if(!strcmp((char*)b+so+no,names[j])){o->sections.data[j]=b+off;o->sections.size[j]=len;o->offsets[j]=off;}
 }
 if(!o->sections.size[DEBUG_INFO]||!o->sections.size[DEBUG_ABBREV]||!o->sections.size[DEBUG_LINE])return 0;
 o->state=backtrace_create_state(NULL,0,error,NULL);if(!o->state)return 0;
 struct libbacktrace_base_address base={0};
 return backtrace_dwarf_add(o->state,base,&o->sections,0,NULL,error,NULL,&o->lookup,&o->entry);
}
int sample_read_pcs(const char*path,uintptr_t*p,size_t cap,size_t*n){
 FILE*f=fopen(path,"r");if(!f)return 0;char line[128];*n=0;
 while(fgets(line,sizeof line,f)){char*end;errno=0;unsigned long long x=strtoull(line,&end,0);if(errno||end==line||(*end!='\n'&&*end!=0)||*n==cap){fclose(f);return 0;}p[(*n)++]=(uintptr_t)x;}
 int ok=!ferror(f)&&*n>0;fclose(f);return ok;
}
