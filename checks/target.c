#include "cases.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "nanoarrow/nanoarrow.h"

#define SETUP_OK(expr)                                                \
  do {                                                                \
    if ((expr) != NANOARROW_OK) {                                     \
      fprintf(stderr, "setup failed %s:%d: %s\n", __FILE__, __LINE__, \
              #expr);                                                 \
      return 1;                                                       \
    }                                                                 \
  } while (0)

static ArrowErrorCode InitDictSchema(struct ArrowSchema* schema,
                                     enum ArrowType index_type,
                                     enum ArrowType value_type) {
  ArrowErrorCode result = ArrowSchemaInitFromType(schema, index_type);
  if (result != NANOARROW_OK) {
    return result;
  }
  result = ArrowSchemaAllocateDictionary(schema);
  if (result != NANOARROW_OK) {
    return result;
  }
  return ArrowSchemaInitFromType(schema->dictionary, value_type);
}

// Compare dictionary value i (which must not be null) against expected bytes.
static int DictValueEquals(struct ArrowArrayView* dict_view, int64_t i,
                           const char* data, int64_t n) {
  if (ArrowArrayViewIsNull(dict_view, i)) {
    return 0;
  }
  struct ArrowStringView value = ArrowArrayViewGetStringUnsafe(dict_view, i);
  return value.size_bytes == n && memcmp(value.data, data, (size_t)n) == 0;
}

// Resolve the dictionary-encoded value of field_view at row and compare it
// against a null-terminated expected string.
static int FieldValueEquals(struct ArrowArrayView* field_view, int64_t row,
                            const char* expect) {
  if (ArrowArrayViewIsNull(field_view, row)) {
    return 0;
  }
  int64_t index = ArrowArrayViewGetIntUnsafe(field_view, row);
  return DictValueEquals(field_view->dictionary, index, expect,
                         (int64_t)strlen(expect));
}

// Two dictionary-encoded fields whose source dictionaries order the same
// values differently must be unified into independent index spaces.
static int TestDictionaryOrder(void) {
  struct ArrowError error;
  struct ArrowSchema schema;
  SETUP_OK(ArrowSchemaInitFromType(&schema, NANOARROW_TYPE_STRUCT));
  SETUP_OK(ArrowSchemaAllocateChildren(&schema, 2));
  SETUP_OK(InitDictSchema(schema.children[0], NANOARROW_TYPE_INT8,
                          NANOARROW_TYPE_STRING));
  SETUP_OK(InitDictSchema(schema.children[1], NANOARROW_TYPE_INT8,
                          NANOARROW_TYPE_STRING));

  struct ArrowArray src;
  SETUP_OK(ArrowArrayInitFromSchema(&src, &schema, &error));
  SETUP_OK(ArrowArrayStartAppending(&src));
  // Field a dictionary: red=0, blue=1
  SETUP_OK(ArrowArrayAppendString(src.children[0]->dictionary,
                                  ArrowCharView("red")));
  SETUP_OK(ArrowArrayAppendString(src.children[0]->dictionary,
                                  ArrowCharView("blue")));
  // Field b dictionary: blue=0, red=1 (opposite order)
  SETUP_OK(ArrowArrayAppendString(src.children[1]->dictionary,
                                  ArrowCharView("blue")));
  SETUP_OK(ArrowArrayAppendString(src.children[1]->dictionary,
                                  ArrowCharView("red")));
  const int64_t a_indices[3] = {0, 1, 1};
  const int64_t b_indices[3] = {0, 1, 1};
  for (int i = 0; i < 3; i++) {
    SETUP_OK(ArrowArrayAppendInt(src.children[0], a_indices[i]));
    SETUP_OK(ArrowArrayAppendInt(src.children[1], b_indices[i]));
    SETUP_OK(ArrowArrayFinishElement(&src));
  }
  SETUP_OK(ArrowArrayFinishBuildingDefault(&src, &error));

  struct ArrowArrayView src_view;
  SETUP_OK(ArrowArrayViewInitFromSchema(&src_view, &schema, &error));
  SETUP_OK(ArrowArrayViewSetArray(&src_view, &src, &error));

  struct ArrowArray dst;
  SETUP_OK(ArrowArrayInitFromSchema(&dst, &schema, &error));
  SETUP_OK(ArrowArrayStartAppending(&dst));
  // Pre-existing destination values must keep their indices.
  SETUP_OK(ArrowArrayAppendString(dst.children[0]->dictionary,
                                  ArrowCharView("keep")));
  SETUP_OK(ArrowArrayAppendString(dst.children[1]->dictionary,
                                  ArrowCharView("keep")));

  SETUP_OK(ArrowArrayAppendUnifiedDictionary(&dst, &src_view, &schema, &error));
  // Appending the same batch again grows rows but not dictionaries.
  SETUP_OK(ArrowArrayAppendUnifiedDictionary(&dst, &src_view, &schema, &error));

  // The destination must remain readable after the source is released.
  ArrowArrayViewReset(&src_view);
  src.release(&src);

  SETUP_OK(ArrowArrayFinishBuilding(&dst, NANOARROW_VALIDATION_LEVEL_FULL,
                                    &error));
  TASK_CHECK(dst.length == 6);
  TASK_CHECK(dst.children[0]->dictionary->length == 3);
  TASK_CHECK(dst.children[1]->dictionary->length == 3);

  struct ArrowArrayView out;
  SETUP_OK(ArrowArrayViewInitFromSchema(&out, &schema, &error));
  SETUP_OK(ArrowArrayViewSetArray(&out, &dst, &error));

  // Existing values keep their indices; new values appear in
  // first-encountered order, per field.
  TASK_CHECK(DictValueEquals(out.children[0]->dictionary, 0, "keep", 4));
  TASK_CHECK(DictValueEquals(out.children[0]->dictionary, 1, "red", 3));
  TASK_CHECK(DictValueEquals(out.children[0]->dictionary, 2, "blue", 4));
  TASK_CHECK(DictValueEquals(out.children[1]->dictionary, 0, "keep", 4));
  TASK_CHECK(DictValueEquals(out.children[1]->dictionary, 1, "blue", 4));
  TASK_CHECK(DictValueEquals(out.children[1]->dictionary, 2, "red", 3));

  // The same source index refers to different values in each field, and the
  // unified dictionaries reflect each field's own first-encountered order.
  TASK_CHECK(ArrowArrayViewGetIntUnsafe(out.children[0], 0) == 1);
  TASK_CHECK(ArrowArrayViewGetIntUnsafe(out.children[1], 0) == 1);
  TASK_CHECK(DictValueEquals(out.children[0]->dictionary, 1, "red", 3));
  TASK_CHECK(DictValueEquals(out.children[1]->dictionary, 1, "blue", 4));

  const char* a_expect[6] = {"red", "blue", "blue", "red", "blue", "blue"};
  const char* b_expect[6] = {"blue", "red", "red", "blue", "red", "red"};
  for (int i = 0; i < 6; i++) {
    TASK_CHECK(FieldValueEquals(out.children[0], i, a_expect[i]));
    TASK_CHECK(FieldValueEquals(out.children[1], i, b_expect[i]));
  }

  ArrowArrayViewReset(&out);
  dst.release(&dst);
  schema.release(&schema);
  return 0;
}

// Null parents, null indices, null dictionary values, embedded zero bytes,
// and a non-zero source offset, all beneath nested containers.
static int TestNestedSlices(void) {
  struct ArrowError error;
  struct ArrowSchema schema;
  SETUP_OK(ArrowSchemaInitFromType(&schema, NANOARROW_TYPE_STRUCT));
  SETUP_OK(ArrowSchemaAllocateChildren(&schema, 2));
  // l: list<dictionary(int16, utf8)>
  SETUP_OK(ArrowSchemaInitFromType(schema.children[0], NANOARROW_TYPE_LIST));
  SETUP_OK(InitDictSchema(schema.children[0]->children[0], NANOARROW_TYPE_INT16,
                          NANOARROW_TYPE_STRING));
  // f: fixed_size_list<dictionary(int32, utf8), 2>
  ArrowSchemaInit(schema.children[1]);
  SETUP_OK(ArrowSchemaSetTypeFixedSize(schema.children[1],
                                       NANOARROW_TYPE_FIXED_SIZE_LIST, 2));
  SETUP_OK(InitDictSchema(schema.children[1]->children[0], NANOARROW_TYPE_INT32,
                          NANOARROW_TYPE_STRING));

  struct ArrowArray src;
  SETUP_OK(ArrowArrayInitFromSchema(&src, &schema, &error));
  SETUP_OK(ArrowArrayStartAppending(&src));

  struct ArrowArray* l = src.children[0];
  struct ArrowArray* l_index = l->children[0];
  struct ArrowArray* l_dict = l_index->dictionary;
  SETUP_OK(ArrowArrayAppendString(l_dict, ArrowCharView("a")));
  SETUP_OK(ArrowArrayAppendString(l_dict, ArrowCharView("b")));
  SETUP_OK(ArrowArrayAppendString(l_dict, ArrowCharView("c")));
  struct ArrowStringView embedded_zero;
  embedded_zero.data = "q\0q";
  embedded_zero.size_bytes = 3;
  SETUP_OK(ArrowArrayAppendString(l_dict, embedded_zero));
  SETUP_OK(ArrowArrayAppendString(l_dict, ArrowCharView("unused")));

  struct ArrowArray* f = src.children[1];
  struct ArrowArray* f_index = f->children[0];
  struct ArrowArray* f_dict = f_index->dictionary;
  SETUP_OK(ArrowArrayAppendString(f_dict, ArrowCharView("x")));
  // A null dictionary value is a mergeable value, distinct from a null index.
  SETUP_OK(ArrowArrayAppendNull(f_dict, 1));
  SETUP_OK(ArrowArrayAppendString(f_dict, ArrowCharView("y")));
  SETUP_OK(ArrowArrayAppendString(f_dict, ArrowCharView("z")));

  // row 0: l = ["a", "b"], f = ["x", "y"]
  SETUP_OK(ArrowArrayAppendInt(l_index, 0));
  SETUP_OK(ArrowArrayAppendInt(l_index, 1));
  SETUP_OK(ArrowArrayFinishElement(l));
  SETUP_OK(ArrowArrayAppendInt(f_index, 0));
  SETUP_OK(ArrowArrayAppendInt(f_index, 2));
  SETUP_OK(ArrowArrayFinishElement(f));
  SETUP_OK(ArrowArrayFinishElement(&src));
  // row 1: l = null, f = ["y", null dictionary value]
  SETUP_OK(ArrowArrayAppendNull(l, 1));
  SETUP_OK(ArrowArrayAppendInt(f_index, 2));
  SETUP_OK(ArrowArrayAppendInt(f_index, 1));
  SETUP_OK(ArrowArrayFinishElement(f));
  SETUP_OK(ArrowArrayFinishElement(&src));
  // row 2: l = ["b", null, "q\0q"], f = null
  SETUP_OK(ArrowArrayAppendInt(l_index, 1));
  SETUP_OK(ArrowArrayAppendNull(l_index, 1));
  SETUP_OK(ArrowArrayAppendInt(l_index, 3));
  SETUP_OK(ArrowArrayFinishElement(l));
  SETUP_OK(ArrowArrayAppendNull(f, 1));
  SETUP_OK(ArrowArrayFinishElement(&src));
  SETUP_OK(ArrowArrayFinishBuildingDefault(&src, &error));

  struct ArrowArrayView src_view;
  SETUP_OK(ArrowArrayViewInitFromSchema(&src_view, &schema, &error));
  SETUP_OK(ArrowArrayViewSetArray(&src_view, &src, &error));
  // Select only rows 1 and 2: row 0 and its dictionary values are excluded.
  src_view.offset = 1;
  src_view.length = 2;

  struct ArrowArray dst;
  SETUP_OK(ArrowArrayInitFromSchema(&dst, &schema, &error));
  SETUP_OK(ArrowArrayStartAppending(&dst));
  SETUP_OK(ArrowArrayAppendUnifiedDictionary(&dst, &src_view, &schema, &error));

  ArrowArrayViewReset(&src_view);
  src.release(&src);

  SETUP_OK(ArrowArrayFinishBuilding(&dst, NANOARROW_VALIDATION_LEVEL_FULL,
                                    &error));
  TASK_CHECK(dst.length == 2);
  TASK_CHECK(dst.null_count == 0);

  struct ArrowArrayView out;
  SETUP_OK(ArrowArrayViewInitFromSchema(&out, &schema, &error));
  SETUP_OK(ArrowArrayViewSetArray(&out, &dst, &error));

  struct ArrowArrayView* out_l = out.children[0];
  struct ArrowArrayView* out_l_index = out_l->children[0];
  struct ArrowArrayView* out_l_dict = out_l_index->dictionary;
  // The null list parent introduces no hidden elements.
  TASK_CHECK(ArrowArrayViewIsNull(out_l, 0) == 1);
  TASK_CHECK(ArrowArrayViewIsNull(out_l, 1) == 0);
  TASK_CHECK(out_l_index->length == 3);
  TASK_CHECK(ArrowArrayViewListChildOffset(out_l, 1) == 0);
  TASK_CHECK(ArrowArrayViewListChildOffset(out_l, 2) == 3);
  // Only "b" and "q\0q" are reachable from the selected rows.
  TASK_CHECK(out_l_dict->length == 2);
  TASK_CHECK(DictValueEquals(out_l_dict, 0, "b", 1));
  struct ArrowStringView embedded =
      ArrowArrayViewGetStringUnsafe(out_l_dict, 1);
  TASK_CHECK(embedded.size_bytes == 3);
  TASK_CHECK(memcmp(embedded.data, "q\0q", 3) == 0);
  // Element values of the valid list row.
  TASK_CHECK(ArrowArrayViewGetIntUnsafe(out_l_index, 0) == 0);
  TASK_CHECK(ArrowArrayViewIsNull(out_l_index, 1) == 1);
  TASK_CHECK(ArrowArrayViewGetIntUnsafe(out_l_index, 2) == 1);

  struct ArrowArrayView* out_f = out.children[1];
  struct ArrowArrayView* out_f_index = out_f->children[0];
  struct ArrowArrayView* out_f_dict = out_f_index->dictionary;
  TASK_CHECK(ArrowArrayViewIsNull(out_f, 0) == 0);
  TASK_CHECK(ArrowArrayViewIsNull(out_f, 1) == 1);
  // A null fixed-size list row still occupies child slots, as null indices.
  TASK_CHECK(out_f_index->length == 4);
  TASK_CHECK(ArrowArrayViewIsNull(out_f_index, 2) == 1);
  TASK_CHECK(ArrowArrayViewIsNull(out_f_index, 3) == 1);
  // "x" is only referenced by the sliced-out row and the null parent row;
  // "z" is never referenced. Only "y" and the null dictionary value remain.
  TASK_CHECK(out_f_dict->length == 2);
  TASK_CHECK(DictValueEquals(out_f_dict, 0, "y", 1));
  TASK_CHECK(ArrowArrayViewIsNull(out_f_dict, 1) == 1);
  TASK_CHECK(ArrowArrayViewGetIntUnsafe(out_f_index, 0) == 0);
  TASK_CHECK(ArrowArrayViewGetIntUnsafe(out_f_index, 1) == 1);

  ArrowArrayViewReset(&out);
  dst.release(&dst);
  schema.release(&schema);
  return 0;
}

// int8 index capacity: success exactly at the boundary, EOVERFLOW beyond it
// with the destination left untouched, and EINVAL for invalid indices.
static int TestIndexOverflow(void) {
  struct ArrowError error;
  char name[16];

  // Part A: 126 existing values + 2 new values exactly fill int8 indices.
  struct ArrowSchema schema;
  SETUP_OK(InitDictSchema(&schema, NANOARROW_TYPE_INT8, NANOARROW_TYPE_STRING));

  struct ArrowArray dst;
  SETUP_OK(ArrowArrayInitFromSchema(&dst, &schema, &error));
  SETUP_OK(ArrowArrayStartAppending(&dst));
  for (int i = 0; i < 126; i++) {
    snprintf(name, sizeof(name), "v%d", i);
    SETUP_OK(ArrowArrayAppendString(dst.dictionary, ArrowCharView(name)));
  }

  struct ArrowArray src;
  SETUP_OK(ArrowArrayInitFromSchema(&src, &schema, &error));
  SETUP_OK(ArrowArrayStartAppending(&src));
  SETUP_OK(ArrowArrayAppendString(src.dictionary, ArrowCharView("n0")));
  SETUP_OK(ArrowArrayAppendString(src.dictionary, ArrowCharView("n1")));
  SETUP_OK(ArrowArrayAppendInt(&src, 0));
  SETUP_OK(ArrowArrayAppendInt(&src, 1));
  SETUP_OK(ArrowArrayAppendInt(&src, 0));
  SETUP_OK(ArrowArrayFinishBuildingDefault(&src, &error));

  struct ArrowArrayView src_view;
  SETUP_OK(ArrowArrayViewInitFromSchema(&src_view, &schema, &error));
  SETUP_OK(ArrowArrayViewSetArray(&src_view, &src, &error));

  SETUP_OK(ArrowArrayAppendUnifiedDictionary(&dst, &src_view, &schema, &error));
  TASK_CHECK(dst.length == 3);
  TASK_CHECK(dst.dictionary->length == 128);

  ArrowArrayViewReset(&src_view);
  src.release(&src);

  SETUP_OK(ArrowArrayFinishBuilding(&dst, NANOARROW_VALIDATION_LEVEL_FULL,
                                    &error));
  struct ArrowArrayView out;
  SETUP_OK(ArrowArrayViewInitFromSchema(&out, &schema, &error));
  SETUP_OK(ArrowArrayViewSetArray(&out, &dst, &error));
  TASK_CHECK(ArrowArrayViewGetIntUnsafe(&out, 0) == 126);
  TASK_CHECK(ArrowArrayViewGetIntUnsafe(&out, 1) == 127);
  TASK_CHECK(ArrowArrayViewGetIntUnsafe(&out, 2) == 126);
  TASK_CHECK(DictValueEquals(out.dictionary, 0, "v0", 2));
  TASK_CHECK(DictValueEquals(out.dictionary, 126, "n0", 2));
  TASK_CHECK(DictValueEquals(out.dictionary, 127, "n1", 2));
  ArrowArrayViewReset(&out);
  dst.release(&dst);
  schema.release(&schema);

  // Part B: overflow in a later field must not leave earlier fields appended.
  struct ArrowSchema schema2;
  SETUP_OK(ArrowSchemaInitFromType(&schema2, NANOARROW_TYPE_STRUCT));
  SETUP_OK(ArrowSchemaAllocateChildren(&schema2, 2));
  SETUP_OK(InitDictSchema(schema2.children[0], NANOARROW_TYPE_INT8,
                          NANOARROW_TYPE_STRING));
  SETUP_OK(InitDictSchema(schema2.children[1], NANOARROW_TYPE_INT8,
                          NANOARROW_TYPE_STRING));

  struct ArrowArray dst2;
  SETUP_OK(ArrowArrayInitFromSchema(&dst2, &schema2, &error));
  SETUP_OK(ArrowArrayStartAppending(&dst2));
  for (int i = 0; i < 127; i++) {
    snprintf(name, sizeof(name), "w%d", i);
    SETUP_OK(ArrowArrayAppendString(dst2.children[1]->dictionary,
                                    ArrowCharView(name)));
  }

  struct ArrowArray src2;
  SETUP_OK(ArrowArrayInitFromSchema(&src2, &schema2, &error));
  SETUP_OK(ArrowArrayStartAppending(&src2));
  SETUP_OK(ArrowArrayAppendString(src2.children[0]->dictionary,
                                  ArrowCharView("new1")));
  SETUP_OK(ArrowArrayAppendString(src2.children[1]->dictionary,
                                  ArrowCharView("new2")));
  SETUP_OK(ArrowArrayAppendString(src2.children[1]->dictionary,
                                  ArrowCharView("new3")));
  SETUP_OK(ArrowArrayAppendInt(src2.children[0], 0));
  SETUP_OK(ArrowArrayAppendInt(src2.children[1], 0));
  SETUP_OK(ArrowArrayFinishElement(&src2));
  SETUP_OK(ArrowArrayAppendInt(src2.children[0], 0));
  SETUP_OK(ArrowArrayAppendInt(src2.children[1], 1));
  SETUP_OK(ArrowArrayFinishElement(&src2));
  SETUP_OK(ArrowArrayFinishBuildingDefault(&src2, &error));

  struct ArrowArrayView src2_view;
  SETUP_OK(ArrowArrayViewInitFromSchema(&src2_view, &schema2, &error));
  SETUP_OK(ArrowArrayViewSetArray(&src2_view, &src2, &error));

  int rc = ArrowArrayAppendUnifiedDictionary(&dst2, &src2_view, &schema2, &error);
  TASK_CHECK(rc == EOVERFLOW);
  TASK_CHECK(error.message[0] != '\0');
  // The whole call is rolled back: no rows and no dictionary values anywhere.
  TASK_CHECK(dst2.length == 0);
  TASK_CHECK(dst2.children[0]->length == 0);
  TASK_CHECK(dst2.children[0]->dictionary->length == 0);
  TASK_CHECK(dst2.children[1]->dictionary->length == 127);

  ArrowArrayViewReset(&src2_view);
  src2.release(&src2);

  // The pre-existing dictionary content is intact and the destination is
  // still a valid, finishable array.
  SETUP_OK(ArrowArrayFinishBuilding(&dst2, NANOARROW_VALIDATION_LEVEL_FULL,
                                    &error));
  struct ArrowArrayView out2;
  SETUP_OK(ArrowArrayViewInitFromSchema(&out2, &schema2, &error));
  SETUP_OK(ArrowArrayViewSetArray(&out2, &dst2, &error));
  TASK_CHECK(DictValueEquals(out2.children[1]->dictionary, 126, "w126", 4));
  ArrowArrayViewReset(&out2);
  dst2.release(&dst2);
  schema2.release(&schema2);

  // Part C: out-of-range and negative indices are rejected with EINVAL and
  // leave the destination unmodified.
  struct ArrowSchema schema3;
  SETUP_OK(InitDictSchema(&schema3, NANOARROW_TYPE_INT8, NANOARROW_TYPE_STRING));
  const int64_t bad_indices[2][2] = {{0, 5}, {-1, 0}};
  for (int t = 0; t < 2; t++) {
    struct ArrowArray bad;
    SETUP_OK(ArrowArrayInitFromSchema(&bad, &schema3, &error));
    SETUP_OK(ArrowArrayStartAppending(&bad));
    SETUP_OK(ArrowArrayAppendString(bad.dictionary, ArrowCharView("only")));
    SETUP_OK(ArrowArrayAppendInt(&bad, bad_indices[t][0]));
    SETUP_OK(ArrowArrayAppendInt(&bad, bad_indices[t][1]));
    SETUP_OK(ArrowArrayFinishBuilding(&bad, NANOARROW_VALIDATION_LEVEL_NONE,
                                      &error));
    struct ArrowArrayView bad_view;
    SETUP_OK(ArrowArrayViewInitFromSchema(&bad_view, &schema3, &error));
    SETUP_OK(ArrowArrayViewSetArrayMinimal(&bad_view, &bad, &error));

    struct ArrowArray dst3;
    SETUP_OK(ArrowArrayInitFromSchema(&dst3, &schema3, &error));
    SETUP_OK(ArrowArrayStartAppending(&dst3));
    rc = ArrowArrayAppendUnifiedDictionary(&dst3, &bad_view, &schema3, &error);
    TASK_CHECK(rc == EINVAL);
    TASK_CHECK(error.message[0] != '\0');
    TASK_CHECK(dst3.length == 0);
    TASK_CHECK(dst3.dictionary->length == 0);

    ArrowArrayViewReset(&bad_view);
    bad.release(&bad);
    dst3.release(&dst3);
  }
  schema3.release(&schema3);
  return 0;
}

// The storage-append entry point keeps its existing behavior, the new entry
// rejects unsupported shapes with ENOTSUP/EINVAL, and handles empty input.
static int TestCompatibility(void) {
  struct ArrowError error;
  struct ArrowSchema schema;
  SETUP_OK(InitDictSchema(&schema, NANOARROW_TYPE_INT8, NANOARROW_TYPE_STRING));

  struct ArrowArray src;
  SETUP_OK(ArrowArrayInitFromSchema(&src, &schema, &error));
  SETUP_OK(ArrowArrayStartAppending(&src));
  SETUP_OK(ArrowArrayAppendString(src.dictionary, ArrowCharView("blue")));
  SETUP_OK(ArrowArrayAppendString(src.dictionary, ArrowCharView("red")));
  SETUP_OK(ArrowArrayAppendInt(&src, 1));
  SETUP_OK(ArrowArrayFinishBuildingDefault(&src, &error));

  struct ArrowArrayView view;
  SETUP_OK(ArrowArrayViewInitFromSchema(&view, &schema, &error));
  SETUP_OK(ArrowArrayViewSetArray(&view, &src, &error));

  // Old entry, shared dictionary: values are preserved.
  struct ArrowArray same;
  SETUP_OK(ArrowArrayInitFromSchema(&same, &schema, &error));
  SETUP_OK(ArrowArrayStartAppending(&same));
  SETUP_OK(ArrowArrayAppendStorageFromArrayView(same.dictionary,
                                                view.dictionary, &error));
  SETUP_OK(ArrowArrayAppendStorageFromArrayView(&same, &view, &error));
  SETUP_OK(ArrowArrayFinishBuilding(&same, NANOARROW_VALIDATION_LEVEL_FULL,
                                    &error));
  struct ArrowArrayView same_view;
  SETUP_OK(ArrowArrayViewInitFromSchema(&same_view, &schema, &error));
  SETUP_OK(ArrowArrayViewSetArray(&same_view, &same, &error));
  TASK_CHECK(FieldValueEquals(&same_view, 0, "red"));
  ArrowArrayViewReset(&same_view);
  same.release(&same);

  // Old entry, independent dictionary: raw indices are copied as-is and the
  // caller remains responsible for dictionary consistency.
  struct ArrowArray opposite;
  SETUP_OK(ArrowArrayInitFromSchema(&opposite, &schema, &error));
  SETUP_OK(ArrowArrayStartAppending(&opposite));
  SETUP_OK(ArrowArrayAppendString(opposite.dictionary, ArrowCharView("red")));
  SETUP_OK(ArrowArrayAppendString(opposite.dictionary, ArrowCharView("blue")));
  SETUP_OK(ArrowArrayAppendStorageFromArrayView(&opposite, &view, &error));
  SETUP_OK(ArrowArrayFinishBuilding(&opposite, NANOARROW_VALIDATION_LEVEL_FULL,
                                    &error));
  struct ArrowArrayView opposite_view;
  SETUP_OK(ArrowArrayViewInitFromSchema(&opposite_view, &schema, &error));
  SETUP_OK(ArrowArrayViewSetArray(&opposite_view, &opposite, &error));
  TASK_CHECK(ArrowArrayViewGetIntUnsafe(&opposite_view, 0) == 1);
  TASK_CHECK(FieldValueEquals(&opposite_view, 0, "blue"));
  ArrowArrayViewReset(&opposite_view);
  opposite.release(&opposite);

  // New entry, zero-row batch: nothing is appended, not even dictionary
  // values that no row references.
  struct ArrowArray empty_dst;
  SETUP_OK(ArrowArrayInitFromSchema(&empty_dst, &schema, &error));
  SETUP_OK(ArrowArrayStartAppending(&empty_dst));
  struct ArrowArray empty_src;
  SETUP_OK(ArrowArrayInitFromSchema(&empty_src, &schema, &error));
  SETUP_OK(ArrowArrayStartAppending(&empty_src));
  SETUP_OK(ArrowArrayAppendString(empty_src.dictionary, ArrowCharView("a")));
  SETUP_OK(ArrowArrayFinishBuildingDefault(&empty_src, &error));
  struct ArrowArrayView empty_view;
  SETUP_OK(ArrowArrayViewInitFromSchema(&empty_view, &schema, &error));
  SETUP_OK(ArrowArrayViewSetArray(&empty_view, &empty_src, &error));
  SETUP_OK(ArrowArrayAppendUnifiedDictionary(&empty_dst, &empty_view, &schema,
                                             &error));
  TASK_CHECK(empty_dst.length == 0);
  TASK_CHECK(empty_dst.dictionary->length == 0);
  ArrowArrayViewReset(&empty_view);
  empty_src.release(&empty_src);

  // New entry, all-null batch: rows are appended, the dictionary is not.
  struct ArrowArray null_src;
  SETUP_OK(ArrowArrayInitFromSchema(&null_src, &schema, &error));
  SETUP_OK(ArrowArrayStartAppending(&null_src));
  SETUP_OK(ArrowArrayAppendString(null_src.dictionary, ArrowCharView("a")));
  SETUP_OK(ArrowArrayAppendNull(&null_src, 2));
  SETUP_OK(ArrowArrayFinishBuildingDefault(&null_src, &error));
  struct ArrowArrayView null_view;
  SETUP_OK(ArrowArrayViewInitFromSchema(&null_view, &schema, &error));
  SETUP_OK(ArrowArrayViewSetArray(&null_view, &null_src, &error));
  SETUP_OK(ArrowArrayAppendUnifiedDictionary(&empty_dst, &null_view, &schema,
                                             &error));
  TASK_CHECK(empty_dst.length == 2);
  TASK_CHECK(empty_dst.null_count == 2);
  TASK_CHECK(empty_dst.dictionary->length == 0);
  SETUP_OK(ArrowArrayFinishBuilding(&empty_dst, NANOARROW_VALIDATION_LEVEL_FULL,
                                    &error));
  struct ArrowArrayView empty_out;
  SETUP_OK(ArrowArrayViewInitFromSchema(&empty_out, &schema, &error));
  SETUP_OK(ArrowArrayViewSetArray(&empty_out, &empty_dst, &error));
  TASK_CHECK(ArrowArrayViewIsNull(&empty_out, 0) == 1);
  TASK_CHECK(ArrowArrayViewIsNull(&empty_out, 1) == 1);
  ArrowArrayViewReset(&empty_out);
  ArrowArrayViewReset(&null_view);
  null_src.release(&null_src);
  empty_dst.release(&empty_dst);

  // New entry, ordered dictionary: ENOTSUP.
  struct ArrowSchema ordered_schema;
  SETUP_OK(InitDictSchema(&ordered_schema, NANOARROW_TYPE_INT8,
                          NANOARROW_TYPE_STRING));
  ordered_schema.flags |= ARROW_FLAG_DICTIONARY_ORDERED;
  struct ArrowArray ordered_dst, ordered_src;
  SETUP_OK(ArrowArrayInitFromSchema(&ordered_dst, &ordered_schema, &error));
  SETUP_OK(ArrowArrayStartAppending(&ordered_dst));
  SETUP_OK(ArrowArrayInitFromSchema(&ordered_src, &ordered_schema, &error));
  SETUP_OK(ArrowArrayStartAppending(&ordered_src));
  SETUP_OK(ArrowArrayFinishBuildingDefault(&ordered_src, &error));
  struct ArrowArrayView ordered_view;
  SETUP_OK(ArrowArrayViewInitFromSchema(&ordered_view, &ordered_schema, &error));
  SETUP_OK(ArrowArrayViewSetArray(&ordered_view, &ordered_src, &error));
  int rc = ArrowArrayAppendUnifiedDictionary(&ordered_dst, &ordered_view,
                                             &ordered_schema, &error);
  TASK_CHECK(rc == ENOTSUP);
  TASK_CHECK(ordered_dst.length == 0);
  ArrowArrayViewReset(&ordered_view);
  ordered_src.release(&ordered_src);
  ordered_dst.release(&ordered_dst);
  ordered_schema.release(&ordered_schema);

  // New entry, unsigned dictionary index: ENOTSUP.
  struct ArrowSchema uint_schema;
  SETUP_OK(InitDictSchema(&uint_schema, NANOARROW_TYPE_UINT8,
                          NANOARROW_TYPE_STRING));
  struct ArrowArray uint_dst, uint_src;
  SETUP_OK(ArrowArrayInitFromSchema(&uint_dst, &uint_schema, &error));
  SETUP_OK(ArrowArrayStartAppending(&uint_dst));
  SETUP_OK(ArrowArrayInitFromSchema(&uint_src, &uint_schema, &error));
  SETUP_OK(ArrowArrayStartAppending(&uint_src));
  SETUP_OK(ArrowArrayFinishBuildingDefault(&uint_src, &error));
  struct ArrowArrayView uint_view;
  SETUP_OK(ArrowArrayViewInitFromSchema(&uint_view, &uint_schema, &error));
  SETUP_OK(ArrowArrayViewSetArray(&uint_view, &uint_src, &error));
  rc = ArrowArrayAppendUnifiedDictionary(&uint_dst, &uint_view, &uint_schema,
                                         &error);
  TASK_CHECK(rc == ENOTSUP);
  ArrowArrayViewReset(&uint_view);
  uint_src.release(&uint_src);
  uint_dst.release(&uint_dst);
  uint_schema.release(&uint_schema);

  // New entry, unsupported leaf type: ENOTSUP.
  struct ArrowSchema half_schema;
  SETUP_OK(ArrowSchemaInitFromType(&half_schema, NANOARROW_TYPE_HALF_FLOAT));
  struct ArrowArray half_dst, half_src;
  SETUP_OK(ArrowArrayInitFromSchema(&half_dst, &half_schema, &error));
  SETUP_OK(ArrowArrayStartAppending(&half_dst));
  SETUP_OK(ArrowArrayInitFromSchema(&half_src, &half_schema, &error));
  SETUP_OK(ArrowArrayStartAppending(&half_src));
  SETUP_OK(ArrowArrayFinishBuildingDefault(&half_src, &error));
  struct ArrowArrayView half_view;
  SETUP_OK(ArrowArrayViewInitFromSchema(&half_view, &half_schema, &error));
  SETUP_OK(ArrowArrayViewSetArray(&half_view, &half_src, &error));
  rc = ArrowArrayAppendUnifiedDictionary(&half_dst, &half_view, &half_schema,
                                         &error);
  TASK_CHECK(rc == ENOTSUP);
  ArrowArrayViewReset(&half_view);
  half_src.release(&half_src);
  half_dst.release(&half_dst);
  half_schema.release(&half_schema);

  // New entry, schema that does not match the storage: EINVAL.
  struct ArrowSchema schema_a, schema_b;
  SETUP_OK(ArrowSchemaInitFromType(&schema_a, NANOARROW_TYPE_STRUCT));
  SETUP_OK(ArrowSchemaAllocateChildren(&schema_a, 1));
  SETUP_OK(ArrowSchemaInitFromType(schema_a.children[0], NANOARROW_TYPE_INT32));
  SETUP_OK(ArrowSchemaInitFromType(&schema_b, NANOARROW_TYPE_STRUCT));
  SETUP_OK(ArrowSchemaAllocateChildren(&schema_b, 1));
  SETUP_OK(ArrowSchemaInitFromType(schema_b.children[0], NANOARROW_TYPE_INT64));
  struct ArrowArray mismatch_dst, mismatch_src;
  SETUP_OK(ArrowArrayInitFromSchema(&mismatch_dst, &schema_a, &error));
  SETUP_OK(ArrowArrayStartAppending(&mismatch_dst));
  SETUP_OK(ArrowArrayInitFromSchema(&mismatch_src, &schema_a, &error));
  SETUP_OK(ArrowArrayStartAppending(&mismatch_src));
  SETUP_OK(ArrowArrayFinishBuildingDefault(&mismatch_src, &error));
  struct ArrowArrayView mismatch_view;
  SETUP_OK(ArrowArrayViewInitFromSchema(&mismatch_view, &schema_a, &error));
  SETUP_OK(ArrowArrayViewSetArray(&mismatch_view, &mismatch_src, &error));
  rc = ArrowArrayAppendUnifiedDictionary(&mismatch_dst, &mismatch_view,
                                         &schema_b, &error);
  TASK_CHECK(rc == EINVAL);
  TASK_CHECK(mismatch_dst.length == 0);
  ArrowArrayViewReset(&mismatch_view);
  mismatch_src.release(&mismatch_src);
  mismatch_dst.release(&mismatch_dst);
  schema_a.release(&schema_a);
  schema_b.release(&schema_b);

  // New entry, plain (non-dictionary) struct: values are appended by value.
  struct ArrowSchema plain_schema;
  SETUP_OK(ArrowSchemaInitFromType(&plain_schema, NANOARROW_TYPE_STRUCT));
  SETUP_OK(ArrowSchemaAllocateChildren(&plain_schema, 2));
  SETUP_OK(ArrowSchemaInitFromType(plain_schema.children[0],
                                   NANOARROW_TYPE_INT32));
  SETUP_OK(ArrowSchemaInitFromType(plain_schema.children[1],
                                   NANOARROW_TYPE_STRING));
  struct ArrowArray plain_src;
  SETUP_OK(ArrowArrayInitFromSchema(&plain_src, &plain_schema, &error));
  SETUP_OK(ArrowArrayStartAppending(&plain_src));
  SETUP_OK(ArrowArrayAppendInt(plain_src.children[0], 42));
  SETUP_OK(ArrowArrayAppendString(plain_src.children[1], ArrowCharView("x")));
  SETUP_OK(ArrowArrayFinishElement(&plain_src));
  SETUP_OK(ArrowArrayAppendNull(plain_src.children[0], 1));
  SETUP_OK(ArrowArrayAppendString(plain_src.children[1], ArrowCharView("y")));
  SETUP_OK(ArrowArrayFinishElement(&plain_src));
  SETUP_OK(ArrowArrayFinishBuildingDefault(&plain_src, &error));
  struct ArrowArrayView plain_view;
  SETUP_OK(ArrowArrayViewInitFromSchema(&plain_view, &plain_schema, &error));
  SETUP_OK(ArrowArrayViewSetArray(&plain_view, &plain_src, &error));
  struct ArrowArray plain_dst;
  SETUP_OK(ArrowArrayInitFromSchema(&plain_dst, &plain_schema, &error));
  SETUP_OK(ArrowArrayStartAppending(&plain_dst));
  SETUP_OK(ArrowArrayAppendUnifiedDictionary(&plain_dst, &plain_view,
                                             &plain_schema, &error));
  SETUP_OK(ArrowArrayFinishBuilding(&plain_dst, NANOARROW_VALIDATION_LEVEL_FULL,
                                    &error));
  struct ArrowArrayView plain_out;
  SETUP_OK(ArrowArrayViewInitFromSchema(&plain_out, &plain_schema, &error));
  SETUP_OK(ArrowArrayViewSetArray(&plain_out, &plain_dst, &error));
  TASK_CHECK(plain_dst.length == 2);
  TASK_CHECK(ArrowArrayViewGetIntUnsafe(plain_out.children[0], 0) == 42);
  TASK_CHECK(ArrowArrayViewIsNull(plain_out.children[0], 1) == 1);
  struct ArrowStringView plain_str =
      ArrowArrayViewGetStringUnsafe(plain_out.children[1], 1);
  TASK_CHECK(plain_str.size_bytes == 1 && memcmp(plain_str.data, "y", 1) == 0);
  ArrowArrayViewReset(&plain_out);
  ArrowArrayViewReset(&plain_view);
  plain_src.release(&plain_src);
  plain_dst.release(&plain_dst);
  plain_schema.release(&plain_schema);

  ArrowArrayViewReset(&view);
  src.release(&src);
  schema.release(&schema);
  return 0;
}

void task_register_tests(void) {
  task_case("DictionaryOrder", TestDictionaryOrder);
  task_case("NestedSlices", TestNestedSlices);
  task_case("IndexOverflow", TestIndexOverflow);
  task_case("Compatibility", TestCompatibility);
}
