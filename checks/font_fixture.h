#ifndef UFB_FONT_FIXTURE_H
#define UFB_FONT_FIXTURE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static void ff_u(FILE*f,unsigned x,int n){while(n--){fputc(x&255,f);x>>=8;}}
static long ff_begin(FILE*f,const char id[4]){fwrite(id,1,4,f);long p=ftell(f);ff_u(f,0,4);return p;}
static void ff_end(FILE*f,long p){long e=ftell(f);fseek(f,p,SEEK_SET);ff_u(f,(unsigned)(e-p-4),4);fseek(f,e,SEEK_SET);if((e-p-4)&1)fputc(0,f);}
static void ff_name(FILE*f,const char*s){char b[20]={0};strncpy(b,s,19);fwrite(b,1,20,f);}
static void ff_gen(FILE*f,unsigned op,unsigned amt){ff_u(f,op,2);ff_u(f,amt,2);}
/* Legal minimal SF2, generated from two original periodic PCM signals. No audio package. */
static tsf* make_font(int layers,unsigned group){
 FILE*f=tmpfile();if(!f)return NULL;long riff=ff_begin(f,"RIFF");fwrite("sfbk",1,4,f);
 long list=ff_begin(f,"LIST");fwrite("INFO",1,4,f);long c=ff_begin(f,"ifil");ff_u(f,2,2);ff_u(f,1,2);ff_end(f,c);ff_end(f,list);
 list=ff_begin(f,"LIST");fwrite("sdta",1,4,f);c=ff_begin(f,"smpl");
 for(int s=0;s<2;s++){for(int i=0;i<256;i++){int x=(int)(10000*sin(6.283185307179586*(s+1)*i/64));ff_u(f,(unsigned)(unsigned short)x,2);}for(int i=0;i<46;i++)ff_u(f,0,2);}ff_end(f,c);ff_end(f,list);
 list=ff_begin(f,"LIST");fwrite("pdta",1,4,f);
 c=ff_begin(f,"phdr");for(int i=0;i<3;i++){ff_name(f,i==2?"EOP":"Fixture");ff_u(f,i==2?0:i,2);ff_u(f,0,2);ff_u(f,i,2);ff_u(f,0,4);ff_u(f,0,4);ff_u(f,0,4);}ff_end(f,c);
 c=ff_begin(f,"pbag");for(int i=0;i<3;i++){ff_u(f,i,2);ff_u(f,0,2);}ff_end(f,c);
 c=ff_begin(f,"pmod");for(int i=0;i<10;i++)fputc(0,f);ff_end(f,c);
 c=ff_begin(f,"pgen");ff_gen(f,41,0);ff_gen(f,41,0);ff_gen(f,0,0);ff_end(f,c);
 c=ff_begin(f,"inst");ff_name(f,"Layered");ff_u(f,0,2);ff_name(f,"EOI");ff_u(f,layers,2);ff_end(f,c);
 c=ff_begin(f,"ibag");for(int i=0;i<=layers;i++){ff_u(f,i*6,2);ff_u(f,0,2);}ff_end(f,c);
 c=ff_begin(f,"imod");for(int i=0;i<10;i++)fputc(0,f);ff_end(f,c);
 c=ff_begin(f,"igen");for(int i=0;i<layers;i++){ff_gen(f,17,(unsigned short)(i%2?500:-500));ff_gen(f,34,(unsigned short)-12000);ff_gen(f,38,(unsigned short)-1200);ff_gen(f,54,1);ff_gen(f,57,group);ff_gen(f,53,i%2);}ff_gen(f,0,0);ff_end(f,c);
 c=ff_begin(f,"shdr");for(int i=0;i<3;i++){ff_name(f,i==2?"EOS":"Wave");unsigned st=i*302;ff_u(f,st,4);ff_u(f,i==2?st:st+256,4);ff_u(f,i==2?st:st+8,4);ff_u(f,i==2?st:st+248,4);ff_u(f,22050,4);ff_u(f,60,1);ff_u(f,0,1);ff_u(f,0,2);ff_u(f,1,2);}ff_end(f,c);ff_end(f,list);ff_end(f,riff);
 long n=ftell(f);rewind(f);void*b=malloc((size_t)n);if(!b){fclose(f);return NULL;}if(fread(b,1,(size_t)n,f)!=(size_t)n){free(b);fclose(f);return NULL;}fclose(f);tsf*t=tsf_load_memory(b,(int)n);free(b);return t;
}
#endif
