/*
 * Independent logical graph model for regress/gc_sequence.c.
 * No mJS headers, symbols or observations are used here.
 */

#include <string.h>

#include "gc_model.h"

static int node_handle_live(const struct gc_model *m, struct gc_handle h) {
  if (h.slot >= GC_MODEL_MAX_NODES) return 0;
  if (!m->nodes[h.slot].used) return 0;
  return m->nodes[h.slot].gen == h.gen;
}

static int root_handle_live(const struct gc_model *m, struct gc_handle h) {
  if (h.slot >= GC_MODEL_MAX_ROOTS) return 0;
  if (!m->roots[h.slot].used) return 0;
  return m->roots[h.slot].gen == h.gen;
}

static void mark_reachable(const struct gc_model *m, uint8_t *reach) {
  uint8_t queue[GC_MODEL_MAX_NODES];
  int qh = 0, qt = 0;
  int i, j;

  memset(reach, 0, GC_MODEL_MAX_NODES);
  for (i = 0; i < GC_MODEL_MAX_ROOTS; i++) {
    const struct gc_model_root *r = &m->roots[i];
    if (r->used && node_handle_live(m, r->node) && !reach[r->node.slot]) {
      reach[r->node.slot] = 1;
      queue[qt++] = r->node.slot;
    }
  }

  while (qh < qt) {
    const struct gc_model_node *n = &m->nodes[queue[qh++]];
    if (n->kind == GC_NODE_OBJECT) {
      for (j = 0; j < GC_MODEL_MAX_OBJECT_KEYS; j++) {
        struct gc_handle h;
        if (!n->obj_present[j] || n->obj[j].kind != GC_VALUE_NODE) continue;
        h = n->obj[j].node;
        if (node_handle_live(m, h) && !reach[h.slot]) {
          reach[h.slot] = 1;
          queue[qt++] = h.slot;
        }
      }
    } else if (n->kind == GC_NODE_ARRAY) {
      for (j = 0; j < GC_MODEL_MAX_ARRAY_INDEX; j++) {
        struct gc_handle h;
        if (!n->arr_present[j] || n->arr[j].kind != GC_VALUE_NODE) continue;
        h = n->arr[j].node;
        if (node_handle_live(m, h) && !reach[h.slot]) {
          reach[h.slot] = 1;
          queue[qt++] = h.slot;
        }
      }
    }
  }
}

static int value_spec_valid(const struct gc_model *m,
                            const struct gc_value_spec *v) {
  switch (v->kind) {
    case GC_VALUE_NODE:
      return gc_model_node_reachable(m, v->node);
    case GC_VALUE_UNDEFINED:
    case GC_VALUE_NULL:
      return 1;
    case GC_VALUE_BOOLEAN:
      return v->boolean == 0 || v->boolean == 1;
    case GC_VALUE_NUMBER:
      return 1;
    default:
      return 0;
  }
}

static int find_free_node_slot(const struct gc_model *m) {
  int i;
  for (i = 0; i < GC_MODEL_MAX_NODES; i++) {
    if (!m->nodes[i].used) return i;
  }
  return -1;
}

void gc_model_init(struct gc_model *m) {
  memset(m, 0, sizeof(*m));
}

int gc_model_node_reachable(const struct gc_model *m, struct gc_handle h) {
  uint8_t reach[GC_MODEL_MAX_NODES];
  if (!node_handle_live(m, h)) return 0;
  mark_reachable(m, reach);
  return reach[h.slot] != 0;
}

int gc_model_root_handle(const struct gc_model *m, uint8_t slot,
                         struct gc_handle *out) {
  if (slot >= GC_MODEL_MAX_ROOTS || !m->roots[slot].used) return 0;
  out->slot = slot;
  out->gen = m->roots[slot].gen;
  return 1;
}

int gc_model_validate(const struct gc_model *m, const struct gc_op *op,
                      const char **why) {
  const char *r = "invalid operation";
  int ok = 0;

  switch (op->opcode) {
    case GC_OP_NEW_OBJECT:
    case GC_OP_NEW_ARRAY:
    case GC_OP_NEW_STRING:
      if (op->dst >= GC_MODEL_MAX_ROOTS) {
        r = "new: root slot out of range";
      } else if (m->roots[op->dst].used) {
        r = "new: root slot already occupied";
      } else if (find_free_node_slot(m) < 0) {
        r = "new: node limit reached";
      } else if (op->opcode == GC_OP_NEW_STRING &&
                 op->str_len > GC_MODEL_MAX_STRING) {
        r = "new string: length limit exceeded";
      } else {
        ok = 1;
      }
      break;

    case GC_OP_SET_PROP:
      if (!gc_model_node_reachable(m, op->handle)) {
        r = "set prop: target handle is not reachable";
      } else if (m->nodes[op->handle.slot].kind != GC_NODE_OBJECT) {
        r = "set prop: target is not an object";
      } else if (op->dst >= GC_MODEL_MAX_OBJECT_KEYS) {
        r = "set prop: key id out of range";
      } else if (!value_spec_valid(m, &op->value)) {
        r = "set prop: value handle is not reachable";
      } else {
        ok = 1;
      }
      break;

    case GC_OP_DEL_PROP:
      if (!gc_model_node_reachable(m, op->handle)) {
        r = "del prop: target handle is not reachable";
      } else if (m->nodes[op->handle.slot].kind != GC_NODE_OBJECT) {
        r = "del prop: target is not an object";
      } else if (op->dst >= GC_MODEL_MAX_OBJECT_KEYS) {
        r = "del prop: key id out of range";
      } else {
        ok = 1;
      }
      break;

    case GC_OP_ARRAY_SET:
      if (!gc_model_node_reachable(m, op->handle)) {
        r = "array set: target handle is not reachable";
      } else if (m->nodes[op->handle.slot].kind != GC_NODE_ARRAY) {
        r = "array set: target is not an array";
      } else if (op->dst >= GC_MODEL_MAX_ARRAY_INDEX) {
        r = "array set: index out of range";
      } else if (!value_spec_valid(m, &op->value)) {
        r = "array set: value handle is not reachable";
      } else {
        ok = 1;
      }
      break;

    case GC_OP_ARRAY_DEL:
      if (!gc_model_node_reachable(m, op->handle)) {
        r = "array del: target handle is not reachable";
      } else if (m->nodes[op->handle.slot].kind != GC_NODE_ARRAY) {
        r = "array del: target is not an array";
      } else if (op->dst >= GC_MODEL_MAX_ARRAY_INDEX) {
        r = "array del: index out of range";
      } else {
        ok = 1;
      }
      break;

    case GC_OP_COPY_ROOT:
      if (op->dst >= GC_MODEL_MAX_ROOTS) {
        r = "copy root: destination slot out of range";
      } else if (!root_handle_live(m, op->handle)) {
        r = "copy root: source generation is stale";
      } else {
        ok = 1;
      }
      break;

    case GC_OP_RELEASE_ROOT:
      if (!root_handle_live(m, op->handle)) {
        r = "release root: handle is stale or free";
      } else {
        ok = 1;
      }
      break;

    case GC_OP_GC_FULL:
    case GC_OP_GC_MINOR:
      ok = 1;
      break;

    default:
      r = "unknown opcode";
      break;
  }

  if (!ok && why != NULL) *why = r;
  return ok;
}

void gc_model_collect(struct gc_model *m) {
  uint8_t reach[GC_MODEL_MAX_NODES];
  int i;
  mark_reachable(m, reach);
  for (i = 0; i < GC_MODEL_MAX_NODES; i++) {
    if (m->nodes[i].used && !reach[i]) {
      m->nodes[i].used = 0;
    }
  }
}

void gc_model_apply(struct gc_model *m, const struct gc_op *op) {
  int slot;

  switch (op->opcode) {
    case GC_OP_NEW_OBJECT:
    case GC_OP_NEW_ARRAY:
    case GC_OP_NEW_STRING: {
      struct gc_model_node *n;
      slot = find_free_node_slot(m);
      if (slot < 0) break; /* validation guarantees this cannot happen */
      n = &m->nodes[slot];
      {
        uint16_t gen = (uint16_t) (n->gen + 1);
        memset(n, 0, sizeof(*n));
        n->gen = gen;
      }
      n->used = 1;
      if (op->opcode == GC_OP_NEW_OBJECT) {
        n->kind = GC_NODE_OBJECT;
      } else if (op->opcode == GC_OP_NEW_ARRAY) {
        n->kind = GC_NODE_ARRAY;
      } else {
        n->kind = GC_NODE_STRING;
        n->str_len = (uint16_t) op->str_len;
        if (op->str_len != 0) memcpy(n->str, op->str, op->str_len);
      }
      m->roots[op->dst].used = 1;
      m->roots[op->dst].gen = (uint16_t) (m->roots[op->dst].gen + 1);
      m->roots[op->dst].node.slot = (uint8_t) slot;
      m->roots[op->dst].node.gen = n->gen;
      break;
    }

    case GC_OP_SET_PROP:
      m->nodes[op->handle.slot].obj_present[op->dst] = 1;
      m->nodes[op->handle.slot].obj[op->dst] = op->value;
      break;

    case GC_OP_DEL_PROP:
      m->nodes[op->handle.slot].obj_present[op->dst] = 0;
      memset(&m->nodes[op->handle.slot].obj[op->dst], 0,
             sizeof(struct gc_value_spec));
      break;

    case GC_OP_ARRAY_SET:
      m->nodes[op->handle.slot].arr_present[op->dst] = 1;
      m->nodes[op->handle.slot].arr[op->dst] = op->value;
      break;

    case GC_OP_ARRAY_DEL:
      m->nodes[op->handle.slot].arr_present[op->dst] = 0;
      memset(&m->nodes[op->handle.slot].arr[op->dst], 0,
             sizeof(struct gc_value_spec));
      break;

    case GC_OP_COPY_ROOT:
      m->roots[op->dst].node = m->roots[op->handle.slot].node;
      m->roots[op->dst].used = 1;
      m->roots[op->dst].gen = (uint16_t) (m->roots[op->dst].gen + 1);
      break;

    case GC_OP_RELEASE_ROOT:
      m->roots[op->handle.slot].used = 0;
      break;

    case GC_OP_GC_FULL:
    case GC_OP_GC_MINOR:
      break;
  }

  gc_model_collect(m);
}

unsigned long gc_model_array_length(const struct gc_model *m, uint8_t slot) {
  unsigned long len = 0;
  int i;
  if (slot >= GC_MODEL_MAX_NODES || !m->nodes[slot].used ||
      m->nodes[slot].kind != GC_NODE_ARRAY) {
    return 0;
  }
  for (i = 0; i < GC_MODEL_MAX_ARRAY_INDEX; i++) {
    if (m->nodes[slot].arr_present[i] &&
        (unsigned long) (i + 1) > len) {
      len = (unsigned long) (i + 1);
    }
  }
  return len;
}

int gc_model_find_path(const struct gc_model *m, struct gc_handle h,
                       struct gc_path *out) {
  int prev[GC_MODEL_MAX_NODES];
  int origin[GC_MODEL_MAX_NODES];
  uint8_t step_kind[GC_MODEL_MAX_NODES];
  uint8_t step_key[GC_MODEL_MAX_NODES];
  uint8_t seen[GC_MODEL_MAX_NODES];
  uint8_t queue[GC_MODEL_MAX_NODES];
  struct gc_path_step rev[GC_MODEL_MAX_PATH];
  int qh = 0, qt = 0, i, j, n = 0, cur;
  uint16_t root_gen = 0;

  if (!node_handle_live(m, h)) return 0;

  memset(seen, 0, sizeof(seen));
  for (i = 0; i < GC_MODEL_MAX_NODES; i++) prev[i] = -1;

  for (i = 0; i < GC_MODEL_MAX_ROOTS; i++) {
    const struct gc_model_root *r = &m->roots[i];
    if (r->used && node_handle_live(m, r->node) && !seen[r->node.slot]) {
      seen[r->node.slot] = 1;
      origin[r->node.slot] = i;
      prev[r->node.slot] = -1;
      step_kind[r->node.slot] = 0;
      step_key[r->node.slot] = 0;
      queue[qt++] = r->node.slot;
    }
  }

  while (qh < qt) {
    int u = queue[qh++];
    const struct gc_model_node *node = &m->nodes[u];
    if (node->kind == GC_NODE_OBJECT) {
      for (j = 0; j < GC_MODEL_MAX_OBJECT_KEYS; j++) {
        struct gc_handle v;
        if (!node->obj_present[j] || node->obj[j].kind != GC_VALUE_NODE) {
          continue;
        }
        v = node->obj[j].node;
        if (!node_handle_live(m, v) || seen[v.slot]) continue;
        seen[v.slot] = 1;
        prev[v.slot] = u;
        origin[v.slot] = origin[u];
        step_kind[v.slot] = GC_PATH_PROP;
        step_key[v.slot] = (uint8_t) j;
        queue[qt++] = v.slot;
        if (v.slot == h.slot) break;
      }
    } else if (node->kind == GC_NODE_ARRAY) {
      for (j = 0; j < GC_MODEL_MAX_ARRAY_INDEX; j++) {
        struct gc_handle v;
        if (!node->arr_present[j] || node->arr[j].kind != GC_VALUE_NODE) {
          continue;
        }
        v = node->arr[j].node;
        if (!node_handle_live(m, v) || seen[v.slot]) continue;
        seen[v.slot] = 1;
        prev[v.slot] = u;
        origin[v.slot] = origin[u];
        step_kind[v.slot] = GC_PATH_INDEX;
        step_key[v.slot] = (uint8_t) j;
        queue[qt++] = v.slot;
        if (v.slot == h.slot) break;
      }
    }
    if (seen[h.slot]) break;
  }

  if (!seen[h.slot]) return 0;

  cur = h.slot;
  while (prev[cur] != -1 && n < GC_MODEL_MAX_PATH) {
    rev[n].kind = step_kind[cur];
    rev[n].key = step_key[cur];
    n++;
    cur = prev[cur];
  }
  if (prev[cur] != -1) return 0; /* path too long (cannot happen with 64 nodes) */

  out->root_slot = (uint8_t) origin[h.slot];
  root_gen = m->roots[out->root_slot].gen;
  out->root_gen = root_gen;
  out->len = (uint8_t) n;
  for (i = 0; i < n; i++) out->steps[i] = rev[n - 1 - i];
  return 1;
}

int gc_model_reachable_nodes(const struct gc_model *m, struct gc_handle *out,
                             int max) {
  uint8_t seen[GC_MODEL_MAX_NODES];
  uint8_t queue[GC_MODEL_MAX_NODES];
  int qh = 0, qt = 0, n = 0, i, j;

  memset(seen, 0, sizeof(seen));
  for (i = 0; i < GC_MODEL_MAX_ROOTS; i++) {
    const struct gc_model_root *r = &m->roots[i];
    if (r->used && node_handle_live(m, r->node) && !seen[r->node.slot]) {
      seen[r->node.slot] = 1;
      queue[qt++] = r->node.slot;
    }
  }

  while (qh < qt) {
    int u = queue[qh++];
    const struct gc_model_node *node = &m->nodes[u];
    if (n < max) {
      out[n].slot = (uint8_t) u;
      out[n].gen = node->gen;
      n++;
    }
    if (node->kind == GC_NODE_OBJECT) {
      for (j = 0; j < GC_MODEL_MAX_OBJECT_KEYS; j++) {
        struct gc_handle v;
        if (!node->obj_present[j] || node->obj[j].kind != GC_VALUE_NODE) {
          continue;
        }
        v = node->obj[j].node;
        if (node_handle_live(m, v) && !seen[v.slot]) {
          seen[v.slot] = 1;
          queue[qt++] = v.slot;
        }
      }
    } else if (node->kind == GC_NODE_ARRAY) {
      for (j = 0; j < GC_MODEL_MAX_ARRAY_INDEX; j++) {
        struct gc_handle v;
        if (!node->arr_present[j] || node->arr[j].kind != GC_VALUE_NODE) {
          continue;
        }
        v = node->arr[j].node;
        if (node_handle_live(m, v) && !seen[v.slot]) {
          seen[v.slot] = 1;
          queue[qt++] = v.slot;
        }
      }
    }
  }
  return n;
}

size_t gc_model_key_name(uint8_t key_id, char *buf, size_t buf_size) {
  char tmp[8];
  size_t n = 0, i;
  int v;

  if (key_id >= GC_MODEL_MAX_OBJECT_KEYS || buf_size == 0) return 0;
  tmp[n++] = 'k';
  v = key_id;
  if (v >= 10) {
    tmp[n++] = (char) ('0' + (v / 10));
    v %= 10;
  }
  tmp[n++] = (char) ('0' + v);
  if (n + 1 > buf_size) return 0;
  for (i = 0; i < n; i++) buf[i] = tmp[i];
  buf[n] = '\0';
  return n;
}
