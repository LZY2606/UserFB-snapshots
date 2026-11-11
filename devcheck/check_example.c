#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv){
  assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);
  unsigned char h[27],laces[255];int pages=0,eos=0,preskip=-1;uint64_t final=0;
  while(fread(h,1,27,f)==27){
    assert(!memcmp(h,"OggS",4));assert(fread(laces,1,h[26],f)==h[26]);size_t n=0;
    for(int i=0;i<h[26];i++)n+=laces[i];unsigned char *p=malloc(n?n:1);assert(p);
    assert(fread(p,1,n,f)==n);
    if(n>=19&&!memcmp(p,"OpusHead",8))preskip=p[10]|p[11]<<8;
    if(h[5]&4){eos++;for(int j=7;j>=0;j--)final=(final<<8)|h[6+j];}
    free(p);pages++;
  }
  assert(feof(f)&&pages>=3&&eos==1&&preskip>=0&&final-(uint64_t)preskip==4800);
  fclose(f);puts("OPUSENC_44100_EXAMPLE_OK");
}
