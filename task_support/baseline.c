#include "fixture.h"
#include <libelf.h>
#include <gelf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int inspect(int zipped,const char *save){
  size_t n=0;unsigned char *b=task_fixture(zipped,&n);if(!b)return 1;
  if(save){FILE *f=fopen(save,"wb");if(!f){free(b);return 2;}if(fwrite(b,1,n,f)!=n){fclose(f);free(b);return 2;}fclose(f);}
  Elf *e=elf_memory((char*)b,n);if(!e){free(b);return 3;}
  Elf_Scn *s=elf_getscn(e,2);Elf_Data *raw=elf_rawdata(s,NULL),*d=elf_getdata(s,NULL);GElf_Sym sym;Elf32_Word x=0;
  Elf_Data *id=elf_getdata(elf_getscn(e,4),NULL);int got=d&&id&&gelf_getsymshndx(d,id,1,&sym,&x)!=NULL;
  char *name=elf_strptr(e,3,192);
  Elf_Data *string_data=elf_getdata(elf_getscn(e,3),NULL);
  GElf_Rela rel;int gotrel=gelf_getrela(elf_getdata(elf_getscn(e,5),NULL),0,&rel)!=NULL;
  printf("%s string_view=%zu rela=%s\n",zipped?"COMPRESSED_VIEW":"PLAIN_VIEW",string_data?(size_t)string_data->d_size:0,gotrel?"OK":"NULL");
  printf("%s sym=%s name=%s section=%u value=%llx raw=%zu logical=%zu\n",zipped?"COMPRESSED_CURRENT":"PLAIN",got?"OK":"NULL",name?name:"NULL",got?x:0,got?(unsigned long long)sym.st_value:0,raw?(size_t)raw->d_size:0,d?(size_t)d->d_size:0);
  int ok=raw&&raw->d_size>0&&(zipped||(got&&x==1&&sym.st_value==0x40&&name&&!strcmp(name,"target")));
  elf_end(e);free(b);return ok?0:4;
}
int main(int argc,char **argv){if(elf_version(EV_CURRENT)==EV_NONE)return 1;if(inspect(0,argc>1?argv[1]:NULL)||inspect(1,argc>2?argv[2]:NULL))return 2;puts("BASELINE_OK");return 0;}
