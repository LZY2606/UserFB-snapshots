/*
 * GC state-sequence entry point.
 *
 * This file never executes JavaScript and never calls an FFI resolver.  It
 * only builds small object graphs through the public mJS C API, replays a
 * binary operation sequence (format documented in gc_sequence.h) and checks
 * the real graph against the independent model in gc_model.c after every
 * executed record.
 *
 * Safety rules followed by the checker:
 *  - every subvalue used by an operation is re-read from a live external root
 *    along a fresh model path; no mjs_val_t is cached across an operation;
 *  - borrowed string pointers are consumed immediately and never cross a
 *    call that could allocate or collect;
 *  - external root variables live in this context for the whole mJS lifetime,
 *    so mjs_own() always registers a stable address;
 *  - truncated or invalid records stop the valid-prefix replay without
 *    touching interpreter state with a stale value.
 */

#include "mjs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc_model.h"
#include "gc_sequence.h"

#define GC_SEQ_FNV_OFFSET 1469598103934665603ULL
#define GC_SEQ_FNV_PRIME 1099511628211ULL

struct seq_ctx {
  struct mjs *mjs;
  struct gc_model model;
  mjs_val_t roots[GC_MODEL_MAX_ROOTS];
  uint8_t root_owned[GC_MODEL_MAX_ROOTS];
  struct gc_seq_result *res;
  size_t op_index;
};

struct cmp_entry {
  int used;
  uint8_t node_slot;
  uint16_t node_gen;
  mjs_val_t real;
  uint32_t serial;
};

struct cmp_ctx {
  struct mjs *mjs;
  const struct gc_model *model;
  struct cmp_entry map[GC_MODEL_MAX_NODES];
  int nserial;
  size_t op_index;
  int root_slot;
  unsigned root_gen;
  char path[GC_SEQUENCE_DIAG_MAX];
  size_t path_len;
  uint64_t digest;
  struct gc_seq_result *res;
  int failed;
};

static uint16_t rd_u16(const unsigned char *p) {
  return (uint16_t) ((uint16_t) p[0] | ((uint16_t) p[1] << 8));
}

static uint32_t rd_u32(const unsigned char *p) {
  return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
         ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static void hash_u8(struct cmp_ctx *c, uint8_t v) {
  c->digest ^= v;
  c->digest *= GC_SEQ_FNV_PRIME;
}

static void hash_u32(struct cmp_ctx *c, uint32_t v) {
  int i;
  for (i = 0; i < 4; i++) hash_u8(c, (uint8_t) (v >> (i * 8)));
}

static void hash_bytes(struct cmp_ctx *c, const unsigned char *p, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) hash_u8(c, p[i]);
}

static void hash_double(struct cmp_ctx *c, double d) {
  uint64_t bits = 0;
  memcpy(&bits, &d, sizeof(bits));
  hash_u32(c, (uint32_t) bits);
  hash_u32(c, (uint32_t) (bits >> 32));
}

static int cmp_fail(struct cmp_ctx *c, const char *detail) {
  if (c->failed) return -1;
  c->failed = 1;
  c->res->status = GC_SEQ_STATUS_MISMATCH;
  c->res->bad_op = c->op_index;
  c->res->root_slot = c->root_slot;
  c->res->root_gen = c->root_gen;
  c->res->path[0] = '\0';
  c->res->detail[0] = '\0';
  if (c->path_len < sizeof(c->res->path)) {
    memcpy(c->res->path, c->path, c->path_len);
    c->res->path[c->path_len] = '\0';
  } else {
    memcpy(c->res->path, c->path, sizeof(c->res->path) - 1);
    c->res->path[sizeof(c->res->path) - 1] = '\0';
  }
  if (detail != NULL) {
    snprintf(c->res->detail, sizeof(c->res->detail), "%s", detail);
  }
  return -1;
}

static int internal_fail(struct seq_ctx *ctx, const char *detail) {
  ctx->res->status = GC_SEQ_STATUS_INTERNAL_ERROR;
  ctx->res->bad_op = ctx->op_index;
  ctx->res->root_slot = -1;
  ctx->res->root_gen = 0;
  snprintf(ctx->res->detail, sizeof(ctx->res->detail), "%s", detail);
  return -1;
}

static void path_reset(struct cmp_ctx *c, int root_slot, unsigned root_gen) {
  size_t n = 0;
  c->root_slot = root_slot;
  c->root_gen = root_gen;
  n = (size_t) snprintf(c->path, sizeof(c->path), "root[%d]", root_slot);
  if (n >= sizeof(c->path)) n = sizeof(c->path) - 1;
  c->path_len = n;
}

static void path_push_prop(struct cmp_ctx *c, unsigned key_id) {
  char tmp[16];
  int n = snprintf(tmp, sizeof(tmp), "->k%u", key_id);
  if (n < 0) n = 0;
  if (c->path_len + (size_t) n >= sizeof(c->path)) return;
  memcpy(c->path + c->path_len, tmp, (size_t) n);
  c->path_len += (size_t) n;
  c->path[c->path_len] = '\0';
}

static void path_push_index(struct cmp_ctx *c, unsigned index) {
  char tmp[16];
  int n = snprintf(tmp, sizeof(tmp), "[%u]", index);
  if (n < 0) n = 0;
  if (c->path_len + (size_t) n >= sizeof(c->path)) return;
  memcpy(c->path + c->path_len, tmp, (size_t) n);
  c->path_len += (size_t) n;
  c->path[c->path_len] = '\0';
}

static void path_pop(struct cmp_ctx *c, size_t old_len) {
  c->path_len = old_len;
  c->path[old_len] = '\0';
}

static struct cmp_entry *map_by_model(struct cmp_ctx *c, uint8_t slot,
                                      uint16_t gen) {
  int i;
  for (i = 0; i < GC_MODEL_MAX_NODES; i++) {
    if (c->map[i].used && c->map[i].node_slot == slot &&
        c->map[i].node_gen == gen) {
      return &c->map[i];
    }
  }
  return NULL;
}

static struct cmp_entry *map_by_real(struct cmp_ctx *c, mjs_val_t real,
                                     int objectish_only) {
  int i;
  for (i = 0; i < GC_MODEL_MAX_NODES; i++) {
    if (!c->map[i].used || c->map[i].real != real) continue;
    if (!objectish_only) return &c->map[i];
    if (c->model->nodes[c->map[i].node_slot].kind == GC_NODE_OBJECT ||
        c->model->nodes[c->map[i].node_slot].kind == GC_NODE_ARRAY) {
      return &c->map[i];
    }
  }
  return NULL;
}

static int cmp_value(struct cmp_ctx *c, const struct gc_value_spec *v,
                     mjs_val_t real);

static int cmp_object(struct cmp_ctx *c, const struct gc_model_node *mn,
                      mjs_val_t real) {
  uint8_t seen[GC_MODEL_MAX_OBJECT_KEYS];
  mjs_val_t iter = mjs_mk_undefined();
  unsigned count = 0;
  int i;

  memset(seen, 0, sizeof(seen));
  while (1) {
    mjs_val_t key;
    const char *kp;
    size_t klen = 0;
    char keybuf[GC_MODEL_KEY_NAME_MAX + 1];
    int key_id = -1;
    size_t old_len;
    mjs_val_t value;

    key = mjs_next(c->mjs, real, &iter);
    if (key == mjs_mk_undefined()) break;

    /* Borrowed key pointer: copy immediately, before any other mJS call. */
    kp = mjs_get_string(c->mjs, &key, &klen);
    if (kp == NULL || klen > GC_MODEL_KEY_NAME_MAX) {
      return cmp_fail(c, "object has a non-canonical property name");
    }
    memcpy(keybuf, kp, klen);
    keybuf[klen] = '\0';

    for (i = 0; i < GC_MODEL_MAX_OBJECT_KEYS; i++) {
      char name[GC_MODEL_KEY_NAME_MAX];
      size_t n = gc_model_key_name((uint8_t) i, name, sizeof(name));
      if (n == klen && memcmp(name, keybuf, klen) == 0) {
        key_id = i;
        break;
      }
    }
    if (key_id < 0) {
      return cmp_fail(c, "object has an unexpected own property");
    }
    if (seen[key_id]) {
      return cmp_fail(c, "object property enumerated twice");
    }
    seen[key_id] = 1;
    if (!mn->obj_present[key_id]) {
      return cmp_fail(c, "object has an extra own property");
    }

    hash_u8(c, (uint8_t) key_id);
    value = mjs_get(c->mjs, real, keybuf, klen);
    old_len = c->path_len;
    path_push_prop(c, (unsigned) key_id);
    if (cmp_value(c, &mn->obj[key_id], value) != 0) {
      path_pop(c, old_len);
      return -1;
    }
    path_pop(c, old_len);
    count++;
  }

  for (i = 0; i < GC_MODEL_MAX_OBJECT_KEYS; i++) {
    if (mn->obj_present[i] && !seen[i]) {
      size_t old_len = c->path_len;
      path_push_prop(c, (unsigned) i);
      (void) old_len;
      return cmp_fail(c, "object is missing a model property");
    }
  }

  hash_u32(c, count);
  return 0;
}

static int parse_array_index(const char *s, size_t n, unsigned *out) {
  unsigned v = 0;
  size_t i;
  if (n == 0 || n > 6) return 0;
  if (s[0] == '0' && n != 1) return 0;
  for (i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return 0;
    v = v * 10u + (unsigned) (s[i] - '0');
    if (v >= GC_MODEL_MAX_ARRAY_INDEX) return 0;
  }
  *out = v;
  return 1;
}

static int cmp_array(struct cmp_ctx *c, const struct gc_model_node *mn,
                     mjs_val_t real) {
  uint8_t seen[GC_MODEL_MAX_ARRAY_INDEX];
  mjs_val_t iter = mjs_mk_undefined();
  unsigned count = 0;
  int i;
  unsigned long real_len = mjs_array_length(c->mjs, real);

  memset(seen, 0, sizeof(seen));
  while (1) {
    mjs_val_t key;
    const char *kp;
    size_t klen = 0;
    char keybuf[16];
    unsigned index = 0;
    size_t old_len;
    mjs_val_t value;

    key = mjs_next(c->mjs, real, &iter);
    if (key == mjs_mk_undefined()) break;

    kp = mjs_get_string(c->mjs, &key, &klen);
    if (kp == NULL || klen >= sizeof(keybuf)) {
      return cmp_fail(c, "array has an invalid property name");
    }
    memcpy(keybuf, kp, klen);
    keybuf[klen] = '\0';

    if (!parse_array_index(keybuf, klen, &index)) {
      return cmp_fail(c, "array has a non-index own property");
    }
    if (seen[index]) return cmp_fail(c, "array index enumerated twice");
    seen[index] = 1;
    if (!mn->arr_present[index]) {
      return cmp_fail(c, "array has an extra index");
    }

    hash_u8(c, (uint8_t) index);
    value = mjs_get(c->mjs, real, keybuf, klen);
    old_len = c->path_len;
    path_push_index(c, index);
    if (cmp_value(c, &mn->arr[index], value) != 0) {
      path_pop(c, old_len);
      return -1;
    }
    path_pop(c, old_len);
    count++;
  }

  for (i = 0; i < GC_MODEL_MAX_ARRAY_INDEX; i++) {
    if (mn->arr_present[i] && !seen[i]) {
      size_t old_len = c->path_len;
      path_push_index(c, (unsigned) i);
      (void) old_len;
      return cmp_fail(c, "array is missing a model index");
    }
  }

  if (real_len != gc_model_array_length(c->model, (uint8_t) (mn - c->model->nodes))) {
    return cmp_fail(c, "array length mismatch (holes vs explicit undefined)");
  }

  hash_u32(c, count);
  hash_u32(c, (uint32_t) real_len);
  return 0;
}

static int cmp_node(struct cmp_ctx *c, uint8_t slot, uint16_t gen,
                    mjs_val_t real) {
  struct cmp_entry *ent;
  const struct gc_model_node *mn;

  if (slot >= GC_MODEL_MAX_NODES || !c->model->nodes[slot].used ||
      c->model->nodes[slot].gen != gen) {
    return cmp_fail(c, "model node handle is stale");
  }
  mn = &c->model->nodes[slot];

  ent = map_by_model(c, slot, gen);
  if (ent != NULL) {
    if (ent->real != real) {
      return cmp_fail(c, "shared object alias was not preserved");
    }
    hash_u8(c, 'R');
    hash_u32(c, ent->serial);
    return 0;
  }

  if (mn->kind == GC_NODE_OBJECT || mn->kind == GC_NODE_ARRAY) {
    struct cmp_entry *other = map_by_real(c, real, 1);
    if (other != NULL) {
      return cmp_fail(c, "distinct model nodes share one real object");
    }
  }

  for (ent = c->map; ent < c->map + GC_MODEL_MAX_NODES; ent++) {
    if (!ent->used) {
      ent->used = 1;
      ent->node_slot = slot;
      ent->node_gen = gen;
      ent->real = real;
      ent->serial = (uint32_t) c->nserial++;
      break;
    }
  }
  if (ent == c->map + GC_MODEL_MAX_NODES) {
    return cmp_fail(c, "comparison identity table overflow");
  }

  hash_u8(c, 'N');
  hash_u8(c, mn->kind);
  hash_u32(c, ent->serial);

  if (mn->kind == GC_NODE_OBJECT) {
    if (!mjs_is_object(real) || mjs_is_array(real)) {
      return cmp_fail(c, "model object is not a real non-array object");
    }
    return cmp_object(c, mn, real);
  }
  if (mn->kind == GC_NODE_ARRAY) {
    if (!mjs_is_array(real)) {
      return cmp_fail(c, "model array is not a real array");
    }
    return cmp_array(c, mn, real);
  }
  if (mn->kind == GC_NODE_STRING) {
    const char *p;
    size_t len = 0;
    mjs_val_t v = real;
    if (!mjs_is_string(real)) {
      return cmp_fail(c, "model string is not a real string");
    }
    p = mjs_get_string(c->mjs, &v, &len);
    if (p == NULL) return cmp_fail(c, "string bytes are not readable");
    if (len != mn->str_len) return cmp_fail(c, "string length mismatch");
    /* Use the borrowed pointer and length immediately; no mJS call follows. */
    if (memcmp(p, mn->str, len) != 0) {
      return cmp_fail(c, "string byte content mismatch");
    }
    hash_u8(c, 'S');
    hash_u32(c, (uint32_t) len);
    hash_bytes(c, (const unsigned char *) p, len);
    return 0;
  }
  return cmp_fail(c, "unknown model node kind");
}

static int cmp_value(struct cmp_ctx *c, const struct gc_value_spec *v,
                     mjs_val_t real) {
  switch (v->kind) {
    case GC_VALUE_NODE:
      return cmp_node(c, v->node.slot, v->node.gen, real);
    case GC_VALUE_UNDEFINED:
      hash_u8(c, 'V');
      hash_u8(c, GC_VALUE_UNDEFINED);
      if (!mjs_is_undefined(real)) {
        return cmp_fail(c, "expected undefined");
      }
      return 0;
    case GC_VALUE_NULL:
      hash_u8(c, 'V');
      hash_u8(c, GC_VALUE_NULL);
      if (!mjs_is_null(real)) return cmp_fail(c, "expected null");
      return 0;
    case GC_VALUE_BOOLEAN:
      hash_u8(c, 'V');
      hash_u8(c, GC_VALUE_BOOLEAN);
      hash_u8(c, (uint8_t) v->boolean);
      if (!mjs_is_boolean(real) || mjs_get_bool(c->mjs, real) != v->boolean) {
        return cmp_fail(c, "boolean value mismatch");
      }
      return 0;
    case GC_VALUE_NUMBER:
      hash_u8(c, 'V');
      hash_u8(c, GC_VALUE_NUMBER);
      hash_double(c, (double) v->number);
      if (!mjs_is_number(real) ||
          mjs_get_double(c->mjs, real) != (double) v->number) {
        return cmp_fail(c, "number value mismatch");
      }
      return 0;
    default:
      return cmp_fail(c, "unknown model value kind");
  }
}

static int compare_graphs(struct seq_ctx *ctx) {
  struct cmp_ctx c;
  int slot;

  memset(&c, 0, sizeof(c));
  c.mjs = ctx->mjs;
  c.model = &ctx->model;
  c.res = ctx->res;
  c.op_index = ctx->op_index;
  c.digest = GC_SEQ_FNV_OFFSET;

  for (slot = 0; slot < GC_MODEL_MAX_ROOTS; slot++) {
    const struct gc_model_root *mr = &ctx->model.roots[slot];
    path_reset(&c, slot, mr->gen);
    hash_u8(&c, 'G');
    hash_u8(&c, (uint8_t) slot);
    hash_u8(&c, mr->used ? 1 : 0);

    if (mr->used) {
      if (!ctx->root_owned[slot]) {
        return cmp_fail(&c, "live model root is not owned in mJS");
      }
      if (cmp_node(&c, mr->node.slot, mr->node.gen, ctx->roots[slot]) != 0) {
        return -1;
      }
    } else {
      if (ctx->root_owned[slot]) {
        return cmp_fail(&c, "free model root is still owned in mJS");
      }
      if (ctx->roots[slot] != mjs_mk_undefined()) {
        return cmp_fail(&c, "free model root holds a real value");
      }
      hash_u8(&c, 'F');
    }
  }

  if (!c.failed) ctx->res->digest = c.digest;
  return c.failed ? -1 : 0;
}

static int resolve_path(struct seq_ctx *ctx, const struct gc_path *path,
                        mjs_val_t *out) {
  mjs_val_t v;
  int i;

  if (path->root_slot >= GC_MODEL_MAX_ROOTS ||
      !ctx->root_owned[path->root_slot]) {
    return internal_fail(ctx, "model path root is not owned");
  }
  v = ctx->roots[path->root_slot];
  for (i = 0; i < path->len; i++) {
    const struct gc_path_step *st = &path->steps[i];
    if (st->kind == GC_PATH_PROP) {
      char name[GC_MODEL_KEY_NAME_MAX];
      size_t n;
      if (!mjs_is_object(v) || mjs_is_array(v)) {
        return internal_fail(ctx, "model object path crossed a non-object");
      }
      n = gc_model_key_name(st->key, name, sizeof(name));
      if (n == 0) return internal_fail(ctx, "bad model key name");
      v = mjs_get(ctx->mjs, v, name, n);
    } else if (st->kind == GC_PATH_INDEX) {
      if (!mjs_is_array(v)) {
        return internal_fail(ctx, "model array path crossed a non-array");
      }
      v = mjs_array_get(ctx->mjs, v, (unsigned long) st->key);
    } else {
      return internal_fail(ctx, "bad model path step");
    }
  }
  *out = v;
  return 0;
}

static int resolve_node(struct seq_ctx *ctx, struct gc_handle h,
                        mjs_val_t *out) {
  struct gc_path path;
  if (!gc_model_find_path(&ctx->model, h, &path)) {
    return internal_fail(ctx, "no path to a validated node");
  }
  return resolve_path(ctx, &path, out);
}

static int resolve_value(struct seq_ctx *ctx, const struct gc_value_spec *v,
                         mjs_val_t *out) {
  switch (v->kind) {
    case GC_VALUE_NODE:
      return resolve_node(ctx, v->node, out);
    case GC_VALUE_UNDEFINED:
      *out = mjs_mk_undefined();
      return 0;
    case GC_VALUE_NULL:
      *out = mjs_mk_null();
      return 0;
    case GC_VALUE_BOOLEAN:
      *out = mjs_mk_boolean(ctx->mjs, v->boolean);
      return 0;
    case GC_VALUE_NUMBER:
      *out = mjs_mk_number(ctx->mjs, (double) v->number);
      return 0;
    default:
      return internal_fail(ctx, "bad value kind");
  }
}

static int bind_new_root(struct seq_ctx *ctx, uint8_t slot, mjs_val_t value) {
  if (slot >= GC_MODEL_MAX_ROOTS || ctx->root_owned[slot]) {
    return internal_fail(ctx, "destination root is not free");
  }
  ctx->roots[slot] = value;
  ctx->root_owned[slot] = 1;
  mjs_own(ctx->mjs, &ctx->roots[slot]);
  return 0;
}

static int exec_op(struct seq_ctx *ctx, const struct gc_op *op) {
  mjs_val_t target = mjs_mk_undefined();
  mjs_val_t value = mjs_mk_undefined();
  struct gc_handle th;
  int rc;

  switch (op->opcode) {
    case GC_OP_NEW_OBJECT: {
      mjs_val_t v = mjs_mk_object(ctx->mjs);
      if (v == mjs_mk_null() || v == mjs_mk_undefined()) {
        return internal_fail(ctx, "mjs_mk_object failed");
      }
      return bind_new_root(ctx, op->dst, v);
    }
    case GC_OP_NEW_ARRAY: {
      mjs_val_t v = mjs_mk_array(ctx->mjs);
      if (v == mjs_mk_null() || v == mjs_mk_undefined()) {
        return internal_fail(ctx, "mjs_mk_array failed");
      }
      return bind_new_root(ctx, op->dst, v);
    }
    case GC_OP_NEW_STRING: {
      mjs_val_t v = mjs_mk_string(ctx->mjs, (const char *) op->str,
                                  op->str_len, 1);
      return bind_new_root(ctx, op->dst, v);
    }
    case GC_OP_SET_PROP:
      th = op->handle;
      if (resolve_node(ctx, th, &target) != 0) return -1;
      if (!mjs_is_object(target) || mjs_is_array(target)) {
        return internal_fail(ctx, "set prop target is not an object");
      }
      if (resolve_value(ctx, &op->value, &value) != 0) return -1;
      {
        char name[GC_MODEL_KEY_NAME_MAX];
        size_t n = gc_model_key_name(op->dst, name, sizeof(name));
        if (n == 0) return internal_fail(ctx, "bad key id");
        rc = mjs_set(ctx->mjs, target, name, n, value);
        if (rc != MJS_OK) return internal_fail(ctx, "mjs_set failed");
      }
      return 0;
    case GC_OP_DEL_PROP: {
      char name[GC_MODEL_KEY_NAME_MAX];
      size_t n = gc_model_key_name(op->dst, name, sizeof(name));
      if (resolve_node(ctx, op->handle, &target) != 0) return -1;
      if (!mjs_is_object(target) || mjs_is_array(target)) {
        return internal_fail(ctx, "del prop target is not an object");
      }
      if (n == 0) return internal_fail(ctx, "bad key id");
      (void) mjs_del(ctx->mjs, target, name, n);
      return 0;
    }
    case GC_OP_ARRAY_SET:
      if (resolve_node(ctx, op->handle, &target) != 0) return -1;
      if (!mjs_is_array(target)) {
        return internal_fail(ctx, "array set target is not an array");
      }
      if (resolve_value(ctx, &op->value, &value) != 0) return -1;
      rc = mjs_array_set(ctx->mjs, target, (unsigned long) op->dst, value);
      if (rc != MJS_OK) return internal_fail(ctx, "mjs_array_set failed");
      return 0;
    case GC_OP_ARRAY_DEL:
      if (resolve_node(ctx, op->handle, &target) != 0) return -1;
      if (!mjs_is_array(target)) {
        return internal_fail(ctx, "array del target is not an array");
      }
      mjs_array_del(ctx->mjs, target, (unsigned long) op->dst);
      return 0;
    case GC_OP_COPY_ROOT: {
      uint8_t s = op->handle.slot;
      uint8_t d = op->dst;
      if (s >= GC_MODEL_MAX_ROOTS || !ctx->root_owned[s]) {
        return internal_fail(ctx, "copy root source is not owned");
      }
      if (d >= GC_MODEL_MAX_ROOTS) {
        return internal_fail(ctx, "copy root destination out of range");
      }
      if (!ctx->root_owned[d]) {
        if (ctx->roots[d] != mjs_mk_undefined()) {
          return internal_fail(ctx, "free destination root is not undefined");
        }
        ctx->roots[d] = ctx->roots[s];
        ctx->root_owned[d] = 1;
        mjs_own(ctx->mjs, &ctx->roots[d]);
      } else {
        ctx->roots[d] = ctx->roots[s];
      }
      return 0;
    }
    case GC_OP_RELEASE_ROOT: {
      uint8_t s = op->handle.slot;
      if (s >= GC_MODEL_MAX_ROOTS || !ctx->root_owned[s]) {
        return internal_fail(ctx, "release root is not owned");
      }
      if (mjs_disown(ctx->mjs, &ctx->roots[s]) != 1) {
        return internal_fail(ctx, "mjs_disown did not find the root");
      }
      ctx->roots[s] = mjs_mk_undefined();
      ctx->root_owned[s] = 0;
      return 0;
    }
    case GC_OP_GC_FULL:
      mjs_gc(ctx->mjs, 1);
      return 0;
    case GC_OP_GC_MINOR:
      mjs_gc(ctx->mjs, 0);
      return 0;
    default:
      return internal_fail(ctx, "unknown opcode at execution");
  }
}

static int decode_value(const unsigned char *p, size_t avail,
                        struct gc_value_spec *v, size_t *used) {
  if (avail < 1) return -1;
  v->kind = p[0];
  switch (v->kind) {
    case GC_VALUE_NODE:
      if (avail < 4) return -1;
      v->node.slot = p[1];
      v->node.gen = rd_u16(p + 2);
      *used = 4;
      return 0;
    case GC_VALUE_UNDEFINED:
    case GC_VALUE_NULL:
      *used = 1;
      return 0;
    case GC_VALUE_BOOLEAN:
      if (avail < 2) return -1;
      v->boolean = p[1];
      *used = 2;
      return 0;
    case GC_VALUE_NUMBER:
      if (avail < 5) return -1;
      v->number = rd_u32(p + 1);
      *used = 5;
      return 0;
    default:
      return -1;
  }
}

static int decode_record(const unsigned char *data, size_t pos,
                         uint8_t opcode, size_t payload_len, struct gc_op *op) {
  const unsigned char *p = data + pos;
  size_t used = 0;

  memset(op, 0, sizeof(*op));
  op->opcode = opcode;

  switch (opcode) {
    case GC_OP_NEW_OBJECT:
    case GC_OP_NEW_ARRAY:
      if (payload_len != 1) return -1;
      op->dst = p[0];
      return 0;
    case GC_OP_NEW_STRING:
      if (payload_len < 3) return -1;
      op->dst = p[0];
      op->str_len = rd_u16(p + 1);
      if (payload_len != 3 + op->str_len) return -1;
      op->str = p + 3;
      return 0;
    case GC_OP_SET_PROP:
      if (payload_len < 5) return -1;
      op->handle.slot = p[0];
      op->handle.gen = rd_u16(p + 1);
      op->dst = p[3];
      if (decode_value(p + 4, payload_len - 4, &op->value, &used) != 0) {
        return -1;
      }
      if (payload_len != 4 + used) return -1;
      return 0;
    case GC_OP_DEL_PROP:
      if (payload_len != 4) return -1;
      op->handle.slot = p[0];
      op->handle.gen = rd_u16(p + 1);
      op->dst = p[3];
      return 0;
    case GC_OP_ARRAY_SET:
      if (payload_len < 5) return -1;
      op->handle.slot = p[0];
      op->handle.gen = rd_u16(p + 1);
      op->dst = p[3];
      if (decode_value(p + 4, payload_len - 4, &op->value, &used) != 0) {
        return -1;
      }
      if (payload_len != 4 + used) return -1;
      return 0;
    case GC_OP_ARRAY_DEL:
      if (payload_len != 4) return -1;
      op->handle.slot = p[0];
      op->handle.gen = rd_u16(p + 1);
      op->dst = p[3];
      return 0;
    case GC_OP_COPY_ROOT:
      if (payload_len != 4) return -1;
      op->handle.slot = p[0];
      op->handle.gen = rd_u16(p + 1);
      op->dst = p[3];
      return 0;
    case GC_OP_RELEASE_ROOT:
      if (payload_len != 3) return -1;
      op->handle.slot = p[0];
      op->handle.gen = rd_u16(p + 1);
      return 0;
    case GC_OP_GC_FULL:
    case GC_OP_GC_MINOR:
      if (payload_len != 0) return -1;
      return 0;
    default:
      return -1;
  }
}

static void result_init(struct gc_seq_result *r) {
  memset(r, 0, sizeof(*r));
  r->status = GC_SEQ_STATUS_OK;
  r->root_slot = -1;
  r->digest = GC_SEQ_FNV_OFFSET;
}

static void result_invalid(struct gc_seq_result *r, size_t op_index,
                           size_t records_seen, const char *why) {
  r->status = GC_SEQ_STATUS_INVALID_INPUT;
  r->invalid_prefix = 1;
  r->bad_record = op_index;
  r->bad_op = op_index;
  r->records_seen = records_seen;
  r->root_slot = -1;
  r->root_gen = 0;
  r->path[0] = '\0';
  snprintf(r->detail, sizeof(r->detail), "%s", why != NULL ? why : "invalid input");
}

int gc_sequence_run(const unsigned char *data, size_t len,
                    struct gc_seq_result *result) {
  static const unsigned char magic[4] = {'M', 'J', 'S', 'G'};
  struct seq_ctx ctx;
  struct gc_op op;
  size_t pos = 5; /* header size */
  size_t records_seen = 0;

  result_init(result);
  memset(&ctx, 0, sizeof(ctx));
  ctx.res = result;
  result->digest = GC_SEQ_FNV_OFFSET;

  if (len < 5 || memcmp(data, magic, 4) != 0 ||
      data[4] != GC_SEQUENCE_VERSION) {
    result_invalid(result, 0, 0, "bad header or unsupported version");
    return result->status;
  }

  ctx.mjs = mjs_create();
  if (ctx.mjs == NULL) {
    result->status = GC_SEQ_STATUS_INTERNAL_ERROR;
    snprintf(result->detail, sizeof(result->detail), "mjs_create failed");
    return result->status;
  }
  gc_model_init(&ctx.model);
  {
    int i;
    for (i = 0; i < GC_MODEL_MAX_ROOTS; i++) {
      ctx.roots[i] = mjs_mk_undefined();
      ctx.root_owned[i] = 0;
    }
  }

  while (pos < len) {
    uint8_t opcode;
    size_t payload_len;
    const char *why = "malformed record";

    if (len - pos < 3) {
      result_invalid(result, records_seen, records_seen, "truncated record header");
      break;
    }
    opcode = data[pos];
    payload_len = rd_u16(data + pos + 1);
    if (payload_len > GC_SEQUENCE_MAX_RECORD_PAYLOAD) {
      result_invalid(result, records_seen, records_seen, "record payload too large");
      break;
    }
    if (len - pos - 3 < payload_len) {
      result_invalid(result, records_seen, records_seen, "truncated record payload");
      break;
    }
    records_seen++;
    ctx.op_index = records_seen - 1;

    if (decode_record(data, pos + 3, opcode, payload_len, &op) != 0) {
      result_invalid(result, ctx.op_index, records_seen, "malformed record payload");
      break;
    }
    if (!gc_model_validate(&ctx.model, &op, &why)) {
      result_invalid(result, ctx.op_index, records_seen, why);
      break;
    }

    if (exec_op(&ctx, &op) != 0) break;

    gc_model_apply(&ctx.model, &op);

    if (compare_graphs(&ctx) != 0) {
      if (result->status == GC_SEQ_STATUS_OK) {
        result->status = GC_SEQ_STATUS_MISMATCH;
      }
      result->bad_op = ctx.op_index;
      break;
    }

    result->ops_executed++;
    pos += 3 + payload_len;
  }

  /* Release every external root before destroying the interpreter. */
  {
    int i;
    for (i = 0; i < GC_MODEL_MAX_ROOTS; i++) {
      if (ctx.root_owned[i]) {
        (void) mjs_disown(ctx.mjs, &ctx.roots[i]);
        ctx.roots[i] = mjs_mk_undefined();
        ctx.root_owned[i] = 0;
      }
    }
  }
  mjs_destroy(ctx.mjs);
  return result->status;
}

static void print_hex(FILE *fp, const uint8_t *data, size_t len) {
  size_t i;
  for (i = 0; i < len; i++) fprintf(fp, "%02x", (unsigned int) data[i]);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  struct gc_seq_result r;
  int status = gc_sequence_run(data, size, &r);

  if (status == GC_SEQ_STATUS_MISMATCH ||
      status == GC_SEQ_STATUS_INTERNAL_ERROR) {
    fprintf(stderr,
            "MJS_GC_SEQUENCE_MISMATCH op=%zu root=%d rootgen=%u path=%s "
            "detail=%s input=",
            r.bad_op, r.root_slot, r.root_gen, r.path, r.detail);
    print_hex(stderr, data, size);
    fprintf(stderr, "\n");
    fflush(stderr);
    abort();
  }
  return 0;
}
