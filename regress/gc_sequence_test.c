/*
 * Deterministic driver for the GC state-sequence entry point.
 *
 * The same bytes are replayed twice in fresh mJS instances and the structured
 * observations (status, executed prefix, deviation location, path and
 * canonical digest) must match.  Fixed named sequences are built with the
 * independent model; seeded generated sequences use exactly the same model
 * to choose only legal handles and operations.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gc_model.h"
#include "gc_sequence.h"

#define SEQ_BUF_SIZE 262144
#define GEN_SEQUENCE_COUNT 16
#define GEN_OPS 192

struct seq_builder {
  struct gc_model model;
  unsigned char buf[SEQ_BUF_SIZE];
  size_t len;
};

static const unsigned char format_magic[4] = {'M', 'J', 'S', 'G'};

static void sb_init(struct seq_builder *b) {
  gc_model_init(&b->model);
  b->len = 0;
  memcpy(b->buf, format_magic, 4);
  b->buf[4] = GC_SEQUENCE_VERSION;
  b->len = 5;
}

static void sb_u16(struct seq_builder *b, uint16_t v) {
  b->buf[b->len++] = (unsigned char) (v & 0xff);
  b->buf[b->len++] = (unsigned char) ((v >> 8) & 0xff);
}

static size_t encode_value(const struct gc_value_spec *v, unsigned char *p,
                           size_t cap) {
  size_t n = 0;
  if (cap < 1) return 0;
  switch (v->kind) {
    case GC_VALUE_NODE:
      if (cap < 4) return 0;
      p[0] = v->kind;
      p[1] = v->node.slot;
      p[2] = (unsigned char) (v->node.gen & 0xff);
      p[3] = (unsigned char) ((v->node.gen >> 8) & 0xff);
      n = 4;
      break;
    case GC_VALUE_UNDEFINED:
    case GC_VALUE_NULL:
      p[0] = v->kind;
      n = 1;
      break;
    case GC_VALUE_BOOLEAN:
      if (cap < 2) return 0;
      p[0] = v->kind;
      p[1] = (unsigned char) (v->boolean ? 1 : 0);
      n = 2;
      break;
    case GC_VALUE_NUMBER:
      if (cap < 5) return 0;
      p[0] = v->kind;
      p[1] = (unsigned char) (v->number & 0xff);
      p[2] = (unsigned char) ((v->number >> 8) & 0xff);
      p[3] = (unsigned char) ((v->number >> 16) & 0xff);
      p[4] = (unsigned char) ((v->number >> 24) & 0xff);
      n = 5;
      break;
    default:
      return 0;
  }
  return n;
}

static size_t encode_payload(const struct gc_op *op, unsigned char *p,
                             size_t cap) {
  size_t n = 0;
  switch (op->opcode) {
    case GC_OP_NEW_OBJECT:
    case GC_OP_NEW_ARRAY:
      if (cap < 1) return 0;
      p[0] = op->dst;
      return 1;
    case GC_OP_NEW_STRING:
      if (cap < 3 || op->str_len > GC_MODEL_MAX_STRING) return 0;
      p[0] = op->dst;
      p[1] = (unsigned char) (op->str_len & 0xff);
      p[2] = (unsigned char) ((op->str_len >> 8) & 0xff);
      if (op->str_len != 0) memcpy(p + 3, op->str, op->str_len);
      return 3 + op->str_len;
    case GC_OP_SET_PROP:
    case GC_OP_ARRAY_SET:
      if (cap < 4) return 0;
      p[0] = op->handle.slot;
      p[1] = (unsigned char) (op->handle.gen & 0xff);
      p[2] = (unsigned char) ((op->handle.gen >> 8) & 0xff);
      p[3] = op->dst;
      n = encode_value(&op->value, p + 4, cap - 4);
      if (n == 0) return 0;
      return 4 + n;
    case GC_OP_DEL_PROP:
    case GC_OP_ARRAY_DEL:
    case GC_OP_COPY_ROOT:
      if (cap < 4) return 0;
      p[0] = op->handle.slot;
      p[1] = (unsigned char) (op->handle.gen & 0xff);
      p[2] = (unsigned char) ((op->handle.gen >> 8) & 0xff);
      p[3] = op->dst;
      return 4;
    case GC_OP_RELEASE_ROOT:
      if (cap < 3) return 0;
      p[0] = op->handle.slot;
      p[1] = (unsigned char) (op->handle.gen & 0xff);
      p[2] = (unsigned char) ((op->handle.gen >> 8) & 0xff);
      return 3;
    case GC_OP_GC_FULL:
    case GC_OP_GC_MINOR:
      return 0;
    default:
      return 0;
  }
}

static int sb_emit(struct seq_builder *b, const struct gc_op *op) {
  unsigned char payload[3 + GC_MODEL_MAX_STRING + 8];
  size_t plen;
  const char *why = NULL;

  if (b->len + 3 + sizeof(payload) > sizeof(b->buf)) return 0;
  if (!gc_model_validate(&b->model, op, &why)) return 0;

  plen = encode_payload(op, payload, sizeof(payload));
  if (op->opcode == GC_OP_NEW_STRING && plen != 3 + op->str_len) return 0;

  b->buf[b->len++] = op->opcode;
  sb_u16(b, (uint16_t) plen);
  if (plen != 0) memcpy(b->buf + b->len, payload, plen);
  b->len += plen;

  gc_model_apply(&b->model, op);
  return 1;
}

static int sb_append_raw(struct seq_builder *b, uint8_t opcode,
                         const unsigned char *payload, size_t plen) {
  if (b->len + 3 + plen > sizeof(b->buf)) return 0;
  b->buf[b->len++] = opcode;
  sb_u16(b, (uint16_t) plen);
  if (plen != 0) memcpy(b->buf + b->len, payload, plen);
  b->len += plen;
  return 1;
}

static int find_free_node(const struct gc_model *m) {
  int i;
  for (i = 0; i < GC_MODEL_MAX_NODES; i++) {
    if (!m->nodes[i].used) return i;
  }
  return -1;
}

static int find_free_root(const struct gc_model *m) {
  int i;
  for (i = 0; i < GC_MODEL_MAX_ROOTS; i++) {
    if (!m->roots[i].used) return i;
  }
  return -1;
}

static struct gc_handle node_handle_at(const struct gc_model *m, int slot) {
  struct gc_handle h;
  h.slot = (uint8_t) slot;
  h.gen = m->nodes[slot].gen;
  return h;
}

static struct gc_handle root_handle_at(const struct gc_model *m, int slot) {
  struct gc_handle h;
  h.slot = (uint8_t) slot;
  h.gen = m->roots[slot].gen;
  return h;
}

static struct gc_value_spec v_node(struct gc_handle h) {
  struct gc_value_spec v;
  memset(&v, 0, sizeof(v));
  v.kind = GC_VALUE_NODE;
  v.node = h;
  return v;
}

static struct gc_value_spec v_undefined(void) {
  struct gc_value_spec v;
  memset(&v, 0, sizeof(v));
  v.kind = GC_VALUE_UNDEFINED;
  return v;
}

static struct gc_value_spec v_null(void) {
  struct gc_value_spec v;
  memset(&v, 0, sizeof(v));
  v.kind = GC_VALUE_NULL;
  return v;
}

static struct gc_value_spec v_bool(int b) {
  struct gc_value_spec v;
  memset(&v, 0, sizeof(v));
  v.kind = GC_VALUE_BOOLEAN;
  v.boolean = b ? 1 : 0;
  return v;
}

static struct gc_value_spec v_number(uint32_t n) {
  struct gc_value_spec v;
  memset(&v, 0, sizeof(v));
  v.kind = GC_VALUE_NUMBER;
  v.number = n;
  return v;
}

static struct gc_op op_new(uint8_t opcode, uint8_t root) {
  struct gc_op op;
  memset(&op, 0, sizeof(op));
  op.opcode = opcode;
  op.dst = root;
  return op;
}

static struct gc_op op_new_string(uint8_t root, const unsigned char *s,
                                  size_t len) {
  struct gc_op op = op_new(GC_OP_NEW_STRING, root);
  op.str = s;
  op.str_len = len;
  return op;
}

static struct gc_op op_set(uint8_t opcode, struct gc_handle h, uint8_t key,
                           struct gc_value_spec v) {
  struct gc_op op;
  memset(&op, 0, sizeof(op));
  op.opcode = opcode;
  op.handle = h;
  op.dst = key;
  op.value = v;
  return op;
}

static struct gc_op op_del(uint8_t opcode, struct gc_handle h, uint8_t key) {
  struct gc_op op;
  memset(&op, 0, sizeof(op));
  op.opcode = opcode;
  op.handle = h;
  op.dst = key;
  return op;
}

static struct gc_op op_root(uint8_t opcode, struct gc_handle src,
                            uint8_t dst) {
  struct gc_op op;
  memset(&op, 0, sizeof(op));
  op.opcode = opcode;
  op.handle = src;
  op.dst = dst;
  return op;
}

static struct gc_op op_gc(int full) {
  struct gc_op op;
  memset(&op, 0, sizeof(op));
  op.opcode = full ? GC_OP_GC_FULL : GC_OP_GC_MINOR;
  return op;
}

static int sb_new_object(struct seq_builder *b, uint8_t root, int *slot_out) {
  struct gc_op op = op_new(GC_OP_NEW_OBJECT, root);
  int slot = find_free_node(&b->model);
  if (!sb_emit(b, &op)) return 0;
  if (slot_out != NULL) *slot_out = slot;
  return 1;
}

static int sb_new_array(struct seq_builder *b, uint8_t root, int *slot_out) {
  struct gc_op op = op_new(GC_OP_NEW_ARRAY, root);
  int slot = find_free_node(&b->model);
  if (!sb_emit(b, &op)) return 0;
  if (slot_out != NULL) *slot_out = slot;
  return 1;
}

static int sb_new_string(struct seq_builder *b, uint8_t root,
                         const unsigned char *s, size_t len, int *slot_out) {
  struct gc_op op = op_new_string(root, s, len);
  int slot = find_free_node(&b->model);
  if (!sb_emit(b, &op)) return 0;
  if (slot_out != NULL) *slot_out = slot;
  return 1;
}

static int same_result(const struct gc_seq_result *a,
                       const struct gc_seq_result *b) {
  return a->status == b->status && a->invalid_prefix == b->invalid_prefix &&
         a->records_seen == b->records_seen &&
         a->ops_executed == b->ops_executed &&
         a->bad_record == b->bad_record && a->bad_op == b->bad_op &&
         a->root_slot == b->root_slot && a->root_gen == b->root_gen &&
         a->digest == b->digest && strcmp(a->path, b->path) == 0 &&
         strcmp(a->detail, b->detail) == 0;
}

static void print_hex(const unsigned char *data, size_t len) {
  size_t i;
  for (i = 0; i < len; i++) printf("%02x", (unsigned int) data[i]);
}

static int replay_deterministic(const char *name, const unsigned char *data,
                                size_t len, int expect_valid, int verbose) {
  struct gc_seq_result r1, r2;
  int st1, st2;

  /* Exercise the actual libFuzzer entry point first: on a content/identity
     deviation it prints the diagnostic and aborts. */
  (void) LLVMFuzzerTestOneInput(data, len);

  st1 = gc_sequence_run(data, len, &r1);
  st2 = gc_sequence_run(data, len, &r2);

  if (st1 != GC_SEQ_STATUS_OK || st2 != GC_SEQ_STATUS_OK) {
    printf("MJS_GC_SEQUENCE_MISMATCH %s status=%d/%d op=%zu root=%d "
           "rootgen=%u path=%s detail=%s input=",
           name, st1, st2, r1.bad_op, r1.root_slot, r1.root_gen, r1.path,
           r1.detail);
    print_hex(data, len);
    printf("\n");
    return 0;
  }
  if (!same_result(&r1, &r2)) {
    printf("MJS_GC_SEQUENCE_DETERMINISM_FAIL %s\n", name);
    return 0;
  }
  if (expect_valid && r1.invalid_prefix) {
    printf("FAIL %s expected a fully valid sequence, stopped at record %zu (%s)\n",
           name, r1.bad_record, r1.detail);
    return 0;
  }
  if (!expect_valid && !r1.invalid_prefix) {
    printf("FAIL %s expected controlled invalid-prefix result\n", name);
    return 0;
  }

  if (verbose) {
    printf("CASE %s OK ops=%zu invalid=%d digest=%llx\n", name,
           r1.ops_executed, r1.invalid_prefix,
           (unsigned long long) r1.digest);
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* Eight named fixed valid sequences                                   */
/* ------------------------------------------------------------------ */

static int case_shared_child(void) {
  struct seq_builder b;
  int a, c, d;
  sb_init(&b);
  if (!sb_new_object(&b, 0, &a) || !sb_new_object(&b, 1, &c) ||
      !sb_new_object(&b, 2, &d)) return 0;
  {
    struct gc_op op = op_set(GC_OP_SET_PROP, node_handle_at(&b.model, a), 0,
                             v_node(node_handle_at(&b.model, d)));
    if (!sb_emit(&b, &op)) return 0;
    op = op_set(GC_OP_SET_PROP, node_handle_at(&b.model, c), 1,
                v_node(node_handle_at(&b.model, d)));
    if (!sb_emit(&b, &op)) return 0;
  }
  {
    struct gc_op op = op_root(GC_OP_RELEASE_ROOT, root_handle_at(&b.model, 2), 0);
    if (!sb_emit(&b, &op)) return 0;
  }
  if (!sb_emit(&b, &(struct gc_op){.opcode = GC_OP_GC_FULL})) return 0;
  {
    struct gc_op op = op_del(GC_OP_DEL_PROP, node_handle_at(&b.model, a), 0);
    if (!sb_emit(&b, &op)) return 0;
    op = op_del(GC_OP_DEL_PROP, node_handle_at(&b.model, c), 1);
    if (!sb_emit(&b, &op)) return 0;
  }
  if (!sb_emit(&b, &(struct gc_op){.opcode = GC_OP_GC_MINOR})) return 0;
  return replay_deterministic("shared-child", b.buf, b.len, 1, 1);
}

static int case_cycle(void) {
  struct seq_builder b;
  int a, c;
  sb_init(&b);
  if (!sb_new_object(&b, 0, &a) || !sb_new_object(&b, 1, &c)) return 0;
  {
    struct gc_op op = op_set(GC_OP_SET_PROP, node_handle_at(&b.model, a), 0,
                             v_node(node_handle_at(&b.model, c)));
    if (!sb_emit(&b, &op)) return 0;
    op = op_set(GC_OP_SET_PROP, node_handle_at(&b.model, c), 0,
                v_node(node_handle_at(&b.model, a)));
    if (!sb_emit(&b, &op)) return 0;
    op = op_root(GC_OP_RELEASE_ROOT, root_handle_at(&b.model, 1), 0);
    if (!sb_emit(&b, &op)) return 0;
  }
  if (!sb_emit(&b, &(struct gc_op){.opcode = GC_OP_GC_FULL})) return 0;
  {
    struct gc_op op = op_root(GC_OP_COPY_ROOT, root_handle_at(&b.model, 0), 2);
    if (!sb_emit(&b, &op)) return 0;
    op = op_root(GC_OP_RELEASE_ROOT, root_handle_at(&b.model, 0), 0);
    if (!sb_emit(&b, &op)) return 0;
  }
  if (!sb_emit(&b, &(struct gc_op){.opcode = GC_OP_GC_FULL})) return 0;
  {
    struct gc_op op = op_set(GC_OP_SET_PROP, node_handle_at(&b.model, c), 1,
                             v_node(node_handle_at(&b.model, a)));
    if (!sb_emit(&b, &op)) return 0;
  }
  if (!sb_emit(&b, &(struct gc_op){.opcode = GC_OP_GC_MINOR})) return 0;
  return replay_deterministic("cycle", b.buf, b.len, 1, 1);
}

static int case_alternate_edge(void) {
  struct seq_builder b;
  int a, c, d;
  sb_init(&b);
  if (!sb_new_object(&b, 0, &a) || !sb_new_object(&b, 1, &c) ||
      !sb_new_object(&b, 2, &d)) return 0;
  {
    struct gc_op op = op_set(GC_OP_SET_PROP, node_handle_at(&b.model, a), 0,
                             v_node(node_handle_at(&b.model, c)));
    if (!sb_emit(&b, &op)) return 0;
    op = op_set(GC_OP_SET_PROP, node_handle_at(&b.model, a), 1,
                v_node(node_handle_at(&b.model, d)));
    if (!sb_emit(&b, &op)) return 0;
    op = op_set(GC_OP_SET_PROP, node_handle_at(&b.model, d), 0,
                v_node(node_handle_at(&b.model, c)));
    if (!sb_emit(&b, &op)) return 0;
    op = op_root(GC_OP_RELEASE_ROOT, root_handle_at(&b.model, 1), 0);
    if (!sb_emit(&b, &op)) return 0;
    op = op_del(GC_OP_DEL_PROP, node_handle_at(&b.model, a), 0);
    if (!sb_emit(&b, &op)) return 0;
  }
  if (!sb_emit(&b, &(struct gc_op){.opcode = GC_OP_GC_FULL})) return 0;
  {
    struct gc_op op = op_del(GC_OP_DEL_PROP, node_handle_at(&b.model, d), 0);
    if (!sb_emit(&b, &op)) return 0;
    op = op_set(GC_OP_SET_PROP, node_handle_at(&b.model, a), 0,
                v_node(node_handle_at(&b.model, d)));
    if (!sb_emit(&b, &op)) return 0;
    op = op_del(GC_OP_DEL_PROP, node_handle_at(&b.model, a), 1);
    if (!sb_emit(&b, &op)) return 0;
  }
  if (!sb_emit(&b, &(struct gc_op){.opcode = GC_OP_GC_MINOR})) return 0;
  return replay_deterministic("alternate-edge", b.buf, b.len, 1, 1);
}

static int case_array_holes(void) {
  struct seq_builder b;
  int arr, o, p;
  sb_init(&b);
  if (!sb_new_array(&b, 0, &arr) || !sb_new_object(&b, 1, &o) ||
      !sb_new_object(&b, 2, &p)) return 0;
  {
    struct gc_op op = op_set(GC_OP_ARRAY_SET, node_handle_at(&b.model, arr), 0,
                             v_node(node_handle_at(&b.model, o)));
    if (!sb_emit(&b, &op)) return 0;
    op = op_set(GC_OP_ARRAY_SET, node_handle_at(&b.model, arr), 2,
                v_node(node_handle_at(&b.model, p)));
    if (!sb_emit(&b, &op)) return 0;
    op = op_set(GC_OP_ARRAY_SET, node_handle_at(&b.model, arr), 3,
                v_undefined());
    if (!sb_emit(&b, &op)) return 0;
    op = op_set(GC_OP_ARRAY_SET, node_handle_at(&b.model, arr), 5,
                v_undefined());
    if (!sb_emit(&b, &op)) return 0;
    op = op_del(GC_OP_ARRAY_DEL, node_handle_at(&b.model, arr), 5);
    if (!sb_emit(&b, &op)) return 0;
    op = op_set(GC_OP_ARRAY_SET, node_handle_at(&b.model, arr), 4,
                v_undefined());
    if (!sb_emit(&b, &op)) return 0;
    op = op_del(GC_OP_ARRAY_DEL, node_handle_at(&b.model, arr), 2);
    if (!sb_emit(&b, &op)) return 0;
    op = op_del(GC_OP_ARRAY_DEL, node_handle_at(&b.model, arr), 4);
    if (!sb_emit(&b, &op)) return 0;
    op = op_del(GC_OP_ARRAY_DEL, node_handle_at(&b.model, arr), 3);
    if (!sb_emit(&b, &op)) return 0;
    op = op_set(GC_OP_ARRAY_SET, node_handle_at(&b.model, arr), 1,
                v_undefined());
    if (!sb_emit(&b, &op)) return 0;
    op = op_del(GC_OP_ARRAY_DEL, node_handle_at(&b.model, arr), 0);
    if (!sb_emit(&b, &op)) return 0;
    op = op_del(GC_OP_ARRAY_DEL, node_handle_at(&b.model, arr), 1);
    if (!sb_emit(&b, &op)) return 0;
    op = op_root(GC_OP_RELEASE_ROOT, root_handle_at(&b.model, 1), 0);
    if (!sb_emit(&b, &op)) return 0;
    op = op_root(GC_OP_RELEASE_ROOT, root_handle_at(&b.model, 2), 0);
    if (!sb_emit(&b, &op)) return 0;
  }
  if (!sb_emit(&b, &(struct gc_op){.opcode = GC_OP_GC_FULL})) return 0;
  return replay_deterministic("array-hole-vs-undefined", b.buf, b.len, 1, 1);
}

static int case_array_length(void) {
  struct seq_builder b;
  int arr, o, p;
  sb_init(&b);
  if (!sb_new_array(&b, 0, &arr) || !sb_new_object(&b, 1, &o) ||
      !sb_new_object(&b, 2, &p)) return 0;
  {
    struct gc_op op = op_set(GC_OP_ARRAY_SET, node_handle_at(&b.model, arr), 7,
                             v_node(node_handle_at(&b.model, o)));
    if (!sb_emit(&b, &op)) return 0;
    op = op_set(GC_OP_ARRAY_SET, node_handle_at(&b.model, arr), 1,
                v_node(node_handle_at(&b.model, p)));
    if (!sb_emit(&b, &op)) return 0;
    op = op_set(GC_OP_ARRAY_SET, node_handle_at(&b.model, arr), 3,
                v_number(7));
    if (!sb_emit(&b, &op)) return 0;
    op = op_root(GC_OP_RELEASE_ROOT, root_handle_at(&b.model, 1), 0);
    if (!sb_emit(&b, &op)) return 0;
    op = op_root(GC_OP_RELEASE_ROOT, root_handle_at(&b.model, 2), 0);
    if (!sb_emit(&b, &op)) return 0;
    op = op_del(GC_OP_ARRAY_DEL, node_handle_at(&b.model, arr), 7);
    if (!sb_emit(&b, &op)) return 0;
    op = op_del(GC_OP_ARRAY_DEL, node_handle_at(&b.model, arr), 3);
    if (!sb_emit(&b, &op)) return 0;
    op = op_set(GC_OP_ARRAY_SET, node_handle_at(&b.model, arr), 0,
                v_bool(1));
    if (!sb_emit(&b, &op)) return 0;
    op = op_del(GC_OP_ARRAY_DEL, node_handle_at(&b.model, arr), 1);
    if (!sb_emit(&b, &op)) return 0;
    op = op_del(GC_OP_ARRAY_DEL, node_handle_at(&b.model, arr), 0);
    if (!sb_emit(&b, &op)) return 0;
    op = op_set(GC_OP_ARRAY_SET, node_handle_at(&b.model, arr), 10,
                v_undefined());
    if (!sb_emit(&b, &op)) return 0;
    op = op_del(GC_OP_ARRAY_DEL, node_handle_at(&b.model, arr), 10);
    if (!sb_emit(&b, &op)) return 0;
  }
  if (!sb_emit(&b, &(struct gc_op){.opcode = GC_OP_GC_FULL})) return 0;
  return replay_deterministic("array-length-delete", b.buf, b.len, 1, 1);
}

static int case_root_release_internal(void) {
  struct seq_builder b;
  int a, c;
  sb_init(&b);
  if (!sb_new_object(&b, 0, &a) || !sb_new_object(&b, 1, &c)) return 0;
  {
    struct gc_op op = op_set(GC_OP_SET_PROP, node_handle_at(&b.model, a), 0,
                             v_node(node_handle_at(&b.model, c)));
    if (!sb_emit(&b, &op)) return 0;
    op = op_root(GC_OP_RELEASE_ROOT, root_handle_at(&b.model, 1), 0);
    if (!sb_emit(&b, &op)) return 0;
  }
  if (!sb_emit(&b, &(struct gc_op){.opcode = GC_OP_GC_FULL})) return 0;
  {
    struct gc_op op = op_root(GC_OP_COPY_ROOT, root_handle_at(&b.model, 0), 2);
    if (!sb_emit(&b, &op)) return 0;
    op = op_root(GC_OP_RELEASE_ROOT, root_handle_at(&b.model, 0), 0);
    if (!sb_emit(&b, &op)) return 0;
  }
  if (!sb_emit(&b, &(struct gc_op){.opcode = GC_OP_GC_FULL})) return 0;
  {
    struct gc_op op = op_set(GC_OP_SET_PROP, node_handle_at(&b.model, a), 1,
                             v_node(node_handle_at(&b.model, c)));
    if (!sb_emit(&b, &op)) return 0;
    op = op_del(GC_OP_DEL_PROP, node_handle_at(&b.model, a), 0);
    if (!sb_emit(&b, &op)) return 0;
  }
  if (!sb_emit(&b, &(struct gc_op){.opcode = GC_OP_GC_MINOR})) return 0;
  {
    struct gc_op op = op_del(GC_OP_DEL_PROP, node_handle_at(&b.model, a), 1);
    if (!sb_emit(&b, &op)) return 0;
  }
  if (!sb_emit(&b, &(struct gc_op){.opcode = GC_OP_GC_FULL})) return 0;
  return replay_deterministic("root-release-internal", b.buf, b.len, 1, 1);
}

static int case_string_compaction(void) {
  struct seq_builder b;
  unsigned char garbage[96], live[96];
  int gs, ls, obj;
  int i;
  sb_init(&b);
  for (i = 0; i < (int) sizeof(garbage); i++) {
    garbage[i] = (unsigned char) ('A' + (i % 19));
  }
  garbage[7] = 0;
  for (i = 0; i < (int) sizeof(live); i++) {
    live[i] = (unsigned char) ('a' + (i % 23));
  }
  live[0] = 'L';
  live[31] = 0;
  live[32] = 'X';
  live[95] = 0;

  if (!sb_new_string(&b, 0, garbage, sizeof(garbage), &gs)) return 0;
  {
    struct gc_op op = op_root(GC_OP_RELEASE_ROOT, root_handle_at(&b.model, 0), 0);
    if (!sb_emit(&b, &op)) return 0;
  }
  /* Garbage remains in the real string buffer until the GC below. */
  if (!sb_new_string(&b, 1, live, sizeof(live), &ls)) return 0;
  if (!sb_new_object(&b, 2, &obj)) return 0;
  {
    struct gc_op op = op_set(GC_OP_SET_PROP, node_handle_at(&b.model, obj), 0,
                             v_node(node_handle_at(&b.model, ls)));
    if (!sb_emit(&b, &op)) return 0;
    op = op_root(GC_OP_COPY_ROOT, root_handle_at(&b.model, 1), 3);
    if (!sb_emit(&b, &op)) return 0;
    op = op_root(GC_OP_RELEASE_ROOT, root_handle_at(&b.model, 1), 0);
    if (!sb_emit(&b, &op)) return 0;
  }
  /* Full GC packs the live string past the garbage and must update both
     aliases (root 3 and the object property). */
  if (!sb_emit(&b, &(struct gc_op){.opcode = GC_OP_GC_FULL})) return 0;
  {
    struct gc_op op = op_set(GC_OP_SET_PROP, node_handle_at(&b.model, obj), 1,
                             v_node(node_handle_at(&b.model, ls)));
    if (!sb_emit(&b, &op)) return 0;
    op = op_root(GC_OP_RELEASE_ROOT, root_handle_at(&b.model, 3), 0);
    if (!sb_emit(&b, &op)) return 0;
  }
  if (!sb_emit(&b, &(struct gc_op){.opcode = GC_OP_GC_FULL})) return 0;
  {
    struct gc_op op = op_del(GC_OP_DEL_PROP, node_handle_at(&b.model, obj), 0);
    if (!sb_emit(&b, &op)) return 0;
  }
  if (!sb_emit(&b, &(struct gc_op){.opcode = GC_OP_GC_MINOR})) return 0;
  {
    struct gc_op op = op_del(GC_OP_DEL_PROP, node_handle_at(&b.model, obj), 1);
    if (!sb_emit(&b, &op)) return 0;
  }
  if (!sb_emit(&b, &(struct gc_op){.opcode = GC_OP_GC_FULL})) return 0;
  return replay_deterministic("string-compaction-aliases", b.buf, b.len, 1, 1);
}

static int case_string_embedded_nul(void) {
  struct seq_builder b;
  unsigned char bytes[128], shortstr[4];
  int ls, ss, obj;
  int i;
  sb_init(&b);
  for (i = 0; i < (int) sizeof(bytes); i++) {
    bytes[i] = (unsigned char) ('0' + (i % 10));
  }
  bytes[1] = 0;
  bytes[2] = 'Z';
  bytes[63] = 0;
  bytes[64] = 0xff;
  bytes[127] = 0x7f;
  shortstr[0] = 'a';
  shortstr[1] = 0;
  shortstr[2] = 'b';
  shortstr[3] = 'c';

  if (!sb_new_string(&b, 0, bytes, sizeof(bytes), &ls)) return 0;
  if (!sb_new_object(&b, 1, &obj)) return 0;
  if (!sb_new_string(&b, 2, shortstr, sizeof(shortstr), &ss)) return 0;
  {
    struct gc_op op = op_set(GC_OP_SET_PROP, node_handle_at(&b.model, obj), 0,
                             v_node(node_handle_at(&b.model, ls)));
    if (!sb_emit(&b, &op)) return 0;
    op = op_set(GC_OP_SET_PROP, node_handle_at(&b.model, obj), 1,
                v_node(node_handle_at(&b.model, ss)));
    if (!sb_emit(&b, &op)) return 0;
    op = op_root(GC_OP_RELEASE_ROOT, root_handle_at(&b.model, 0), 0);
    if (!sb_emit(&b, &op)) return 0;
    op = op_root(GC_OP_RELEASE_ROOT, root_handle_at(&b.model, 2), 0);
    if (!sb_emit(&b, &op)) return 0;
  }
  if (!sb_emit(&b, &(struct gc_op){.opcode = GC_OP_GC_FULL})) return 0;
  {
    struct gc_op op = op_root(GC_OP_COPY_ROOT, root_handle_at(&b.model, 1), 3);
    if (!sb_emit(&b, &op)) return 0;
    op = op_root(GC_OP_RELEASE_ROOT, root_handle_at(&b.model, 1), 0);
    if (!sb_emit(&b, &op)) return 0;
    op = op_set(GC_OP_SET_PROP, node_handle_at(&b.model, obj), 2,
                v_node(node_handle_at(&b.model, ls)));
    if (!sb_emit(&b, &op)) return 0;
    op = op_del(GC_OP_DEL_PROP, node_handle_at(&b.model, obj), 0);
    if (!sb_emit(&b, &op)) return 0;
  }
  if (!sb_emit(&b, &(struct gc_op){.opcode = GC_OP_GC_MINOR})) return 0;
  {
    struct gc_op op = op_del(GC_OP_DEL_PROP, node_handle_at(&b.model, obj), 1);
    if (!sb_emit(&b, &op)) return 0;
    op = op_del(GC_OP_DEL_PROP, node_handle_at(&b.model, obj), 2);
    if (!sb_emit(&b, &op)) return 0;
  }
  if (!sb_emit(&b, &(struct gc_op){.opcode = GC_OP_GC_FULL})) return 0;
  return replay_deterministic("string-embedded-nul", b.buf, b.len, 1, 1);
}

/* ------------------------------------------------------------------ */
/* Seeded generator                                                    */
/* ------------------------------------------------------------------ */

static uint64_t gen_rng_state = 0x9e3779b97f4a7c15ULL;

static uint32_t gen_rand(void) {
  uint64_t x = gen_rng_state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  gen_rng_state = x;
  return (uint32_t) (x & 0xffffffffULL);
}

static uint32_t gen_mod(uint32_t n) {
  return n == 0 ? 0 : gen_rand() % n;
}

static int pick_node_of_kind(const struct gc_model *m, uint8_t kind,
                             struct gc_handle *out) {
  struct gc_handle list[GC_MODEL_MAX_NODES];
  int n = gc_model_reachable_nodes(m, list, GC_MODEL_MAX_NODES);
  int i, count = 0;
  for (i = 0; i < n; i++) {
    if (m->nodes[list[i].slot].kind == kind) count++;
  }
  if (count == 0) return 0;
  count = (int) gen_mod((uint32_t) count);
  for (i = 0; i < n; i++) {
    if (m->nodes[list[i].slot].kind == kind) {
      if (count == 0) {
        *out = list[i];
        return 1;
      }
      count--;
    }
  }
  return 0;
}

static int pick_reachable_node(const struct gc_model *m, struct gc_handle *out) {
  struct gc_handle list[GC_MODEL_MAX_NODES];
  int n = gc_model_reachable_nodes(m, list, GC_MODEL_MAX_NODES);
  if (n == 0) return 0;
  *out = list[gen_mod((uint32_t) n)];
  return 1;
}

static int pick_used_root(const struct gc_model *m, uint8_t *slot) {
  int used[GC_MODEL_MAX_ROOTS];
  int n = 0, i;
  for (i = 0; i < GC_MODEL_MAX_ROOTS; i++) {
    if (m->roots[i].used) used[n++] = i;
  }
  if (n == 0) return 0;
  *slot = (uint8_t) used[gen_mod((uint32_t) n)];
  return 1;
}

static struct gc_value_spec gen_value(const struct gc_model *m) {
  struct gc_handle h;
  switch (gen_mod(6)) {
    case 0:
    case 1:
    case 2:
      if (pick_reachable_node(m, &h)) return v_node(h);
      return v_undefined();
    case 3:
      return v_undefined();
    case 4:
      return v_bool((int) gen_mod(2));
    case 5:
      return v_null();
    default:
      return v_number(gen_rand());
  }
}

static void gen_bytes(unsigned char *p, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    p[i] = (unsigned char) gen_rand();
    if ((gen_rand() & 7) == 0) p[i] = 0; /* embedded NULs */
  }
}

static int append_generated_sequence(struct seq_builder *b) {
  unsigned char s1[256], s2[256];
  int gs, ls, obj, step;
  struct gc_op op;

  sb_init(b);

  /* Every generated sequence starts with the compaction shape: one long
     garbage string, then a rooted long live string with a root alias and a
     object-property alias. */
  gen_bytes(s1, 72);
  if (!sb_new_string(b, 0, s1, 72, &gs)) return 0;
  op = op_root(GC_OP_RELEASE_ROOT, root_handle_at(&b->model, 0), 0);
  if (!sb_emit(b, &op)) return 0;
  gen_bytes(s2, 96);
  if (!sb_new_string(b, 1, s2, 96, &ls)) return 0;
  if (!sb_new_object(b, 2, &obj)) return 0;
  op = op_set(GC_OP_SET_PROP, node_handle_at(&b->model, obj), 0,
              v_node(node_handle_at(&b->model, ls)));
  if (!sb_emit(b, &op)) return 0;
  op = op_root(GC_OP_COPY_ROOT, root_handle_at(&b->model, 1), 3);
  if (!sb_emit(b, &op)) return 0;
  op = op_root(GC_OP_RELEASE_ROOT, root_handle_at(&b->model, 1), 0);
  if (!sb_emit(b, &op)) return 0;
  op = op_gc(1);
  if (!sb_emit(b, &op)) return 0;

  for (step = 0; step < GEN_OPS; step++) {
    int done = 0, attempt;
    unsigned char newstr[GC_MODEL_MAX_STRING];
    for (attempt = 0; attempt < 64 && !done; attempt++) {
      uint32_t pick = gen_mod(100);
      struct gc_op cand;
      memset(&cand, 0, sizeof(cand));

      if (pick < 20) {
        int root = find_free_root(&b->model);
        int node = find_free_node(&b->model);
        uint32_t kind;
        if (root < 0 || node < 0) continue;
        kind = gen_mod(10);
        if (kind < 6) {
          cand = op_new(GC_OP_NEW_OBJECT, (uint8_t) root);
        } else if (kind < 8) {
          cand = op_new(GC_OP_NEW_ARRAY, (uint8_t) root);
        } else {
          size_t len;
          if (gen_mod(3) == 0) {
            len = 6 + gen_mod((uint32_t) (GC_MODEL_MAX_STRING - 5));
          } else {
            len = gen_mod(6);
          }
          gen_bytes(newstr, len);
          cand = op_new_string((uint8_t) root, newstr, len);
        }
      } else if (pick < 55) {
        struct gc_handle target;
        uint8_t target_kind;
        if (gen_mod(100) < 60) target_kind = GC_NODE_OBJECT;
        else target_kind = GC_NODE_ARRAY;
        if (!pick_node_of_kind(&b->model, target_kind, &target)) continue;
        cand.handle = target;
        cand.dst = (uint8_t) (target_kind == GC_NODE_ARRAY
                                  ? gen_mod(GC_MODEL_MAX_ARRAY_INDEX)
                                  : gen_mod(GC_MODEL_MAX_OBJECT_KEYS));
        if (gen_mod(100) < 65) {
          cand.opcode = target_kind == GC_NODE_ARRAY ? GC_OP_ARRAY_SET
                                                     : GC_OP_SET_PROP;
          cand.value = gen_value(&b->model);
        } else {
          cand.opcode = target_kind == GC_NODE_ARRAY ? GC_OP_ARRAY_DEL
                                                     : GC_OP_DEL_PROP;
        }
      } else if (pick < 67) {
        uint8_t src;
        if (!pick_used_root(&b->model, &src)) continue;
        cand = op_root(GC_OP_COPY_ROOT,
                       root_handle_at(&b->model, src), (uint8_t) gen_mod(16));
      } else if (pick < 77) {
        uint8_t src;
        if (!pick_used_root(&b->model, &src)) continue;
        cand = op_root(GC_OP_RELEASE_ROOT,
                       root_handle_at(&b->model, src), 0);
      } else {
        cand = op_gc((int) gen_mod(2));
      }

      if (gc_model_validate(&b->model, &cand, NULL) && sb_emit(b, &cand)) {
        done = 1;
      }
    }
    if (!done) {
      struct gc_op gc = op_gc(1);
      if (!sb_emit(b, &gc)) return 0;
    }
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* Invalid-input self checks                                           */
/* ------------------------------------------------------------------ */

static int check_invalid_inputs(void) {
  struct seq_builder b;
  struct gc_seq_result r;
  unsigned char bad_header[5] = {'M', 'J', 'S', 'G', 2};
  struct gc_op op;
  int a;
  struct gc_handle oldroot;

  if (gc_sequence_run(bad_header, sizeof(bad_header), &r) !=
          GC_SEQ_STATUS_INVALID_INPUT ||
      r.ops_executed != 0 || !r.invalid_prefix) {
    printf("FAIL invalid-input bad-header\n");
    return 0;
  }

  sb_init(&b);
  if (!sb_new_object(&b, 0, &a)) return 0;
  /* Truncated record: complete header, declared payload missing. */
  b.buf[b.len++] = GC_OP_NEW_OBJECT;
  sb_u16(&b, 1); /* payload_len says one byte, but none follows */
  if (gc_sequence_run(b.buf, b.len, &r) != GC_SEQ_STATUS_INVALID_INPUT ||
      r.ops_executed != 1 || r.bad_record != 1 || !r.invalid_prefix) {
    printf("FAIL invalid-input truncated-record\n");
    return 0;
  }

  sb_init(&b);
  if (!sb_new_object(&b, 0, &a)) return 0;
  oldroot = root_handle_at(&b.model, 0);
  op = op_root(GC_OP_RELEASE_ROOT, oldroot, 0);
  if (!sb_emit(&b, &op)) return 0;
  /* Reuse the same slot; the old generation must not address the new object. */
  {
    struct gc_op reuse = op_new(GC_OP_NEW_OBJECT, 0);
    if (!sb_emit(&b, &reuse)) return 0;
  }
  {
    unsigned char payload[4];
    payload[0] = oldroot.slot;
    payload[1] = (unsigned char) (oldroot.gen & 0xff);
    payload[2] = (unsigned char) ((oldroot.gen >> 8) & 0xff);
    payload[3] = 1;
    if (!sb_append_raw(&b, GC_OP_COPY_ROOT, payload, 4)) return 0;
  }
  if (gc_sequence_run(b.buf, b.len, &r) != GC_SEQ_STATUS_INVALID_INPUT ||
      r.ops_executed != 3 || r.bad_record != 3 || !r.invalid_prefix ||
      strstr(r.detail, "stale") == NULL) {
    printf("FAIL invalid-input stale-root-generation (%s)\n", r.detail);
    return 0;
  }

  sb_init(&b);
  if (!sb_new_object(&b, 0, &a)) return 0;
  {
    unsigned char payload[4];
    payload[0] = 200; /* root slot out of range */
    payload[1] = 0;
    payload[2] = 0;
    payload[3] = 1;
    if (!sb_append_raw(&b, GC_OP_COPY_ROOT, payload, 4)) return 0;
  }
  if (gc_sequence_run(b.buf, b.len, &r) != GC_SEQ_STATUS_INVALID_INPUT ||
      r.ops_executed != 1 || r.bad_record != 1 || !r.invalid_prefix) {
    printf("FAIL invalid-input out-of-range-handle\n");
    return 0;
  }

  printf("INVALID-INPUT bad-header truncated-record stale-root-generation "
         "out-of-range-handle OK\n");
  return 1;
}

int main(void) {
  int cases = 0;
  int gen_ok = 0;
  int i;

  cases += case_shared_child();
  cases += case_cycle();
  cases += case_alternate_edge();
  cases += case_array_holes();
  cases += case_array_length();
  cases += case_root_release_internal();
  cases += case_string_compaction();
  cases += case_string_embedded_nul();

  if (cases != 8) {
    printf("FAIL fixed cases passed=%d/8\n", cases);
    return 1;
  }

  for (i = 0; i < GEN_SEQUENCE_COUNT; i++) {
    struct seq_builder b;
    char name[64];
    if (!append_generated_sequence(&b)) {
      printf("FAIL generated sequence %d could not be built\n", i);
      return 1;
    }
    snprintf(name, sizeof(name), "generated-%02d", i);
    if (!replay_deterministic(name, b.buf, b.len, 1, 0)) return 1;
    gen_ok++;
  }

  if (!check_invalid_inputs()) return 1;

  printf("MJS_GC_SEQUENCE_TEST_OK cases=8 generated=%d mutants=0\n", gen_ok);
  return 0;
}
