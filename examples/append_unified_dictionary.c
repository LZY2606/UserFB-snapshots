// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// Example: merge dictionary-encoded column batches whose dictionaries were
// encoded independently (e.g., per-shard encodings of a low-cardinality
// column like a device model string).
//
// Build against the nanoarrow static library, e.g.:
//   cc -std=c99 -Isrc -I<build>/src examples/append_unified_dictionary.c \
//     <build>/libnanoarrow_static.a -o append_unified_dictionary

#include <stdio.h>

#include "nanoarrow/nanoarrow.h"

// One shard of a struct<device: dictionary(int8, utf8)> column. Each shard
// encodes its dictionary independently, so index 0 may mean "pixel" in one
// shard and "kindle" in another.
static int MakeShard(struct ArrowSchema* schema, const char** dictionary,
                     int64_t n_dictionary, const int64_t* indices,
                     int64_t n_indices, struct ArrowArray* out,
                     struct ArrowError* error) {
  if (ArrowArrayInitFromSchema(out, schema, error) != NANOARROW_OK) {
    return 1;
  }
  if (ArrowArrayStartAppending(out) != NANOARROW_OK) {
    return 1;
  }
  for (int64_t i = 0; i < n_dictionary; i++) {
    if (ArrowArrayAppendString(out->children[0]->dictionary,
                               ArrowCharView(dictionary[i])) != NANOARROW_OK) {
      return 1;
    }
  }
  for (int64_t i = 0; i < n_indices; i++) {
    if (ArrowArrayAppendInt(out->children[0], indices[i]) != NANOARROW_OK ||
        ArrowArrayFinishElement(out) != NANOARROW_OK) {
      return 1;
    }
  }
  return ArrowArrayFinishBuildingDefault(out, error) != NANOARROW_OK;
}

int main(void) {
  struct ArrowError error;
  struct ArrowSchema schema;
  ArrowSchemaInitFromType(&schema, NANOARROW_TYPE_STRUCT);
  ArrowSchemaAllocateChildren(&schema, 1);
  ArrowSchemaInitFromType(schema.children[0], NANOARROW_TYPE_INT8);
  ArrowSchemaAllocateDictionary(schema.children[0]);
  ArrowSchemaInitFromType(schema.children[0]->dictionary, NANOARROW_TYPE_STRING);

  const char* dict_a[] = {"pixel", "kindle"};
  const int64_t indices_a[] = {0, 1, 0};
  const char* dict_b[] = {"kindle", "pixel", "nest"};
  const int64_t indices_b[] = {2, 1, 0};

  struct ArrowArray shard_a, shard_b;
  if (MakeShard(&schema, dict_a, 2, indices_a, 3, &shard_a, &error) ||
      MakeShard(&schema, dict_b, 3, indices_b, 3, &shard_b, &error)) {
    fprintf(stderr, "failed to build input: %s\n", ArrowErrorMessage(&error));
    return 1;
  }

  struct ArrowArray merged;
  ArrowArrayInitFromSchema(&merged, &schema, &error);
  ArrowArrayStartAppending(&merged);

  const struct ArrowArray* shards[] = {&shard_a, &shard_b};
  for (int i = 0; i < 2; i++) {
    struct ArrowArrayView view;
    ArrowArrayViewInitFromSchema(&view, &schema, &error);
    ArrowArrayViewSetArray(&view, shards[i], &error);
    // Re-encodes indices by value: dictionary values already present in
    // merged keep their indices, new values are appended in
    // first-encountered order.
    ArrowErrorCode code =
        ArrowArrayAppendUnifiedDictionary(&merged, &view, &schema, &error);
    ArrowArrayViewReset(&view);
    if (code != NANOARROW_OK) {
      fprintf(stderr, "append failed: %s\n", ArrowErrorMessage(&error));
      return 1;
    }
  }

  shard_a.release(&shard_a);
  shard_b.release(&shard_b);

  if (ArrowArrayFinishBuilding(&merged, NANOARROW_VALIDATION_LEVEL_FULL,
                               &error) != NANOARROW_OK) {
    fprintf(stderr, "invalid output: %s\n", ArrowErrorMessage(&error));
    return 1;
  }

  struct ArrowArrayView merged_view;
  ArrowArrayViewInitFromSchema(&merged_view, &schema, &error);
  ArrowArrayViewSetArray(&merged_view, &merged, &error);
  struct ArrowArrayView* device = merged_view.children[0];
  for (int64_t i = 0; i < merged_view.length; i++) {
    int64_t index = ArrowArrayViewGetIntUnsafe(device, i);
    struct ArrowStringView value =
        ArrowArrayViewGetStringUnsafe(device->dictionary, index);
    printf("row %lld: index=%lld value=%.*s\n", (long long)i, (long long)index,
           (int)value.size_bytes, value.data);
  }

  ArrowArrayViewReset(&merged_view);
  merged.release(&merged);
  schema.release(&schema);
  return 0;
}
