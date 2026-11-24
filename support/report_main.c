#include "report_api.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
static unsigned long long num(const char*s){char*e;errno=0;unsigned long long n=strtoull(s,&e,0);if(errno||*e||!*s){fprintf(stderr,"bad number: %s\n",s);exit(4);}return n;}
static int inspect(FILE*f,const char*pcs){
 FILE*p=fopen(pcs,"r");if(!p)return 0;unsigned long long expected[256];size_t nq=0;char line[8192];
 while(fgets(line,sizeof line,p)){char*e=strchr(line,'\n');if(e)*e=0;if(nq==256){fclose(p);return 0;}expected[nq++]=num(line);}fclose(p);if(!nq)return 0;
 unsigned long long seen=0,want=0,frames=0,total=0,attrs=0,refs=0,tables=0;int active=0,sections=0;unsigned long long info_size=0,line_size=0,str_size=0;
 rewind(f);while(fgets(line,sizeof line,f)){
  char*t[32];size_t n=0;char*s=strtok(line,"\t\r\n");while(s&&n<32){t[n++]=s;s=strtok(NULL,"\t\r\n");}if(!n)return 0;
  if(!strcmp(t[0],"S")){
   if(n!=4||active)return 0;num(t[2]);unsigned long long z=num(t[3]);
   if(!strcmp(t[1],".debug_info")){info_size=z;sections|=1;}else if(!strcmp(t[1],".debug_abbrev"))sections|=2;else if(!strcmp(t[1],".debug_line")){line_size=z;sections|=4;}else if(!strcmp(t[1],".debug_str"))str_size=z;
  }else if(!strcmp(t[0],"Q")){
   if(n!=4||sections!=7||(active&&frames!=want)||seen>=nq||num(t[1])!=seen||num(t[2])!=expected[seen])return 0;
   want=num(t[3]);if(want>16)return 0;frames=0;seen++;active=1;
  }else{
   if(!active||n<3||num(t[1])!=seen-1)return 0;unsigned long long d=num(t[2]);if(d>=want)return 0;
   if(!strcmp(t[0],"F")){
    if(n!=14||d!=want-frames-1)return 0;unsigned long long lo=num(t[5]),hi=num(t[6]);if(!(lo<=expected[seen-1]&&expected[seen-1]<hi))return 0;
    if(num(t[3])>=info_size||num(t[4])>=info_size)return 0;frames++;total++;
   }else if(!strcmp(t[0],"A")){
    if(n!=10)return 0;unsigned long long off=num(t[6]),len=num(t[7]);if(!len||off>=info_size||len>info_size-off)return 0;num(t[8]);attrs++;
   }else if(!strcmp(t[0],"R")){
    if(n!=13)return 0;unsigned long long off=num(t[8]),len=num(t[9]);if(!len||off>=info_size||len>info_size-off)return 0;num(t[10]);refs++;
   }else if(!strcmp(t[0],"T")){
    if(n!=14)return 0;unsigned long long off=num(t[7]),len=num(t[8]);if(!len||off>=line_size||len>line_size-off)return 0;tables++;
   }else if(!strcmp(t[0],"U")){
    if(n!=8)return 0;unsigned long long off=num(t[5]),len=num(t[6]);unsigned long long size=!strcmp(t[4],".debug_info")?info_size:!strcmp(t[4],".debug_str")?str_size:0;
    if(!len||off>=size||len>size-off)return 0;
   }else return 0;
  }
 }
 return seen==nq&&frames==want&&total>=10&&attrs>=total*3&&refs>0&&tables>0;
}
static int compare(FILE*a,FILE*b){rewind(a);rewind(b);int x,y;do{x=fgetc(a);y=fgetc(b);if(x!=y)return 0;}while(x!=EOF);return !ferror(a)&&!ferror(b);}
/* Compare query blocks after removing the request index. This checks reverse
   lookup consistency without knowing the target frame values. */
static int blocks(FILE*f,char out[256][65536],size_t*n){
 char line[8192];*n=0;rewind(f);memset(out,0,256*65536);
 while(fgets(line,sizeof line,f)){
  if(line[0]=='S'&&line[1]=='\t')continue;
  char*one=strchr(line,'\t');if(!one)return 0;char*two=strchr(one+1,'\t');if(!two)return 0;
  if(line[0]=='Q'&&line[1]=='\t'){if(*n==256)return 0;(*n)++;}
  if(!*n)return 0;size_t len=strlen(out[*n-1]),more=strlen(two);
  if(len+more+2>=65536)return 0;out[*n-1][len]=line[0];memcpy(out[*n-1]+len+1,two,more+1);
 }
 return *n>0;
}
static FILE*report(const char*elf,const char*pcs,const char*out){
 FILE*f=fopen(out,"w+");if(!f)return NULL;
 if(sample_report(elf,pcs,f)||fflush(f)||!inspect(f,pcs)){fprintf(stderr,"invalid or empty report: %s\n",out);fclose(f);return NULL;}return f;
}
int main(int argc,char**argv){
 if(argc!=8)return 2;
 /* A, B, A: catches stale cross-object state in the normal invocation. */
 FILE*a=report(argv[1],argv[2],argv[5]);if(!a)return 3;
 FILE*b=report(argv[3],argv[4],argv[6]);if(!b)return 3;
 FILE*c=report(argv[1],argv[2],argv[7]);if(!c)return 3;
 if(!compare(a,c)){fprintf(stderr,"object replay changed\n");return 5;}
 char reverse_pcs[4096],reverse_out[4096];
 if(snprintf(reverse_pcs,sizeof reverse_pcs,"%s.reverse.pcs",argv[7])>=(int)sizeof reverse_pcs||snprintf(reverse_out,sizeof reverse_out,"%s.reverse.tsv",argv[7])>=(int)sizeof reverse_out)return 5;
 FILE*p=fopen(argv[2],"r");if(!p)return 5;char pcs[256][128];size_t n=0;
 while(n<256&&fgets(pcs[n],sizeof pcs[n],p))n++;fclose(p);p=fopen(reverse_pcs,"w");if(!p)return 5;
 for(size_t i=n;i>0;i--)fputs(pcs[i-1],p);fclose(p);
 FILE*d=report(argv[1],reverse_pcs,reverse_out);if(!d)return 5;
 char(*forward)[65536]=malloc(256*65536),(*reverse)[65536]=malloc(256*65536);size_t na,nd;
 if(!forward||!reverse||!blocks(a,forward,&na)||!blocks(d,reverse,&nd)||na!=nd)return 5;
 for(size_t i=0;i<na;i++)if(strcmp(forward[i],reverse[na-1-i])){fprintf(stderr,"reverse query changed witness\n");return 5;}
 free(forward);free(reverse);fclose(d);
 fclose(a);fclose(b);fclose(c);puts("SAMPLE_REPORTS_RETURNED objects=2 replay=1");return 0;
}
