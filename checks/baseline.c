#include "nanoarrow/nanoarrow.h"
#include <stdio.h>
#include <string.h>
#define CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr, "baseline:%d: %s\n", __LINE__, #expr); return 1; \
} } while (0)
int main(void) {
  struct ArrowSchema s;
  struct ArrowArray src, same, opposite;
  struct ArrowArrayView view, result;
  struct ArrowError error;
  CHECK(ArrowSchemaInitFromType(&s, NANOARROW_TYPE_INT8) == 0);
  CHECK(ArrowSchemaAllocateDictionary(&s) == 0);
  CHECK(ArrowSchemaInitFromType(s.dictionary, NANOARROW_TYPE_STRING) == 0);
  CHECK(ArrowArrayInitFromSchema(&src, &s, &error) == 0);
  CHECK(ArrowArrayStartAppending(&src) == 0);
  CHECK(ArrowArrayAppendString(src.dictionary, ArrowCharView("blue")) == 0);
  CHECK(ArrowArrayAppendString(src.dictionary, ArrowCharView("red")) == 0);
  CHECK(ArrowArrayAppendInt(&src, 1) == 0);
  CHECK(ArrowArrayFinishBuildingDefault(&src, &error) == 0);
  CHECK(ArrowArrayViewInitFromSchema(&view, &s, &error) == 0);
  CHECK(ArrowArrayViewSetArray(&view, &src, &error) == 0);
  CHECK(ArrowArrayInitFromSchema(&same, &s, &error) == 0);
  CHECK(ArrowArrayStartAppending(&same) == 0);
  CHECK(ArrowArrayAppendStorageFromArrayView(same.dictionary, view.dictionary, &error) == 0);
  CHECK(ArrowArrayAppendStorageFromArrayView(&same, &view, &error) == 0);
  CHECK(ArrowArrayFinishBuilding(&same, NANOARROW_VALIDATION_LEVEL_FULL, &error) == 0);
  CHECK(ArrowArrayViewInitFromSchema(&result, &s, &error) == 0);
  CHECK(ArrowArrayViewSetArray(&result, &same, &error) == 0);
  int64_t index = ArrowArrayViewGetIntUnsafe(&result, 0);
  struct ArrowStringView value = ArrowArrayViewGetStringUnsafe(result.dictionary, index);
  CHECK(same.length == 1 && same.dictionary->length == 2 && index == 1);
  CHECK(value.size_bytes == 3 && memcmp(value.data, "red", 3) == 0);
  puts("NANOARROW_STORAGE_OK rows=1 dictionary=2 index=1 value=red");
  ArrowArrayViewReset(&result);
  CHECK(ArrowArrayInitFromSchema(&opposite, &s, &error) == 0);
  CHECK(ArrowArrayStartAppending(&opposite) == 0);
  CHECK(ArrowArrayAppendString(opposite.dictionary, ArrowCharView("red")) == 0);
  CHECK(ArrowArrayAppendString(opposite.dictionary, ArrowCharView("blue")) == 0);
  CHECK(ArrowArrayAppendStorageFromArrayView(&opposite, &view, &error) == 0);
  CHECK(ArrowArrayFinishBuilding(&opposite, NANOARROW_VALIDATION_LEVEL_FULL, &error) == 0);
  CHECK(ArrowArrayViewInitFromSchema(&result, &s, &error) == 0);
  CHECK(ArrowArrayViewSetArray(&result, &opposite, &error) == 0);
  index = ArrowArrayViewGetIntUnsafe(&result, 0);
  value = ArrowArrayViewGetStringUnsafe(result.dictionary, index);
  CHECK(index == 1 && value.size_bytes == 4 && memcmp(value.data, "blue", 4) == 0);
  puts("NANOARROW_UNIFICATION_GAP source=red copied=blue raw_index=1");
  ArrowArrayViewReset(&result);
  ArrowArrayViewReset(&view);
  src.release(&src); same.release(&same); opposite.release(&opposite); s.release(&s);
  return 0;
}
