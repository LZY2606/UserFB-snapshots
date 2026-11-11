/*
 * Independent logical model for the mJS GC sequence regression entry point.
 *
 * This file deliberately does not include or call any mJS API.  The real
 * interpreter and this model are both driven by the same decoded operation
 * sequence; the model is never updated from observations of mJS.
 *
 * Handles are (slot, generation) pairs.  Generations are bumped whenever a
 * root slot is bound to a value and whenever a node slot is reused, so an
 * operation carrying an old generation is rejected instead of silently
 * addressing a new object.
 */

#ifndef GC_MODEL_H_
#define GC_MODEL_H_

#include <stddef.h>
#include <stdint.h>

#define GC_MODEL_MAX_ROOTS 16
#define GC_MODEL_MAX_NODES 64
#define GC_MODEL_MAX_STRING 256
#define GC_MODEL_MAX_OBJECT_KEYS 16
#define GC_MODEL_MAX_ARRAY_INDEX 64
#define GC_MODEL_MAX_PATH 64
#define GC_MODEL_KEY_NAME_MAX 16

enum gc_node_kind {
  GC_NODE_OBJECT = 1,
  GC_NODE_ARRAY = 2,
  GC_NODE_STRING = 3
};

enum gc_value_kind {
  GC_VALUE_NODE = 0,
  GC_VALUE_UNDEFINED = 1,
  GC_VALUE_NULL = 2,
  GC_VALUE_BOOLEAN = 3,
  GC_VALUE_NUMBER = 4
};

enum gc_opcode {
  GC_OP_NEW_OBJECT = 1,
  GC_OP_NEW_ARRAY = 2,
  GC_OP_NEW_STRING = 3,
  GC_OP_SET_PROP = 4,
  GC_OP_DEL_PROP = 5,
  GC_OP_ARRAY_SET = 6,
  GC_OP_ARRAY_DEL = 7,
  GC_OP_COPY_ROOT = 8,
  GC_OP_RELEASE_ROOT = 9,
  GC_OP_GC_FULL = 10,
  GC_OP_GC_MINOR = 11
};

struct gc_handle {
  uint8_t slot;
  uint16_t gen;
};

struct gc_value_spec {
  uint8_t kind; /* enum gc_value_kind */
  struct gc_handle node;
  int boolean;
  uint32_t number;
};

struct gc_op {
  uint8_t opcode; /* enum gc_opcode */
  uint8_t dst;    /* destination root slot / object key id / array index */
  struct gc_handle handle; /* object/array/source-root handle */
  struct gc_value_spec value;
  const unsigned char *str; /* GC_OP_NEW_STRING payload, not NUL terminated */
  size_t str_len;
};

struct gc_model_node {
  uint8_t used;
  uint8_t kind; /* enum gc_node_kind */
  uint16_t gen;
  uint16_t str_len;
  unsigned char str[GC_MODEL_MAX_STRING];
  uint8_t obj_present[GC_MODEL_MAX_OBJECT_KEYS];
  struct gc_value_spec obj[GC_MODEL_MAX_OBJECT_KEYS];
  uint8_t arr_present[GC_MODEL_MAX_ARRAY_INDEX];
  struct gc_value_spec arr[GC_MODEL_MAX_ARRAY_INDEX];
};

struct gc_model_root {
  uint8_t used;
  uint16_t gen;
  struct gc_handle node;
};

struct gc_model {
  struct gc_model_node nodes[GC_MODEL_MAX_NODES];
  struct gc_model_root roots[GC_MODEL_MAX_ROOTS];
};

enum gc_path_step_kind {
  GC_PATH_PROP = 1,
  GC_PATH_INDEX = 2
};

struct gc_path_step {
  uint8_t kind; /* enum gc_path_step_kind */
  uint8_t key;  /* object key id, or array index */
};

struct gc_path {
  uint8_t root_slot;
  uint16_t root_gen;
  uint8_t len;
  struct gc_path_step steps[GC_MODEL_MAX_PATH];
};

void gc_model_init(struct gc_model *m);

/*
 * Validate an operation against the model without changing it.  Returns 0
 * when the operation is legal in the current state.  On failure stores a
 * static human-readable reason in *why (when why is not NULL).
 */
int gc_model_validate(const struct gc_model *m, const struct gc_op *op,
                      const char **why);

/* Apply a previously validated operation and collect unreachable nodes. */
void gc_model_apply(struct gc_model *m, const struct gc_op *op);

/* Free every node that is not reachable from an occupied external root. */
void gc_model_collect(struct gc_model *m);

/* A node handle is usable only when it is live, current and reachable. */
int gc_model_node_reachable(const struct gc_model *m, struct gc_handle h);

/* Current handle of an occupied external root, or 0 when it is free. */
int gc_model_root_handle(const struct gc_model *m, uint8_t slot,
                         struct gc_handle *out);

/*
 * Find a deterministic path from an external root to a reachable node.  The
 * path is suitable for re-reading the value from the real interpreter: every
 * step is an own property of an object or array, so no stale mjs_val_t needs
 * to survive an allocation or a GC.
 */
int gc_model_find_path(const struct gc_model *m, struct gc_handle h,
                       struct gc_path *out);

unsigned long gc_model_array_length(const struct gc_model *m, uint8_t slot);

/* Enumerate reachable nodes in root-then-BFS order (used by the generator). */
int gc_model_reachable_nodes(const struct gc_model *m, struct gc_handle *out,
                             int max);

/* Canonical property name for an object key id: "k0" .. "k15". */
size_t gc_model_key_name(uint8_t key_id, char *buf, size_t buf_size);

#endif /* GC_MODEL_H_ */
