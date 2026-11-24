#include "config.h"
#include "backtrace.h"
#include "sample_io.h"
#include <inttypes.h>
#include <stdlib.h>
struct ctx {size_t q,frames;uintptr_t pc;};
static void err(void*d,const char*s,int n){(void)d;fprintf(stderr,"lookup: %s (%d)\n",s,n);exit(3);}
static int frame(void*d,uintptr_t pc,const char*file,int line,const char*name){struct ctx*c=d;c->frames++;printf("FRAME q=%zu pc=0x%"PRIxPTR" name=%s file=%s line=%d\n",c->q,pc,name?name:"unknown",file?file:"unknown",line);return 0;}
int main(int argc,char**argv){
 if(argc!=3)return 2;struct sample_object o;uintptr_t pcs[128];size_t n;
 if(!sample_open(argv[1],&o)||!sample_read_pcs(argv[2],pcs,128,&n))return 2;
 size_t total=0;for(size_t i=0;i<n;i++){struct ctx c={i,0,pcs[i]};if(o.lookup(o.state,pcs[i],frame,err,&c))return 3;total+=c.frames;}
 if(total<10)return 4;
 printf("BASELINE_OK queries=%zu callbacks=%zu\n",n,total);return 0;
}
