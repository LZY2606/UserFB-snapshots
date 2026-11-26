#include "harness.h"

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "interpreter.h"
#include "kafel.h"
#include "repeated.h"
#include "value_plan.h"

#define CUSTOM_NR 123
#define ERRNO7 ((uint32_t)(SECCOMP_RET_ERRNO | 7))
#define SAMPLE_COUNT 320

/* ------------------------------------------------------------------ */
/* Small deterministic argument source                                 */
/* ------------------------------------------------------------------ */

static const uint64_t boundary_values[] = {
    0,
    1,
    2,
    3,
    0x7f,
    0x80,
    0xff,
    0x100,
    0x7fffffffU,
    0x80000000U,
    0xfffffffeU,
    0xffffffffU,
    0x100000000ULL,
    0x100000001ULL,
    0x12345678ULL,
    0x9abcdef0ULL,
    0x1000000000000000ULL,
    0x8000000000000000ULL,
    0xfffffffffffffffeULL,
    0xffffffffffffffffULL,
};

static uint64_t splitmix64(uint64_t *state) {
  uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

static void make_args(uint64_t args[6], unsigned sample) {
  const unsigned nb =
      sizeof(boundary_values) / sizeof(boundary_values[0]);

  if (sample < 120) {
    for (int i = 0; i < 6; ++i) {
      args[i] = boundary_values[(sample + i * 7) % nb];
    }
    if ((sample % 5) == 0) {
      args[sample % 6] ^= 1ULL << ((sample / 5) % 64);
    }
  } else {
    uint64_t state = 0x4b6166656c2d7467ULL ^ ((uint64_t)sample * 0xd1b54a32d192ed03ULL);
    for (int i = 0; i < 6; ++i) {
      args[i] = splitmix64(&state);
    }
  }
}

/* Independent ordinary-C model of repeated_policy(depth). */
static uint64_t repeated_reference(unsigned depth, const uint64_t args[6]) {
  static const int pairs[6][2] = {
      {2, 3}, {4, 5}, {0, 2}, {1, 4}, {3, 5}, {0, 4},
  };
  uint64_t value = args[0] & args[1];

  for (unsigned i = 0; i < depth; ++i) {
    uint64_t p = args[pairs[i][0]];
    uint64_t q = args[pairs[i][1]];
    uint64_t left_side;
    uint64_t right_side;

    /* repeated.h uses one op for the inner pairs and the opposite op in
     * the middle: even layers are (&) then |, odd layers are (|) then &. */
    if ((i & 1U) == 0) {
      left_side = value & p;
      right_side = value & q;
      value = left_side | right_side;
    } else {
      left_side = value | p;
      right_side = value | q;
      value = left_side & right_side;
    }
  }
  return value;
}

/* ------------------------------------------------------------------ */
/* Independent random expression reference and policy printer         */
/* ------------------------------------------------------------------ */

enum ref_op { REF_AND, REF_OR };

struct ref_expr {
  int kind; /* 0 var, 1 const, 2 binary */
  int op;
  int var;
  uint64_t value;
  struct ref_expr *left;
  struct ref_expr *right;
};

static struct ref_expr ref_pool[512];
static unsigned ref_pool_used;
static uint64_t ref_rng_state;

static struct ref_expr *ref_new(void) {
  CHECK_CHECK(ref_pool_used < sizeof(ref_pool) / sizeof(ref_pool[0]));
  return &ref_pool[ref_pool_used++];
}

static struct ref_expr *ref_var(int var) {
  struct ref_expr *e = ref_new();
  e->kind = 0;
  e->var = var;
  return e;
}

static struct ref_expr *ref_const(uint64_t value) {
  struct ref_expr *e = ref_new();
  e->kind = 1;
  e->value = value;
  return e;
}

static struct ref_expr *ref_binary(int op, struct ref_expr *l,
                                   struct ref_expr *r) {
  struct ref_expr *e = ref_new();
  e->kind = 2;
  e->op = op;
  e->left = l;
  e->right = r;
  return e;
}

static uint64_t ref_rng(void) { return splitmix64(&ref_rng_state); }

static struct ref_expr *ref_generate(int depth) {
  if (depth <= 0 || ref_rng() % 10 < 4) {
    if (ref_rng() % 3 == 0) {
      return ref_const(boundary_values[ref_rng() %
                        (sizeof(boundary_values) /
                         sizeof(boundary_values[0]))]);
    }
    return ref_var((int)(ref_rng() % 6));
  }

  int op = (ref_rng() & 1U) ? REF_OR : REF_AND;
  return ref_binary(op, ref_generate(depth - 1), ref_generate(depth - 1));
}

static struct ref_expr *ref_clone(const struct ref_expr *e) {
  if (e->kind == 0) {
    return ref_var(e->var);
  }
  if (e->kind == 1) {
    return ref_const(e->value);
  }
  return ref_binary(e->op, ref_clone(e->left), ref_clone(e->right));
}

static void ref_commutize(struct ref_expr *e) {
  if (e->kind != 2) {
    return;
  }
  ref_commutize(e->left);
  ref_commutize(e->right);
  if (ref_rng() & 1U) {
    struct ref_expr *tmp = e->left;
    e->left = e->right;
    e->right = tmp;
  }
}

static uint64_t ref_eval(const struct ref_expr *e, const uint64_t args[6]) {
  if (e->kind == 0) {
    return args[e->var];
  }
  if (e->kind == 1) {
    return e->value;
  }
  uint64_t left = ref_eval(e->left, args);
  uint64_t right = ref_eval(e->right, args);
  return e->op == REF_AND ? (left & right) : (left | right);
}

struct string_buffer {
  char *data;
  size_t len;
  size_t cap;
};

static void sb_init(struct string_buffer *b) {
  b->cap = 128;
  b->len = 0;
  b->data = malloc(b->cap);
  CHECK_CHECK(b->data != NULL);
  b->data[0] = '\0';
}

static void sb_reserve(struct string_buffer *b, size_t extra) {
  if (b->len + extra + 1 <= b->cap) {
    return;
  }
  while (b->len + extra + 1 > b->cap) {
    b->cap *= 2;
  }
  char *next = realloc(b->data, b->cap);
  CHECK_CHECK(next != NULL);
  b->data = next;
}

static void sb_puts(struct string_buffer *b, const char *s) {
  size_t n = strlen(s);
  sb_reserve(b, n);
  memcpy(b->data + b->len, s, n + 1);
  b->len += n;
}

static void sb_putc(struct string_buffer *b, char c) {
  sb_reserve(b, 1);
  b->data[b->len++] = c;
  b->data[b->len] = '\0';
}

static void render_expr(struct string_buffer *b, const struct ref_expr *e) {
  static const char *names[6] = {"a", "b", "c", "d", "e", "f"};
  char number[32];

  if (e->kind == 0) {
    sb_puts(b, names[e->var]);
    return;
  }
  if (e->kind == 1) {
    snprintf(number, sizeof(number), "0x%llx",
             (unsigned long long)e->value);
    sb_puts(b, number);
    return;
  }

  sb_putc(b, '(');
  render_expr(b, e->left);
  sb_putc(b, ' ');
  sb_putc(b, e->op == REF_AND ? '&' : '|');
  sb_putc(b, ' ');
  render_expr(b, e->right);
  sb_putc(b, ')');
}

static bool ref_compare(uint64_t left, int op, uint64_t right) {
  switch (op) {
    case 0:
      return left == right;
    case 1:
      return left != right;
    case 2:
      return left < right;
    case 3:
      return left <= right;
    case 4:
      return left > right;
    case 5:
      return left >= right;
  }
  return false;
}

/* ------------------------------------------------------------------ */
/* BPF storage checks                                                 */
/* ------------------------------------------------------------------ */

static bool is_mem_load(uint16_t code) {
  if (BPF_CLASS(code) == BPF_LD && (code & 0xe0U) == BPF_MEM) {
    return true;
  }
  if (BPF_CLASS(code) == BPF_LDX && (code & 0xe0U) == BPF_MEM) {
    return true;
  }
  return false;
}

static bool is_mem_store(uint16_t code) {
  return BPF_CLASS(code) == BPF_ST || BPF_CLASS(code) == BPF_STX;
}

/*
 * Must-write data-flow over reachable BPF CFG.  At a join only slots written
 * on every predecessor remain in the mask, so a load that is uninitialized on
 * any reachable path fails.
 */
static void storage_check(const struct sock_fprog *prog) {
  CHECK_CHECK(prog != NULL);
  CHECK_CHECK(prog->len > 0);
  if (prog == NULL || prog->len == 0) {
    return;
  }

  for (unsigned i = 0; i < prog->len; ++i) {
    uint16_t code = prog->filter[i].code;
    uint32_t slot = prog->filter[i].k;
    if ((is_mem_load(code) || is_mem_store(code))) {
      CHECK_CHECK(slot < BPF_MEMWORDS);
    }
  }

  size_t n = prog->len;
  uint32_t *written = calloc(n, sizeof(*written));
  bool *reachable = calloc(n, sizeof(*reachable));
  bool *queued = calloc(n, sizeof(*queued));
  size_t *queue = malloc(n * sizeof(*queue));
  CHECK_CHECK(written != NULL && reachable != NULL && queued != NULL &&
              queue != NULL);
  if (written == NULL || reachable == NULL || queued == NULL ||
      queue == NULL) {
    exit(1);
  }

  size_t head = 0;
  size_t tail = 0;
  reachable[0] = true;
  queued[0] = true;
  queue[tail++ % n] = 0;

  while (head < tail) {
    size_t pc = queue[head++ % n];
    queued[pc] = false;

    const struct sock_filter ins = prog->filter[pc];
    uint16_t code = ins.code;
    uint32_t mask = written[pc];

    if (is_mem_load(code)) {
      CHECK_CHECK(ins.k < BPF_MEMWORDS);
      CHECK_CHECK((mask & (1U << ins.k)) != 0);
    }
    if (is_mem_store(code)) {
      CHECK_CHECK(ins.k < BPF_MEMWORDS);
      mask |= 1U << ins.k;
    }

    size_t successors[2];
    unsigned successor_count = 0;

    if (BPF_CLASS(code) == BPF_JMP) {
      if (BPF_OP(code) == BPF_JA) {
        uint32_t target = pc + 1 + ins.k;
        CHECK_CHECK(target < n);
        if (target < n) {
          successors[successor_count++] = target;
        }
      } else {
        uint32_t t = pc + 1 + ins.jt;
        uint32_t f = pc + 1 + ins.jf;
        CHECK_CHECK(t < n && f < n);
        if (t < n) successors[successor_count++] = t;
        if (f < n && f != t) successors[successor_count++] = f;
      }
    } else if (BPF_CLASS(code) != BPF_RET) {
      if (pc + 1 < n) {
        successors[successor_count++] = pc + 1;
      }
    }

    for (unsigned i = 0; i < successor_count; ++i) {
      size_t target = successors[i];
      bool changed = false;
      if (!reachable[target]) {
        reachable[target] = true;
        written[target] = mask;
        changed = true;
      } else {
        uint32_t merged = written[target] & mask;
        changed = merged != written[target];
        written[target] = merged;
      }
      if (changed && !queued[target]) {
        queued[target] = true;
        CHECK_CHECK(tail - head < n);
        queue[tail++ % n] = target;
      }
    }
  }

  free(queue);
  free(queued);
  free(reachable);
  free(written);
}

static unsigned count_code(const struct sock_fprog *prog, uint16_t code) {
  unsigned count = 0;
  for (unsigned i = 0; i < prog->len; ++i) {
    if (prog->filter[i].code == code) {
      ++count;
    }
  }
  return count;
}

static unsigned count_alu_bitops(const struct sock_fprog *prog) {
  unsigned count = 0;
  for (unsigned i = 0; i < prog->len; ++i) {
    uint16_t code = prog->filter[i].code;
    if (BPF_CLASS(code) == BPF_ALU &&
        (BPF_OP(code) == BPF_AND || BPF_OP(code) == BPF_OR)) {
      ++count;
    }
  }
  return count;
}

static unsigned count_store_slot(const struct sock_fprog *prog,
                                 uint32_t slot) {
  unsigned count = 0;
  for (unsigned i = 0; i < prog->len; ++i) {
    if (prog->filter[i].code == BPF_ST && prog->filter[i].k == slot) {
      ++count;
    }
  }
  return count;
}

/* Planned code parks its A result in slot 14; the full A/X form also parks
 * its right operand in slot 15.  The planner never emits TAX. */
static bool uses_value_plan(const struct sock_fprog *prog) {
  return count_store_slot(prog, 14) > 0 &&
         count_code(prog, BPF_MISC | BPF_TAX) == 0;
}

static bool uses_full_two_side_plan(const struct sock_fprog *prog) {
  return uses_value_plan(prog) && count_store_slot(prog, 15) > 0;
}

/* ------------------------------------------------------------------ */
/* Compile/interpret helpers                                          */
/* ------------------------------------------------------------------ */

static struct sock_fprog compile_arch(const char *source, uint32_t arch_bit) {
  kafel_ctxt_t context = kafel_ctxt_create();
  struct sock_fprog prog = {0};

  kafel_set_target_archs(context, arch_bit);
  kafel_set_input_string(context, source);
  int rc = kafel_compile(context, &prog);
  if (rc != 0) {
    fprintf(stderr, "target policy compile failed: %s\n",
            kafel_error_msg(context));
  }
  CHECK_CHECK(rc == 0);
  if (rc != 0) {
    exit(1);
  }
  kafel_ctxt_destroy(&context);
  storage_check(&prog);
  return prog;
}

static struct sock_fprog compile_x64(const char *source) {
  return compile_arch(source, KAFEL_TARGET_ARCH_X86_64);
}

static bool run_prog(const struct sock_fprog *prog, uint32_t audit_arch,
                     uint32_t nr, const uint64_t args[6],
                     uint32_t *result) {
  interpreter_ctxt_t interpreter;
  struct seccomp_data data;
  memset(&interpreter, 0, sizeof(interpreter));
  memset(&data, 0, sizeof(data));

  data.arch = audit_arch;
  data.nr = nr;
  for (int i = 0; i < 6; ++i) {
    data.args[i] = args[i];
  }

  bool ok = interpreter_run(&interpreter, prog, &data);
  if (!ok) {
    fprintf(stderr, "interpreter failed: %s\n", interpreter.error_buf);
  }
  *result = interpreter.result;
  return ok;
}

static void expect_action_nr(const struct sock_fprog *prog,
                             uint32_t audit_arch, uint32_t nr,
                             const uint64_t args[6], uint32_t expected) {
  uint32_t actual = 0;
  bool ok = run_prog(prog, audit_arch, nr, args, &actual);
  CHECK_CHECK(ok);
  if (actual != expected) {
    fprintf(stderr, "action mismatch nr=%u arch=%x actual=%x expected=%x args=%llx,%llx,%llx,%llx,%llx,%llx\n",
            nr, audit_arch, actual, expected, (unsigned long long)args[0],
            (unsigned long long)args[1], (unsigned long long)args[2],
            (unsigned long long)args[3], (unsigned long long)args[4],
            (unsigned long long)args[5]);
  }
  CHECK_CHECK(actual == expected);
}

static void expect_action(const struct sock_fprog *prog,
                          const uint64_t args[6], uint32_t expected) {
  expect_action_nr(prog, AUDIT_ARCH_X86_64, CUSTOM_NR, args, expected);
}

static void expect_wrong_arch_kill(const struct sock_fprog *prog) {
  uint64_t args[6] = {0};
  expect_action_nr(prog, 0, CUSTOM_NR, args, SECCOMP_RET_KILL);
}

/* ------------------------------------------------------------------ */
/* sharing group                                                      */
/* ------------------------------------------------------------------ */

static void check_repeated_fixture(void) {
  for (unsigned depth = 1; depth <= 6; ++depth) {
    char *source = repeated_policy(depth);
    CHECK_CHECK(source != NULL);
    struct sock_fprog prog = compile_x64(source);

    if (depth == 6) {
      CHECK_CHECK(prog.len <= 400);
    }
    CHECK_CHECK(uses_value_plan(&prog));

    for (unsigned sample = 0; sample < SAMPLE_COUNT; ++sample) {
      uint64_t args[6];
      make_args(args, sample);
      bool allow =
          repeated_reference(depth, args) == 0x100000001ULL;
      expect_action(&prog, args, allow ? SECCOMP_RET_ALLOW : ERRNO7);
    }

    free(prog.filter);
    free(source);
  }

  char *source = repeated_policy(6);
  struct sock_fprog prog = compile_x64(source);
  CHECK_CHECK(prog.len <= 400);

  uint64_t allow_args[6] = {0, 0, 0x100000001ULL, 0,
                           0x100000001ULL, 0x100000001ULL};
  uint64_t high_word_args[6] = {0, 0, 0x1ULL, 0, 0x1ULL, 0x1ULL};
  uint64_t low_word_args[6] = {0, 0, 0x100000000ULL, 0,
                               0x100000000ULL, 0x100000000ULL};
  uint64_t deny_args[6] = {0};

  CHECK_CHECK(repeated_reference(6, allow_args) == 0x100000001ULL);
  CHECK_CHECK(repeated_reference(6, high_word_args) == 0x1ULL);
  CHECK_CHECK(repeated_reference(6, low_word_args) == 0x100000000ULL);
  CHECK_CHECK(repeated_reference(6, deny_args) == 0);

  expect_action(&prog, allow_args, SECCOMP_RET_ALLOW);
  expect_action(&prog, high_word_args, ERRNO7);
  expect_action(&prog, low_word_args, ERRNO7);
  expect_action(&prog, deny_args, ERRNO7);
  expect_wrong_arch_kill(&prog);

  free(prog.filter);
  free(source);
}

static void check_commutative_sharing(void) {
  static const char *sources[] = {
      "ALLOW { SYSCALL[123](a,b,c,d,e,f) { "
      "((a & b) | (c & d)) == ((d & c) | (b & a)) } } "
      "DEFAULT ERRNO(7)",
      "ALLOW { SYSCALL[123](a,b,c,d,e,f) { "
      "((a & b) & c) == (c & (b & a)) } } DEFAULT ERRNO(7)",
      "ALLOW { SYSCALL[123](a,b,c,d,e,f) { "
      "((a & b) | c) == (c | (a & b)) } } DEFAULT ERRNO(7)",
  };

  for (unsigned i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i) {
    struct sock_fprog prog = compile_x64(sources[i]);
    CHECK_CHECK(uses_full_two_side_plan(&prog));
    CHECK_CHECK(count_alu_bitops(&prog) <= 8);

    for (unsigned sample = 0; sample < 96; ++sample) {
      uint64_t args[6];
      make_args(args, sample);
      expect_action(&prog, args, SECCOMP_RET_ALLOW);
    }
    free(prog.filter);
  }
}

static void check_random_equivalence(void) {
  static const char *ops[6] = {"==", "!=", "<", "<=", ">", ">="};

  for (unsigned test = 0; test < 42; ++test) {
    ref_pool_used = 0;
    ref_rng_state = 0x53686172696e6700ULL + test * 0x9e3779b97f4a7c15ULL;

    struct ref_expr *left = ref_generate(4);
    struct ref_expr *right;
    if (test % 3 == 0) {
      right = ref_clone(left);
      ref_commutize(right);
    } else {
      right = ref_generate(4);
    }

    int comparison = (int)(test % 6);
    struct string_buffer left_text;
    struct string_buffer right_text;
    sb_init(&left_text);
    sb_init(&right_text);
    render_expr(&left_text, left);
    render_expr(&right_text, right);

    char source[32768];
    int n = snprintf(
        source, sizeof(source),
        "ALLOW { SYSCALL[123](a,b,c,d,e,f) { %s %s %s } } DEFAULT ERRNO(7)",
        left_text.data, ops[comparison], right_text.data);
    CHECK_CHECK(n > 0 && (size_t)n < sizeof(source));

    struct sock_fprog prog = compile_x64(source);

    for (unsigned sample = 0; sample < 96; ++sample) {
      uint64_t args[6];
      make_args(args, sample + test * 97);
      bool allow = ref_compare(ref_eval(left, args), comparison,
                               ref_eval(right, args));
      expect_action(&prog, args, allow ? SECCOMP_RET_ALLOW : ERRNO7);
    }

    free(prog.filter);
    free(left_text.data);
    free(right_text.data);
  }
}

static void check_sharing_group(void) {
  check_repeated_fixture();
  check_commutative_sharing();
  check_random_equivalence();
}

/* ------------------------------------------------------------------ */
/* words group                                                        */
/* ------------------------------------------------------------------ */

static uint64_t lhs_ab_and(const uint64_t a[6]) { return a[0] & a[1]; }
static uint64_t lhs_abc_or_and(const uint64_t a[6]) {
  return (a[0] & a[1]) | a[2];
}
static uint64_t lhs_bc_and_or(const uint64_t a[6]) {
  return a[0] | (a[1] & a[2]);
}

struct word_material {
  const char *expression;
  int op;
  uint64_t (*left)(const uint64_t *);
  uint64_t right;
};

static void check_word_identities(void) {
  struct word_material materials[] = {
      {"(a & b) == 0x100000001", 0, lhs_ab_and, 0x100000001ULL},
      {"(a & b) != 0x100000001", 1, lhs_ab_and, 0x100000001ULL},
      {"((a & b) | c) < 0xf0000000f0000000", 2, lhs_abc_or_and,
       0xf0000000f0000000ULL},
      {"((a & b) | c) <= 0xf0000000f0000000", 3, lhs_abc_or_and,
       0xf0000000f0000000ULL},
      {"(a | (b & c)) > 0x1000", 4, lhs_bc_and_or, 0x1000},
      {"(a | (b & c)) >= 0x1000", 5, lhs_bc_and_or, 0x1000},
  };

  for (unsigned i = 0; i < sizeof(materials) / sizeof(materials[0]); ++i) {
    char source[1024];
    snprintf(source, sizeof(source),
             "ALLOW { SYSCALL[123](a,b,c,d,e,f) { %s } } DEFAULT ERRNO(7)",
             materials[i].expression);
    struct sock_fprog prog = compile_x64(source);

    for (unsigned sample = 0; sample < SAMPLE_COUNT; ++sample) {
      uint64_t args[6];
      make_args(args, sample + i * 101);
      bool allow = ref_compare(materials[i].left(args), materials[i].op,
                               materials[i].right);
      expect_action(&prog, args, allow ? SECCOMP_RET_ALLOW : ERRNO7);
    }
    free(prog.filter);
  }
}

static void check_high_low_offsets(void) {
  const char *source =
      "ALLOW { SYSCALL[123](a) { a == 0x100000001 } } DEFAULT ERRNO(7)";
  struct sock_fprog prog = compile_x64(source);

  uint32_t low_offset =
      (uint32_t)offsetof(struct seccomp_data, args[0]);
  uint32_t high_offset = low_offset + 4;
  bool saw_low = false;
  bool saw_high = false;

  for (unsigned i = 0; i < prog.len; ++i) {
    if (prog.filter[i].code == (BPF_LD | BPF_W | BPF_ABS)) {
      if (prog.filter[i].k == low_offset) saw_low = true;
      if (prog.filter[i].k == high_offset) saw_high = true;
    }
  }
  CHECK_CHECK(saw_low && saw_high);

  uint64_t exact[6] = {0x100000001ULL};
  uint64_t high_different[6] = {1};
  uint64_t low_different[6] = {0x100000000ULL};
  uint64_t both_different[6] = {0x200000002ULL};

  expect_action(&prog, exact, SECCOMP_RET_ALLOW);
  expect_action(&prog, high_different, ERRNO7);
  expect_action(&prog, low_different, ERRNO7);
  expect_action(&prog, both_different, ERRNO7);

  free(prog.filter);
}

static void check_arg_indices(void) {
  const char *source =
      "ALLOW { SYSCALL[123](a,b,c,d) { (a & b) == (c | d) } } "
      "DEFAULT ERRNO(7)";
  struct sock_fprog prog = compile_x64(source);

  for (unsigned sample = 0; sample < SAMPLE_COUNT; ++sample) {
    uint64_t args[6];
    make_args(args, sample + 700);
    bool allow = (args[0] & args[1]) == (args[2] | args[3]);
    expect_action(&prog, args, allow ? SECCOMP_RET_ALLOW : ERRNO7);
  }
  free(prog.filter);
}

static void check_32bit_source(void) {
  const char *source =
      "ALLOW { openat { (dfd & flags) == 0x1234 } } DEFAULT ERRNO(7)";
  struct sock_fprog prog = compile_x64(source);

  for (unsigned sample = 0; sample < SAMPLE_COUNT; ++sample) {
    uint64_t raw[6];
    make_args(raw, sample + 900);
    /* Deliberately set high bits; a 32-bit source must ignore them. */
    uint32_t dfd = (uint32_t)raw[0];
    uint32_t flags = (uint32_t)raw[2];
    uint64_t args[6] = {
        (uint64_t)dfd | 0x900000000ULL,
        raw[1],
        (uint64_t)flags | 0xa00000000ULL,
        raw[3],
        raw[4],
        raw[5],
    };
    bool allow = ((uint32_t)args[0] & (uint32_t)args[2]) == 0x1234U;
    expect_action_nr(&prog, AUDIT_ARCH_X86_64, 257, args,
                     allow ? SECCOMP_RET_ALLOW : ERRNO7);
  }
  free(prog.filter);
}

static void check_identity_planner_units(void) {
  struct expr_tree v32 = {0};
  struct expr_tree v64 = {0};
  v32.type = EXPR_VAR;
  v32.var = 3;
  v32.size = 4;
  v64.type = EXPR_VAR;
  v64.var = 3;
  v64.size = 8;

  struct value_plan *plan = value_plan_build(&v32, &v64, VALUE_PLAN_WORD_LOW);
  CHECK_CHECK(plan != NULL);
  CHECK_CHECK(plan->feasible);
  CHECK_CHECK(plan->node_len == 2);
  value_plan_destroy(plan);

  /*
   * Fifteen distinct terms are used once in each comparison side.  After the
   * right side has been materialized, all fifteen terms must remain live for
   * the left side.  Only fourteen work slots exist after reserving the two
   * comparison-result slots, so this plan must report capacity pressure.
   */
  enum { PRESSURE_TERMS = 15 };
  struct expr_tree variable = {0};
  struct expr_tree constants[PRESSURE_TERMS];
  struct expr_tree terms[PRESSURE_TERMS];
  struct expr_tree right_chain[PRESSURE_TERMS];
  struct expr_tree left_chain[PRESSURE_TERMS];

  variable.type = EXPR_VAR;
  variable.var = 0;
  variable.size = 8;

  struct expr_tree *right_root = NULL;
  struct expr_tree *left_root = NULL;
  for (int i = 0; i < PRESSURE_TERMS; ++i) {
    memset(&constants[i], 0, sizeof(constants[i]));
    memset(&terms[i], 0, sizeof(terms[i]));
    memset(&right_chain[i], 0, sizeof(right_chain[i]));
    memset(&left_chain[i], 0, sizeof(left_chain[i]));

    constants[i].type = EXPR_NUMBER;
    constants[i].number = (uint64_t)(i + 1) * 0x1111ULL;
    terms[i].type = EXPR_BIT_AND;
    terms[i].left = &variable;
    terms[i].right = &constants[i];

    if (i == 0) {
      right_root = &terms[i];
      left_root = &terms[i];
    } else {
      right_chain[i].type = EXPR_BIT_OR;
      right_chain[i].left = right_root;
      right_chain[i].right = &terms[i];
      right_root = &right_chain[i];

      left_chain[i].type = EXPR_BIT_AND;
      left_chain[i].left = left_root;
      left_chain[i].right = &terms[i];
      left_root = &left_chain[i];
    }
  }

  struct value_plan *pressure =
      value_plan_build(right_root, left_root, VALUE_PLAN_WORD_LOW);
  CHECK_CHECK(pressure != NULL);
  CHECK_CHECK(!pressure->feasible);
  value_plan_destroy(pressure);
}

static void check_words_group(void) {
  check_word_identities();
  check_high_low_offsets();
  check_arg_indices();
  check_32bit_source();
  check_identity_planner_units();
}

/* ------------------------------------------------------------------ */
/* branches group                                                     */
/* ------------------------------------------------------------------ */

static bool branch_predicate_1(const uint64_t a[6]) {
  return (((a[0] & 1ULL) == 1ULL) && ((a[1] & 2ULL) == 2ULL)) ||
         (a[2] == 7);
}

static bool branch_predicate_2(const uint64_t a[6]) {
  return !((a[0] & 3ULL) == 3ULL) && (a[1] < a[2]);
}

static bool branch_predicate_3(const uint64_t a[6]) {
  return ((a[0] == a[1]) || (a[2] > a[3])) && (a[4] <= a[5]);
}

static void check_boolean_branches(void) {
  struct {
    const char *source;
    bool (*predicate)(const uint64_t *);
  } cases[] = {
      {"ALLOW { SYSCALL[123](a,b,c) { "
       "((a & 1) == 1 && (b & 2) == 2) || (c == 7) } } "
       "DEFAULT ERRNO(7)",
       branch_predicate_1},
      {"ALLOW { SYSCALL[123](a,b,c) { "
       "!((a & 3) == 3) && (b < c) } } DEFAULT ERRNO(7)",
       branch_predicate_2},
      {"ALLOW { SYSCALL[123](a,b,c,d,e,f) { "
       "((a == b) || (c > d)) && (e <= f) } } DEFAULT ERRNO(7)",
       branch_predicate_3},
  };

  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    struct sock_fprog prog = compile_x64(cases[i].source);
    for (unsigned sample = 0; sample < SAMPLE_COUNT; ++sample) {
      uint64_t args[6];
      make_args(args, sample + i * 313);
      uint32_t expected =
          cases[i].predicate(args) ? SECCOMP_RET_ALLOW : ERRNO7;
      expect_action(&prog, args, expected);
    }
    expect_wrong_arch_kill(&prog);
    free(prog.filter);
  }
}

static void check_first_rule_actions(void) {
  const char *source =
      "ALLOW { SYSCALL[123](a) { a == 1 } } "
      "ERRNO(9) { SYSCALL[123](a) { a == 2 } } "
      "DEFAULT ERRNO(7)";
  struct sock_fprog prog = compile_x64(source);

  for (unsigned sample = 0; sample < SAMPLE_COUNT; ++sample) {
    uint64_t args[6];
    make_args(args, sample + 1100);
    uint32_t expected;
    if (args[0] == 1) {
      expected = SECCOMP_RET_ALLOW;
    } else if (args[0] == 2) {
      expected = SECCOMP_RET_ERRNO | 9;
    } else {
      expected = ERRNO7;
    }
    expect_action(&prog, args, expected);
  }

  uint64_t one[6] = {1};
  uint64_t two[6] = {2};
  expect_action(&prog, one, SECCOMP_RET_ALLOW);
  expect_action(&prog, two, SECCOMP_RET_ERRNO | 9);
  expect_wrong_arch_kill(&prog);

  free(prog.filter);
}

static void check_branches_group(void) {
  check_boolean_branches();
  check_first_rule_actions();
}

/* ------------------------------------------------------------------ */
/* storage group                                                      */
/* ------------------------------------------------------------------ */

static struct sock_fprog compile_pressure_policy(uint64_t keys[15]) {
  char expression[16384];
  char next_expression[16384];
  char source[20000];

  snprintf(expression, sizeof(expression), "a");
  for (int i = 0; i < 15; ++i) {
    uint32_t k1 = (uint32_t)(0x1000U + (unsigned)i * 0x137U);
    uint32_t k2 = (uint32_t)(0x8000U + (unsigned)i * 0x211U);
    keys[i] = ((uint64_t)k1 << 32) | k2;
    snprintf(next_expression, sizeof(next_expression),
             "(%s & ((b | 0x%x) & (c | 0x%x)))", expression, k1, k2);
    CHECK_CHECK(strlen(next_expression) < sizeof(expression));
    strcpy(expression, next_expression);
  }

  snprintf(source, sizeof(source),
           "ALLOW { SYSCALL[123](a,b,c) { %s == 0x12345678 } } "
           "DEFAULT ERRNO(7)",
           expression);
  return compile_x64(source);
}

static void check_storage_group(void) {
  CHECK_CHECK(BPF_MEMWORDS == 16);
  CHECK_CHECK(VALUE_PLAN_MEMWORDS == BPF_MEMWORDS);
  CHECK_CHECK(VALUE_PLAN_WORK_SLOTS + VALUE_PLAN_RESULT_SLOTS ==
              BPF_MEMWORDS);

  char *repeated_source = repeated_policy(6);
  struct sock_fprog repeated_prog = compile_x64(repeated_source);
  CHECK_CHECK(repeated_prog.len <= 400);
  CHECK_CHECK(count_store_slot(&repeated_prog, 14) > 0);
  storage_check(&repeated_prog);
  free(repeated_prog.filter);
  free(repeated_source);

  uint64_t keys[15] = {0};
  struct sock_fprog pressure = compile_pressure_policy(keys);
  bool saw_memory = false;
  for (unsigned i = 0; i < pressure.len; ++i) {
    if (is_mem_load(pressure.filter[i].code) ||
        is_mem_store(pressure.filter[i].code)) {
      saw_memory = true;
    }
  }
  CHECK_CHECK(saw_memory);
  storage_check(&pressure);

  for (unsigned sample = 0; sample < SAMPLE_COUNT; ++sample) {
    uint64_t args[6];
    make_args(args, sample + 1500);
    uint64_t value = args[0];
    for (int i = 0; i < 15; ++i) {
      uint32_t k1 = (uint32_t)(0x1000U + (unsigned)i * 0x137U);
      uint32_t k2 = (uint32_t)(0x8000U + (unsigned)i * 0x211U);
      uint64_t right = (args[1] | k1) & (args[2] | k2);
      value &= right;
    }
    bool allow = (uint32_t)value == 0x12345678U;
    expect_action(&pressure, args, allow ? SECCOMP_RET_ALLOW : ERRNO7);
  }
  free(pressure.filter);
}

/* ------------------------------------------------------------------ */
/* reuse group                                                        */
/* ------------------------------------------------------------------ */

static void check_reuse_group(void) {
  kafel_ctxt_t context = kafel_ctxt_create();
  CHECK_CHECK(context != NULL);

  static const char *sources[3] = {
      "ALLOW { SYSCALL[123](a,b,c,d,e,f) { (a & b) == 0x11 } } "
      "DEFAULT ERRNO(7)",
      "ALLOW { SYSCALL[123](a,b,c,d,e,f) { (c | d) == 0x22 } } "
      "DEFAULT ERRNO(7)",
      "ALLOW { SYSCALL[123](a,b,c,d,e,f) { (e & f) == 0x33 } } "
      "DEFAULT ERRNO(7)",
  };
  uint32_t bits[3] = {
      KAFEL_TARGET_ARCH_X86_64,
      KAFEL_TARGET_ARCH_X86,
      KAFEL_TARGET_ARCH_AARCH64,
  };
  uint32_t audits[3] = {
      AUDIT_ARCH_X86_64,
      AUDIT_ARCH_I386,
      AUDIT_ARCH_AARCH64,
  };

  for (unsigned iteration = 0; iteration < 12; ++iteration) {
    unsigned kind = iteration % 3;
    kafel_set_target_archs(context, bits[kind]);
    kafel_set_input_string(context, sources[kind]);

    struct sock_fprog prog = {0};
    int rc = kafel_compile(context, &prog);
    CHECK_CHECK(rc == 0);
    if (rc != 0) {
      fprintf(stderr, "reuse compile failed: %s\n", kafel_error_msg(context));
      exit(1);
    }
    storage_check(&prog);

    uint64_t allow[6] = {0};
    uint64_t deny[6] = {0};
    if (kind == 0) {
      allow[0] = 0x11;
      allow[1] = 0x1f;
      deny[0] = 0x10;
      deny[1] = 0x01;
    } else if (kind == 1) {
      allow[2] = 0x20;
      allow[3] = 0x02;
      deny[2] = 0x20;
      deny[3] = 0x00;
    } else {
      allow[4] = 0x33;
      allow[5] = 0xff;
      deny[4] = 0x30;
      deny[5] = 0x0c;
    }

    expect_action_nr(&prog, audits[kind], CUSTOM_NR, allow,
                     SECCOMP_RET_ALLOW);
    expect_action_nr(&prog, audits[kind], CUSTOM_NR, deny, ERRNO7);

    /* Re-run with a fresh interpreter but the same compiled filter. */
    expect_action_nr(&prog, audits[kind], CUSTOM_NR, allow,
                     SECCOMP_RET_ALLOW);
    free(prog.filter);
  }

  kafel_ctxt_destroy(&context);
}

void check_case(const char *group) {
  CHECK_CHECK(group != NULL);
  if (strcmp(group, "sharing") == 0) {
    check_sharing_group();
  } else if (strcmp(group, "words") == 0) {
    check_words_group();
  } else if (strcmp(group, "branches") == 0) {
    check_branches_group();
  } else if (strcmp(group, "storage") == 0) {
    check_storage_group();
  } else if (strcmp(group, "reuse") == 0) {
    check_reuse_group();
  } else {
    CHECK_CHECK(0);
  }
}
