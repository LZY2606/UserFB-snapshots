#ifndef REDOS_MODEL_H
#define REDOS_MODEL_H

#include <stdint.h>
#include <stddef.h>

#define RB_MAX_TEXT 32
#define RB_MAX_PAT 320
#define RB_MAX_FRAG 10
#define RB_MAX_CAPTURES 4
#define RB_MAX_POSITIONS (RB_MAX_TEXT + 1)
#define RB_BRANCH_ATTEMPT_LIMIT 65536
#define RB_REDUCTION_ATTEMPTS 48

enum rb_kind {
  RB_KIND_BRANCH = 0,
  RB_KIND_CALL = 1,
  RB_KIND_RECURSE = 2,
  RB_KIND_RECALL = 3
};

enum rb_direction { RB_FORWARD = 0, RB_BACKWARD = 1 };

enum rb_plan {
  RB_PLAN_NONE = 0,
  RB_PLAN_RETRY_TOTAL = 1,
  RB_PLAN_CALL_TOTAL = 2
};

typedef struct rb_capture {
  int beg;
  int end;
} rb_capture;

typedef struct rb_reference {
  int match;
  int pos;
  int end;
  int num_regs;
  rb_capture cap[RB_MAX_CAPTURES];
} rb_reference;

typedef struct rb_spec {
  char group[16];
  unsigned seed;
  int case_id;

  int kind;
  char frag[2][RB_MAX_FRAG + 1];
  int frag_len[2];
  int nf;
  int order_swapped;

  int lo;
  int hi;
  int na;
  char suffix;
  int depth;

  int direction;
  int plan;
  int observe;

  unsigned long long expansion;
  int allowed;

  int pat_len;
  int text_len;
  char pat[RB_MAX_PAT];
  char text[RB_MAX_TEXT + 1];
} rb_spec;

uint64_t rb_splitmix64(uint64_t *state);
unsigned rb_rand_range(uint64_t *state, unsigned limit);
unsigned rb_seed_for(const char *group, unsigned seed, int case_id);

void rb_generate(rb_spec *spec, const char *group, unsigned seed, int case_id);
void rb_refresh(rb_spec *spec);
const char *rb_kind_name(int kind);
const char *rb_direction_name(int direction);

int rb_build_pattern(const rb_spec *spec, int with_callout, char *out, size_t size);
unsigned long long rb_expansion_bound(const rb_spec *spec);
int rb_spec_within_limits(const rb_spec *spec);

void rb_match_reference(const rb_spec *spec, int pos, rb_reference *result);
rb_reference rb_reference_search(const rb_spec *spec);

int rb_write_spec(const rb_spec *spec, const char *path);
int rb_read_spec(rb_spec *spec, const char *path);
int rb_hex_encode(const unsigned char *data, int len, char *out, size_t out_size);
int rb_hex_decode(const char *hex, unsigned char *out, size_t out_size);

#endif
