/* Original deterministic PCM source. CC0-1.0. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv){
 if(argc!=3)return 2;
 int channels=atoi(argv[2]);FILE *f=fopen(argv[1],"wb");if(!f)return 3;
 for(int n=0;n<1152*8;n++)for(int c=0;c<channels;c++){
  int period=c?173:97;
  int v=((n%period)*10000/period-5000)+(((n/(c?31:23))%2)?1200:-1200);
  uint16_t u=(uint16_t)(int16_t)v;
  if(fputc(u&255,f)==EOF||fputc(u>>8,f)==EOF)return 4;
 }
 return fclose(f)!=0;
}
