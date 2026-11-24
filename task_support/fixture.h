#ifndef LOCATION_FIXTURE_H
#define LOCATION_FIXTURE_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
/* Complete, data-only ELF64 object; buffers are deliberately small. */
static void fixture_u16(unsigned char *p,uint16_t v){p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);}
static void fixture_u32(unsigned char *p,uint32_t v){unsigned i;for(i=0;i<4;++i)p[i]=(unsigned char)(v>>(8*i));}
static void fixture_u64(unsigned char *p,uint64_t v){unsigned i;for(i=0;i<8;++i)p[i]=(unsigned char)(v>>(8*i));}
static int fixture_write(const char *path,int with_views){
 unsigned char image[2048]={0},info[128]={0},loc[128]={0};
 const unsigned char abbrev[]={1,0x11,1,0x11,1,0x8c,1,0x17,0,0,2,0x34,0,3,8,2,0x17,0,0,0};
 const char names[]="\0.debug_info\0.debug_abbrev\0.debug_loclists\0.shstrtab\0";
 const unsigned char *parts[4];size_t sizes[4],offsets[4],cursor=64,n=4,k=4,shoff,i;
 uint32_t nameoff=1;FILE *out;
 fixture_u16(info+n,5);n+=2;info[n++]=1;info[n++]=8;fixture_u32(info+n,0);n+=4;
 info[n++]=1;fixture_u64(info+n,0x1000);n+=8;fixture_u32(info+n,12);n+=4;
 info[n++]=2;memcpy(info+n,"value",6);n+=6;fixture_u32(info+n,12);n+=4;info[n++]=0;fixture_u32(info,(uint32_t)n-4);
 fixture_u16(loc+k,5);k+=2;loc[k++]=8;loc[k++]=0;fixture_u32(loc+k,0);k+=4;
 loc[k++]=6;fixture_u64(loc+k,0x1000);k+=8;
 if(with_views){loc[k++]=9;loc[k++]=1;loc[k++]=2;}
 loc[k++]=4;loc[k++]=0;loc[k++]=0;loc[k++]=1;loc[k++]=0x50;
 if(with_views){loc[k++]=9;loc[k++]=0;loc[k++]=0;}
 loc[k++]=4;loc[k++]=0;loc[k++]=16;loc[k++]=1;loc[k++]=0x51;loc[k++]=0;
 fixture_u32(loc,(uint32_t)k-4);
 parts[0]=info;parts[1]=abbrev;parts[2]=loc;parts[3]=(const unsigned char*)names;
 sizes[0]=n;sizes[1]=sizeof(abbrev);sizes[2]=k;sizes[3]=sizeof(names);
 for(i=0;i<4;++i){offsets[i]=cursor;memcpy(image+cursor,parts[i],sizes[i]);cursor+=sizes[i];}
 shoff=(cursor+7)&~(size_t)7;
 memcpy(image,"\177ELF\2\1\1",7);fixture_u16(image+16,1);fixture_u16(image+18,62);fixture_u32(image+20,1);
 fixture_u64(image+40,shoff);fixture_u16(image+52,64);fixture_u16(image+58,64);fixture_u16(image+60,5);fixture_u16(image+62,4);
 for(i=0;i<4;++i){unsigned char *s=image+shoff+(i+1)*64;fixture_u32(s,nameoff);fixture_u32(s+4,i==3?3:1);fixture_u64(s+24,offsets[i]);fixture_u64(s+32,sizes[i]);fixture_u64(s+48,1);nameoff+=(uint32_t)strlen(names+nameoff)+1;}
 out=fopen(path,"wb");if(!out)return 1;i=fwrite(image,1,shoff+5*64,out);if(fclose(out)||i!=shoff+5*64)return 1;return 0;
}
#endif
