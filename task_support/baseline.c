#include "fixture.h"
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
static int normal(int bits) {
    unsigned char b[FIXTURE_SIZE]; pe_ctx_t c;
    fixture(b,bits);
    if(fixture_open(&c,b,sizeof(b))!=LIBPE_E_OK) return 1;
    pe_exports_t *e=pe_exports(&c);
    int bad=!e || e->err || e->functions_count!=2 || !e->functions
        || strcmp(e->name,"fixture.dll") || strcmp(e->functions[0].name,"Alpha")
        || e->functions[0].ordinal!=7 || strcmp(e->functions[1].fwd_name,"other.Func");
    if(!bad) printf("NORMAL bits=%d count=2 ordinal=7 name=Alpha fwd=other.Func\n",bits);
    fixture_close(&c); return bad;
}
int main(void) {
    if(normal(32)||normal(64)) return 1;
    unsigned char b[FIXTURE_SIZE]; pe_ctx_t c;
    fixture(b,32); put32(b,0x21c,0x1dfc); put32(b,0xffc,0x1400);
    if(fixture_open(&c,b,sizeof(b))) return 2;
    pe_exports_t *e=pe_exports(&c);
    printf("SHORT_TABLE err=%d count=%u partial=%d\n",e?e->err:999,e?e->functions_count:0,e&&e->functions!=NULL);
    int wrap=pe_can_read(&c,(const void *)(UINTPTR_MAX-7),16);
    printf("WRAP_RANGE accepted=%d desired=0\n",wrap);
    fixture_close(&c);
    pid_t pid=fork(); if(pid<0) return 3;
    if(pid==0) {
        struct rlimit lim={1024*1024,1024*1024};
        struct rlimit core={0,0};
        if(setrlimit(RLIMIT_STACK,&lim)||setrlimit(RLIMIT_CORE,&core)) _exit(90);
        unsigned char *p=malloc(FIXTURE_SIZE); if(!p) _exit(91);
        fixture(p,64); put32(p,0x214,0x10000000);
        pe_ctx_t child;
        if(fixture_open(&child,p,FIXTURE_SIZE)) _exit(92);
        pe_exports_t *r=pe_exports(&child);
        int ok=r && r->err!=LIBPE_E_OK && !r->functions && r->functions_count==0;
        fixture_close(&child); free(p); _exit(ok?0:93);
    }
    int st; if(waitpid(pid,&st,0)!=pid) return 4;
    printf("HUGE_COUNT signal=%d exit=%d desired_exit=0\n",WIFSIGNALED(st)?WTERMSIG(st):0,WIFEXITED(st)?WEXITSTATUS(st):-1);
    puts("BASELINE_OBSERVED"); return 0;
}
