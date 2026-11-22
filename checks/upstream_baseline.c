/* Finite benign observation of the real six bitflip stages. Apache-2.0. */
#define AFL_TASK_PROBE
#define main afl_original_main
#include "../afl-fuzz.c"
#undef main
#include <stdint.h>

static u8 original[160];
static u64 calls[6], digest=1469598103934665603ULL;
static int boundary_seen, failure;
static const char *names[6]={"flip1","flip2","flip4","flip8","flip16","flip32"};

static u8 observe(char **argv,u8 *data,u32 len) {
  unsigned s, i; int active=0;
  (void)argv;
  for(s=0;s<6;s++) if(!strcmp((char*)stage_short,names[s])) break;
  if(s==6 || len!=sizeof original) {failure=1;stop_soon=1;return 1;}
  calls[s]++;
  digest^=s; digest*=1099511628211ULL;
  digest^=(u32)stage_cur_byte; digest*=1099511628211ULL;
  for(i=0;i<len;i++){digest^=data[i];digest*=1099511628211ULL;}
  for(i=16;i<24;i++) if(data[i]!=original[i]) active=1;
  memset(trace_bits,0,MAP_SIZE); trace_bits[10]=(u8)active;
  total_execs++;
  return 0;
}

static void boundary(const u8 *map,u32 len) {
  unsigned i; int token=0;
  boundary_seen++;
  if(len!=20)failure=1;
  for(i=0;i<len;i++)if(!!map[i]!=(i==0||i==2||i==19))failure=1;
  for(i=0;i<a_extras_cnt;i++)if(a_extras[i].len==8 && !memcmp(a_extras[i].data,original+16,8))token=1;
  if(!token || a_extras_cnt!=1)failure=1;
  stop_soon=1;
}

int main(void) {
  unsigned i; int fd; struct queue_entry q;
  const char *file=".task-build/benign-input";
  memset(&q,0,sizeof q);
  for(i=0;i<sizeof original;i++)original[i]=(u8)('a'+i%23);
  fd=open(file,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return 2;
  if(write(fd,original,sizeof original)!=(ssize_t)sizeof original)return 2;
  close(fd);
  trace_bits=calloc(1,MAP_SIZE);if(!trace_bits)return 2;
  q.fname=(u8*)file;q.len=sizeof original;q.depth=1;q.favored=1;q.trim_done=1;q.exec_us=1;q.bitmap_size=1;
  q.exec_cksum=hash32(trace_bits,MAP_SIZE,HASH_CONST);
  queue_cur=queue=&q;queued_paths=1;pending_not_fuzzed=1;pending_favored=0;
  total_cal_us=total_cal_cycles=total_bitmap_size=total_bitmap_entries=1;
  afl_task_execute_hook=observe;afl_task_boundary_hook=boundary;
  (void)fuzz_one(NULL);
  if(calls[0]!=1280||calls[1]!=1279||calls[2]!=1277||calls[3]!=160||!calls[4]||!calls[5])failure=1;
  for(i=0;i<6;i++)if(stage_cycles[i]!=calls[i])failure=1;
  if(boundary_seen!=1||q.was_fuzzed||q.passed_det)failure=1;
  if(memcmp(original,"abcdefgh",8))failure=1;
  if(failure){fprintf(stderr,"baseline content mismatch boundary=%d tokens=%u\n",boundary_seen,a_extras_cnt);return 3;}
  printf("AFL_PRELUDE_BASELINE_OK events=%llu,%llu,%llu,%llu,%llu,%llu digest=%016llx tokens=%u effector=3/20\n",calls[0],calls[1],calls[2],calls[3],calls[4],calls[5],digest,a_extras_cnt);
  for(i=0;i<a_extras_cnt;i++)ck_free(a_extras[i].data);
  ck_free(a_extras);free(trace_bits);unlink(file);return 0;
}
