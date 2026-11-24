#include "fixture.h"
#include <libelf.h>
#include <stdlib.h>
#include <string.h>
#include "vendor/zlib/zlib.h"
static void put(unsigned char *p,unsigned long long v,size_t n){for(size_t i=0;i<n;++i)p[i]=(unsigned char)(v>>(8*i));}
static unsigned char *section(const unsigned char *src,size_t n,int zip,unsigned align,size_t *out){
  if(!zip){unsigned char *b=malloc(n);if(b)memcpy(b,src,n);*out=n;return b;}
  uLongf cap=compressBound(n);unsigned char *b=calloc(1,24+cap);if(!b)return NULL;
  put(b,1,4);put(b+8,n,8);put(b+16,align,8);
  if(compress2(b+24,&cap,src,n,9)!=Z_OK){free(b);return NULL;}*out=24+cap;return b;
}
unsigned char *task_fixture(int compressed,size_t *length){
  unsigned char text[96]={0},sym[48]={0},str[256]={0},idx[8]={0},rela[24]={0};
  put(sym+24,192,4);sym[28]=0x12;put(sym+30,SHN_XINDEX,2);put(sym+32,0x40,8);put(sym+40,8,8);
  memcpy(str+192,"target",7);put(idx+4,1,4);put(rela,8,8);put(rela+8,(1ULL<<32)|2,8);put(rela+16,(unsigned long long)-4LL,8);
  const unsigned char *input[]={text,sym,str,idx,rela};size_t lens[]={96,48,256,8,24};unsigned aligns[]={16,8,1,4,8};
  unsigned types[]={SHT_PROGBITS,SHT_SYMTAB,SHT_STRTAB,SHT_SYMTAB_SHNDX,SHT_RELA};
  unsigned links[]={0,3,0,2,2},infos[]={0,1,0,0,1},ents[]={0,24,0,4,24};
  unsigned char *data[5]={0};size_t sizes[5],positions[5],next=64+6*64;
  for(size_t i=0;i<5;++i){int zip=compressed&&i>0;data[i]=section(input[i],lens[i],zip,aligns[i],&sizes[i]);if(!data[i])goto fail;next=(next+15)&~(size_t)15;positions[i]=next;next+=sizes[i];}
  unsigned char *b=calloc(1,next);if(!b)goto fail;memcpy(b,"\177ELF",4);b[EI_CLASS]=ELFCLASS64;b[EI_DATA]=ELFDATA2LSB;b[EI_VERSION]=EV_CURRENT;
  put(b+16,ET_REL,2);put(b+18,EM_X86_64,2);put(b+20,EV_CURRENT,4);put(b+40,64,8);put(b+52,64,2);put(b+58,64,2);put(b+60,6,2);
  for(size_t i=0;i<5;++i){unsigned char *h=b+64+(i+1)*64;int zip=compressed&&i>0;put(h+4,types[i],4);put(h+8,i==0?SHF_ALLOC|SHF_EXECINSTR:(zip?SHF_COMPRESSED:0),8);put(h+24,positions[i],8);put(h+32,sizes[i],8);put(h+40,links[i],4);put(h+44,infos[i],4);put(h+48,zip?8:aligns[i],8);put(h+56,ents[i],8);memcpy(b+positions[i],data[i],sizes[i]);free(data[i]);}
  *length=next;return b;
fail:for(size_t i=0;i<5;++i)free(data[i]);return NULL;
}
