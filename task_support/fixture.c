/* SPDX-License-Identifier: MIT */
#include "fixture.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct section { const char *name; const unsigned char *data; size_t size; unsigned nameoff, off; };
static void put(unsigned char *p, uint64_t v, unsigned n) {
    unsigned i; for(i=0;i<n;++i) { p[i]=(unsigned char)v; v>>=8; }
}
static int elf(const char *path, struct section *s, unsigned count) {
    unsigned char data[8192]={0},names[256]={0};
    unsigned i,no=1,pos=64,shoff,n=count+2;
    FILE *f;
    for(i=0;i<count;++i) {
        s[i].nameoff=no; memcpy(names+no,s[i].name,strlen(s[i].name)+1);
        no+=(unsigned)strlen(s[i].name)+1;
        s[i].off=pos; memcpy(data+pos,s[i].data,s[i].size); pos+=(unsigned)s[i].size;
    }
    { unsigned nameoff=pos,shname=no;
      memcpy(names+no,".shstrtab",10); no+=10;
      memcpy(data+pos,names,no); pos+=no; shoff=(pos+7)&~7u;
      memcpy(data,"\177ELF\2\1\1",7);
      put(data+16,2,2); put(data+18,62,2); put(data+20,1,4);
      put(data+40,shoff,8); put(data+52,64,2); put(data+58,64,2);
      put(data+60,n,2); put(data+62,n-1,2);
      for(i=0;i<count;++i) {
          unsigned char *h=data+shoff+64*(i+1);
          put(h,s[i].nameoff,4); put(h+4,1,4); put(h+24,s[i].off,8);
          put(h+32,s[i].size,8); put(h+48,1,8);
      }
      { unsigned char *h=data+shoff+64*(n-1);
        put(h,shname,4); put(h+4,3,4); put(h+24,nameoff,8); put(h+32,no,8); put(h+48,1,8); }
    }
    f=fopen(path,"wb"); if(!f) return 1;
    i=fwrite(data,1,shoff+64*n,f)==shoff+64*n ? 0:1;
    return fclose(f) ? 1:(int)i;
}
int task_basic_fixtures(const char *directory) {
    /* Abbrev: compile_unit, no children, name, macros, stmt_list. */
    static const unsigned char abbrev[]={1,0x11,0,3,8,0x79,0x17,0x10,0x17,0,0,0};
    static const unsigned char info[]={24,0,0,0,5,0,1,8,0,0,0,0,
        1,'m','a','i','n','.','c',0,0,0,0,0,0,0,0,0};
    /* DWARF5 line header: one directory/file, only end_sequence. */
    static const unsigned char line[]={49,0,0,0,5,0,8,0,38,0,0,0,
        1,1,1,0xfb,14,13,0,1,1,1,1,0,0,0,1,0,0,1,
        1,1,8,1,'.',0,
        2,1,8,2,0x0b,1,'m','a','i','n','.','c',0,0,
        0,1,1};
    static const unsigned char mainmac[]={5,0,2,0,0,0,0,
        3,1,0,5,1,0,0,0,0,10,0,0,0,0,4,0};
    static const unsigned char supmac[]={5,0,0,5,1,0,0,0,0,0};
    static const unsigned char mainstr[]="MAIN_OWNER 1";
    static const unsigned char supstr[]="SUP_OWNER 9";
    struct section mainsec[]={
        {".debug_abbrev",abbrev,sizeof(abbrev),0,0},
        {".debug_info",info,sizeof(info),0,0},
        {".debug_line",line,sizeof(line),0,0},
        {".debug_macro",mainmac,sizeof(mainmac),0,0},
        {".debug_str",mainstr,sizeof(mainstr),0,0}};
    struct section supsec[]={
        {".debug_macro",supmac,sizeof(supmac),0,0},
        {".debug_str",supstr,sizeof(supstr),0,0}};
    char path[4096];
    if(snprintf(path,sizeof(path),"%s/main.elf",directory)>=(int)sizeof(path)) return 1;
    if(elf(path,mainsec,5)) return 1;
    if(snprintf(path,sizeof(path),"%s/sup.elf",directory)>=(int)sizeof(path)) return 1;
    return elf(path,supsec,2);
}
