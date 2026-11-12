// Copyright 2016 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Author: jdtang@google.com (Jonathan Tang)
//
// Generic storage for the per-text-node source log and the node->log
// registry.  Parser-state-dependent recording lives in parser.c, since the
// GumboParserState definition is private to that translation unit.

#include "text_source.h"

#include <stdint.h>
#include <string.h>

#include "parser.h"
#include "util.h"

// A text node together with the source log captured when it was flushed.
typedef struct {
  GumboNode* node;
  GumboTextSourceLog log;
} GumboTextSourceRecord;

// Open-addressing registry mapping a text-node pointer to its record.
struct GumboTextSourceRegistry {
  // Hash table: keys[i] is the node pointer (NULL means an empty slot) and
  // indices[i] is the record number in `records`.
  GumboNode** keys;
  size_t* indices;
  size_t table_size;  // Always a power of two.
  size_t table_count;

  GumboTextSourceRecord* records;
  size_t records_length;
  size_t records_capacity;

  GumboAllocatorFunction allocator;
  GumboDeallocatorFunction deallocator;
  void* userdata;
};

static void* registry_allocate(
    GumboTextSourceRegistry* registry, size_t n) {
  return registry->allocator(registry->userdata, n);
}

static void registry_deallocate(
    GumboTextSourceRegistry* registry, void* p) {
  registry->deallocator(registry->userdata, p);
}

void gumbo_text_source_log_init(
    GumboParser* parser, GumboTextSourceLog* log) {
  (void) parser;
  log->data = NULL;
  log->length = 0;
  log->capacity = 0;
}

void gumbo_text_source_log_destroy(
    GumboParser* parser, GumboTextSourceLog* log) {
  if (log->data) {
    gumbo_parser_deallocate(parser, log->data);
    log->data = NULL;
  }
  log->length = 0;
  log->capacity = 0;
}

void gumbo_text_source_log_add(
    GumboParser* parser, GumboTextSourceLog* log,
    const GumboTextSourceEntry* entry) {
  if (log->capacity == log->length) {
    size_t new_capacity = log->capacity ? log->capacity * 2 : 8;
    GumboTextSourceEntry* new_data =
        gumbo_parser_allocate(parser, new_capacity * sizeof(*new_data));
    if (log->length) {
      memcpy(new_data, log->data, log->length * sizeof(*new_data));
    }
    if (log->data) {
      gumbo_parser_deallocate(parser, log->data);
    }
    log->data = new_data;
    log->capacity = new_capacity;
  }
  log->data[log->length++] = *entry;
}

bool gumbo_text_source_tracking_enabled(const GumboParser* parser) {
  return parser->_options->track_text_sources;
}

// Fibonacci-style mixing for pointer keys; good enough for linear probing.
static size_t pointer_hash(const void* key) {
  uintptr_t bits = (uintptr_t) key;
  bits ^= bits >> 33;
  bits *= 0xff51afd7ed558ccdULL;
  bits ^= bits >> 33;
  return (size_t) bits;
}

static void registry_rehash(
    GumboTextSourceRegistry* registry, size_t new_size) {
  GumboNode** new_keys =
      registry_allocate(registry, new_size * sizeof(*new_keys));
  size_t* new_indices =
      registry_allocate(registry, new_size * sizeof(*new_indices));
  for (size_t i = 0; i < new_size; ++i) {
    new_keys[i] = NULL;
  }
  for (size_t i = 0; i < registry->table_size; ++i) {
    GumboNode* key = registry->keys[i];
    if (key == NULL) {
      continue;
    }
    size_t slot = pointer_hash(key) & (new_size - 1);
    while (new_keys[slot] != NULL) {
      slot = (slot + 1) & (new_size - 1);
    }
    new_keys[slot] = key;
    new_indices[slot] = registry->indices[i];
  }
  registry_deallocate(registry, registry->keys);
  registry_deallocate(registry, registry->indices);
  registry->keys = new_keys;
  registry->indices = new_indices;
  registry->table_size = new_size;
}

GumboTextSourceRegistry* gumbo_text_source_registry_create(
    const GumboOptions* options) {
  GumboTextSourceRegistry* registry =
      options->allocator(options->userdata, sizeof(*registry));
  registry->table_size = 16;
  registry->table_count = 0;
  registry->keys = options->allocator(
      options->userdata, registry->table_size * sizeof(*registry->keys));
  registry->indices = options->allocator(
      options->userdata, registry->table_size * sizeof(*registry->indices));
  for (size_t i = 0; i < registry->table_size; ++i) {
    registry->keys[i] = NULL;
  }
  registry->records = NULL;
  registry->records_length = 0;
  registry->records_capacity = 0;
  registry->allocator = options->allocator;
  registry->deallocator = options->deallocator;
  registry->userdata = options->userdata;
  return registry;
}

void gumbo_text_source_registry_store(GumboTextSourceRegistry* registry,
    GumboNode* node, GumboTextSourceLog* log, const GumboOptions* options) {
  (void) options;
  if (registry->records_length == registry->records_capacity) {
    size_t new_capacity =
        registry->records_capacity ? registry->records_capacity * 2 : 16;
    GumboTextSourceRecord* new_records = registry_allocate(
        registry, new_capacity * sizeof(*new_records));
    if (registry->records_length) {
      memcpy(new_records, registry->records,
          registry->records_length * sizeof(*new_records));
    }
    if (registry->records) {
      registry_deallocate(registry, registry->records);
    }
    registry->records = new_records;
    registry->records_capacity = new_capacity;
  }
  size_t record_index = registry->records_length;
  GumboTextSourceRecord* record = &registry->records[record_index];
  record->node = node;
  record->log = *log;  // Transfer ownership of the log storage.
  log->data = NULL;
  log->length = 0;
  log->capacity = 0;
  ++registry->records_length;

  if ((registry->table_count + 1) * 10 >= registry->table_size * 7) {
    registry_rehash(registry, registry->table_size * 2);
  }
  size_t slot = pointer_hash(node) & (registry->table_size - 1);
  while (registry->keys[slot] != NULL) {
    slot = (slot + 1) & (registry->table_size - 1);
  }
  registry->keys[slot] = node;
  registry->indices[slot] = record_index;
  ++registry->table_count;
}

const GumboTextSourceLog* gumbo_text_source_registry_find(
    const GumboTextSourceRegistry* registry, const GumboNode* node) {
  if (!registry || registry->table_count == 0) {
    return NULL;
  }
  size_t slot = pointer_hash(node) & (registry->table_size - 1);
  for (;;) {
    GumboNode* key = registry->keys[slot];
    if (key == NULL) {
      return NULL;
    }
    if (key == node) {
      return &registry->records[registry->indices[slot]].log;
    }
    slot = (slot + 1) & (registry->table_size - 1);
  }
}

void gumbo_text_source_registry_destroy(
    const GumboOptions* options, GumboTextSourceRegistry* registry) {
  if (!registry) {
    return;
  }
  for (size_t i = 0; i < registry->records_length; ++i) {
    GumboTextSourceLog* log = &registry->records[i].log;
    if (log->data) {
      options->deallocator(options->userdata, log->data);
    }
  }
  registry->deallocator(registry->userdata, registry->records);
  registry->deallocator(registry->userdata, registry->keys);
  registry->deallocator(registry->userdata, registry->indices);
  registry->deallocator(registry->userdata, registry);
}
