/* Original deterministic fixture generator, dedicated to the public domain (CC0-1.0). */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
int main(int argc,char **argv){
 if(argc!=2)return 2;
 int ch=atoi(argv[1]);if(ch!=5&&ch!=6)return 3;
 const int freq[6]={311,487,673,83,997,1291};
 for(int n=0;n<1536*12;n++)for(int c=0;c<ch;c++){
  int k=(ch==5&&c>=3)?c+1:c;
  double v=2400*sin(n*(6.2831853071795864769*freq[k]/48000.0)+k*.21);
  if(k!=3 && n%2101<5)v+=1700-k*100;
  int16_t s=(int16_t)v;unsigned u=(uint16_t)s;
  putchar(u&255);putchar((u>>8)&255);
 }
 return ferror(stdout)?4:0;
}
