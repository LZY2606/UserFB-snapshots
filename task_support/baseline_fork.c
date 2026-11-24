#include "theft.h"
#include <unistd.h>
#include <string.h>
static pid_t parent_pid;
static int live, posts;
static enum theft_alloc_res alloc_data(struct theft *t, void *env, void **out) {
    (void)t; (void)env;
    unsigned char *p=malloc(16);
    if (!p) return THEFT_ALLOC_ERROR;
    for (size_t i=0;i<16;i++) p[i]=(unsigned char)(3*i);
    live++;*out=p;return THEFT_ALLOC_OK;
}
static void release(void *p, void *env) {(void)env;if(p){live--;free(p);}}
static enum theft_trial_res prop(struct theft *t, void *arg) {
    (void)t; const unsigned char *p=arg;
    if(getpid()==parent_pid) return THEFT_TRIAL_ERROR;
    for(size_t i=0;i<16;i++) if(p[i]!=(unsigned char)(3*i))return THEFT_TRIAL_ERROR;
    return THEFT_TRIAL_PASS;
}
static enum theft_hook_trial_post_res post(const struct theft_hook_trial_post_info *i, void *e) {
    (void)e;
    if(getpid()!=parent_pid || i->result!=THEFT_TRIAL_PASS)return THEFT_HOOK_TRIAL_POST_ERROR;
    posts++;return THEFT_HOOK_TRIAL_POST_CONTINUE;
}
int main(void) {
    parent_pid=getpid();
    struct theft_type_info ti={.alloc=alloc_data,.free=release};
    struct theft_run_config c={.prop1=prop,.type_info={&ti},.trials=3,.seed=1814,.fork={.enable=true,.timeout=2000},.hooks={.trial_post=post}};
    enum theft_run_res r=theft_run(&c);
    if(r!=THEFT_RUN_PASS || posts!=3 || live!=0)return 18;
    puts("THEFT_FORK_BASELINE_OK trials=3 parent_posts=3 live=0");return 0;
}
