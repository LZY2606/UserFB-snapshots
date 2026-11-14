#include "basic_fixture.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
struct buf {unsigned char b[8192];size_t n;};
static void u8(struct buf *b,unsigned x){assert(b->n<sizeof b->b);b->b[b->n++]=(unsigned char)x;}
static void num(struct buf *b,uint64_t x,unsigned n){unsigned i;for(i=0;i<n;i++){u8(b,(unsigned)x);x>>=8;}}
static void str(struct buf *b,const char *s){do{u8(b,(unsigned char)*s);}while(*s++);}
static void patch(unsigned char *b,size_t o,uint64_t x,unsigned n){unsigned i;for(i=0;i<n;i++){b[o+i]=(unsigned char)x;x>>=8;}}
struct sec {const char *name;uint32_t type;uint64_t flags,addr;struct buf data;uint32_t link,info;uint64_t entsize;};
int task_write_elf(const char *path,const char *fn,const char *sym,const char *source,int dynamic){
 struct sec s[9]={0};struct buf file={0};uint32_t names[9]={0};size_t offsets[9]={0},sho,i,o,header_end;FILE *fp;
 unsigned char abbrev[]={1,0x11,1,3,8,0x1b,8,0x10,0x17,0x11,1,0x12,6,0,0,
 2,0x2e,0,3,8,0x11,1,0x12,6,0x3a,0xb,0x3b,0xb,0,0,0};
 unsigned char lengths[]={0,1,1,1,1,0,0,0,1,0,0,1};
 s[1].name=".text";s[1].type=1;s[1].flags=6;s[1].addr=0x1000;
 for(i=0;i<16;i++)u8(&s[1].data,0x90);
 s[2].name=".strtab";s[2].type=3;u8(&s[2].data,0);str(&s[2].data,sym);
 s[3].name=".symtab";s[3].type=2;s[3].link=2;s[3].info=1;s[3].entsize=24;
 for(i=0;i<24;i++)u8(&s[3].data,0);num(&s[3].data,1,4);u8(&s[3].data,0x12);u8(&s[3].data,0);num(&s[3].data,1,2);num(&s[3].data,0x1000,8);num(&s[3].data,16,8);
 s[4].name=".debug_abbrev";s[4].type=1;for(i=0;i<sizeof abbrev;i++)u8(&s[4].data,abbrev[i]);
 s[5].name=".debug_info";s[5].type=1;
 num(&s[5].data,0,4);num(&s[5].data,4,2);num(&s[5].data,0,4);u8(&s[5].data,8);
 u8(&s[5].data,1);str(&s[5].data,"unit");str(&s[5].data,"/fixture");num(&s[5].data,0,4);num(&s[5].data,0x1000,8);num(&s[5].data,16,4);
 u8(&s[5].data,2);str(&s[5].data,fn);num(&s[5].data,0x1000,8);num(&s[5].data,16,4);u8(&s[5].data,1);u8(&s[5].data,7);u8(&s[5].data,0);patch(s[5].data.b,0,s[5].data.n-4,4);
 s[6].name=".debug_line";s[6].type=1;
 num(&s[6].data,0,4);num(&s[6].data,4,2);num(&s[6].data,0,4);
 u8(&s[6].data,1);u8(&s[6].data,1);u8(&s[6].data,1);u8(&s[6].data,251);u8(&s[6].data,14);u8(&s[6].data,13);
 for(i=0;i<12;i++)u8(&s[6].data,lengths[i]);u8(&s[6].data,0);str(&s[6].data,source);u8(&s[6].data,0);u8(&s[6].data,0);u8(&s[6].data,0);u8(&s[6].data,0);header_end=s[6].data.n;patch(s[6].data.b,6,header_end-10,4);
 u8(&s[6].data,0);u8(&s[6].data,9);u8(&s[6].data,2);num(&s[6].data,0x1000,8);
 u8(&s[6].data,3);u8(&s[6].data,6);u8(&s[6].data,1);
 u8(&s[6].data,2);u8(&s[6].data,8);u8(&s[6].data,3);u8(&s[6].data,1);u8(&s[6].data,1);
 u8(&s[6].data,2);u8(&s[6].data,8);u8(&s[6].data,0);u8(&s[6].data,1);u8(&s[6].data,1);patch(s[6].data.b,0,s[6].data.n-4,4);
 s[7].name=".shstrtab";s[7].type=3;u8(&s[7].data,0);
 for(i=1;i<8;i++){names[i]=(uint32_t)s[7].data.n;str(&s[7].data,s[i].name);}
 for(i=0;i<64;i++)u8(&file,0);
 for(i=1;i<8;i++){while(file.n%8)u8(&file,0);offsets[i]=file.n;for(o=0;o<s[i].data.n;o++)u8(&file,s[i].data.b[o]);}
 while(file.n%8)u8(&file,0);sho=file.n;for(i=0;i<8*64;i++)u8(&file,0);
 memcpy(file.b,"\177ELF\2\1\1",7);patch(file.b,16,dynamic?3:2,2);patch(file.b,18,62,2);patch(file.b,20,1,4);patch(file.b,40,sho,8);patch(file.b,52,64,2);patch(file.b,58,64,2);patch(file.b,60,8,2);patch(file.b,62,7,2);
 for(i=1;i<8;i++){o=sho+i*64;patch(file.b,o,names[i],4);patch(file.b,o+4,s[i].type,4);patch(file.b,o+8,s[i].flags,8);patch(file.b,o+16,s[i].addr,8);patch(file.b,o+24,offsets[i],8);patch(file.b,o+32,s[i].data.n,8);patch(file.b,o+40,s[i].link,4);patch(file.b,o+44,s[i].info,4);patch(file.b,o+48,1,8);patch(file.b,o+56,s[i].entsize,8);}
 fp=fopen(path,"wb");if(!fp)return 0;i=fwrite(file.b,1,file.n,fp);return fclose(fp)==0&&i==file.n;
}
