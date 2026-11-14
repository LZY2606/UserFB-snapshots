/**
 * Copyright 2008-2012 Digital Bazaar, Inc.
 *
 * This file is part of librdfa.
 *
 * librdfa is Free Software, and can be licensed under any of the
 * following three licenses:
 *
 *   1. GNU Lesser General Public License (LGPL) V2.1 or any
 *      newer version
 *   2. GNU General Public License (GPL) V2 or any newer version
 *   3. Apache License, V2.0 or any newer version
 *
 * You may not use this file except in compliance with at least one of
 * the above three licenses.
 *
 * See LICENSE-* at the top of this software distribution for more
 * information regarding the details of each license.
 *
 * Implements HTML+RDFa 1.1 property copying.  While the option is
 * enabled the entire default graph is buffered.  Once the input has
 * ended successfully the rdfa:copy closure is applied, the copied
 * pattern definitions are removed, and each remaining triple is
 * delivered exactly once through the normal default graph handler.
 */
#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdlib.h>
#include <string.h>
#include "rdfa.h"
#include "rdfa_utils.h"
#include "property_copy.h"

#define RDFA_COPY_PREDICATE "http://www.w3.org/ns/rdfa#copy"
#define RDF_TYPE_PREDICATE \
   "http://www.w3.org/1999/02/22-rdf-syntax-ns#type"
#define RDFA_PATTERN_IRI "http://www.w3.org/ns/rdfa#Pattern"

#define PC_MIN_CAPACITY 16
#define PC_INITIAL_TABLE_SIZE 1024
#define PC_INITIAL_BUCKETS 64

#define PC_NO_INDEX ((size_t)-1)

typedef struct pc_table_entry
{
   unsigned long hash;
   size_t index; /* stored as index + 1, zero means empty */
} pc_table_entry;

typedef struct pc_subject_entry
{
   const char* subject; /* borrowed from a stored triple */
   size_t* indices;
   size_t count;
   size_t capacity;
   struct pc_subject_entry* next;
} pc_subject_entry;

typedef struct
{
   pc_subject_entry** buckets;
   size_t bucket_count;
   size_t entry_count;
} pc_subject_index;

struct rdfa_property_copy_state
{
   rdftriple** triples;
   size_t count;
   size_t capacity;

   pc_table_entry* table;
   size_t table_size;
   size_t table_used;

   int delivered;
};

static unsigned long pc_hash_string(const char* str)
{
   unsigned long hash = 5381;
   if(str == NULL)
   {
      return 0;
   }
   while(*str != '\0')
   {
      hash = ((hash << 5) + hash) ^ (unsigned long)(unsigned char)*str;
      str++;
   }
   return hash;
}

static unsigned long pc_hash_triple(const rdftriple* triple)
{
   unsigned long hash = pc_hash_string(triple->subject);
   hash = (hash * 31u) + pc_hash_string(triple->predicate);
   hash = (hash * 31u) + pc_hash_string(triple->object);
   hash = (hash * 31u) + (unsigned long)triple->object_type;
   hash = (hash * 31u) + pc_hash_string(triple->datatype);
   hash = (hash * 31u) + pc_hash_string(triple->language);
   return hash;
}

static int pc_string_equal(const char* left, const char* right)
{
   if(left == NULL || right == NULL)
   {
      return left == right;
   }
   return strcmp(left, right) == 0;
}

static int pc_triple_equal(const rdftriple* left, const rdftriple* right)
{
   return left->object_type == right->object_type &&
      pc_string_equal(left->subject, right->subject) &&
      pc_string_equal(left->predicate, right->predicate) &&
      pc_string_equal(left->object, right->object) &&
      pc_string_equal(left->datatype, right->datatype) &&
      pc_string_equal(left->language, right->language);
}

static int pc_is_copy_edge(const rdftriple* triple)
{
   return triple->object_type == RDF_TYPE_IRI &&
      pc_string_equal(triple->predicate, RDFA_COPY_PREDICATE);
}

static int pc_is_pattern_type_triple(const rdftriple* triple)
{
   return pc_string_equal(triple->predicate, RDF_TYPE_PREDICATE) &&
      triple->object_type == RDF_TYPE_IRI &&
      pc_string_equal(triple->object, RDFA_PATTERN_IRI);
}

static int pc_table_resize(rdfa_property_copy_state* state, size_t new_size)
{
   pc_table_entry* new_table = (pc_table_entry*)calloc(
      new_size, sizeof(pc_table_entry));
   size_t i;

   if(new_table == NULL)
   {
      return 0;
   }

   for(i = 0; i < state->count; i++)
   {
      unsigned long hash = pc_hash_triple(state->triples[i]);
      size_t slot = (size_t)(hash & (unsigned long)(new_size - 1));
      while(new_table[slot].index != 0)
      {
         slot = (slot + 1) & (new_size - 1);
      }
      new_table[slot].hash = hash;
      new_table[slot].index = i + 1;
   }

   free(state->table);
   state->table = new_table;
   state->table_size = new_size;
   state->table_used = state->count;
   return 1;
}

/**
 * Searches the graph for a triple equal to the given triple.
 *
 * @return the index of the triple, or PC_NO_INDEX when it is not present.
 */
static size_t pc_find(
   const rdfa_property_copy_state* state, const rdftriple* triple)
{
   unsigned long hash;
   size_t slot;
   size_t probe = 0;

   if(state->table_size == 0)
   {
      return PC_NO_INDEX;
   }

   hash = pc_hash_triple(triple);
   slot = (size_t)(hash & (unsigned long)(state->table_size - 1));
   while(state->table[slot].index != 0)
   {
      size_t index = state->table[slot].index - 1;
      if(state->table[slot].hash == hash &&
         pc_triple_equal(state->triples[index], triple))
      {
         return index;
      }
      probe++;
      if(probe >= state->table_size)
      {
         break;
      }
      slot = (slot + 1) & (state->table_size - 1);
   }

   return PC_NO_INDEX;
}

static size_t pc_store(rdfa_property_copy_state* state, rdftriple* triple)
{
   unsigned long hash;
   size_t slot;
   size_t probe = 0;
   size_t index;

   if(pc_find(state, triple) != PC_NO_INDEX)
   {
      rdfa_free_triple(triple);
      return PC_NO_INDEX;
   }

   if(state->count >= state->capacity)
   {
      size_t new_capacity =
         state->capacity == 0 ? PC_MIN_CAPACITY : state->capacity * 2;
      rdftriple** new_triples = (rdftriple**)realloc(
         state->triples, sizeof(rdftriple*) * new_capacity);
      if(new_triples == NULL)
      {
         rdfa_free_triple(triple);
         return PC_NO_INDEX;
      }
      state->triples = new_triples;
      state->capacity = new_capacity;
   }

   if((state->table_used + 1) * 10 >= state->table_size * 7)
   {
      if(!pc_table_resize(state, state->table_size * 2))
      {
         rdfa_free_triple(triple);
         return PC_NO_INDEX;
      }
   }

   index = state->count;
   state->triples[index] = triple;
   state->count++;

   hash = pc_hash_triple(triple);
   slot = (size_t)(hash & (unsigned long)(state->table_size - 1));
   while(state->table[slot].index != 0)
   {
      probe++;
      if(probe >= state->table_size)
      {
         /* the table is full; this cannot happen given the load factor */
         return index;
      }
      slot = (slot + 1) & (state->table_size - 1);
   }
   state->table[slot].hash = hash;
   state->table[slot].index = index + 1;
   state->table_used++;

   return index;
}

void rdfa_property_copy_collect(
   rdfa_property_copy_state* state, rdftriple* triple)
{
   if(triple == NULL)
   {
      return;
   }

   if(state == NULL)
   {
      rdfa_free_triple(triple);
      return;
   }

   pc_store(state, triple);
}

/* ------------------------------------------------------------------ */
/* Subject index                                                       */
/* ------------------------------------------------------------------ */

static int pc_index_init(pc_subject_index* index)
{
   index->bucket_count = PC_INITIAL_BUCKETS;
   index->entry_count = 0;
   index->buckets = (pc_subject_entry**)calloc(
      index->bucket_count, sizeof(pc_subject_entry*));
   return index->buckets != NULL;
}

static void pc_index_free(pc_subject_index* index)
{
   size_t i;
   for(i = 0; i < index->bucket_count; i++)
   {
      pc_subject_entry* entry = index->buckets[i];
      while(entry != NULL)
      {
         pc_subject_entry* next = entry->next;
         free(entry->indices);
         free(entry);
         entry = next;
      }
   }
   free(index->buckets);
   index->buckets = NULL;
   index->bucket_count = 0;
   index->entry_count = 0;
}

static pc_subject_entry* pc_index_lookup(
   const pc_subject_index* index, const char* subject)
{
   size_t bucket;
   pc_subject_entry* entry;

   if(index->bucket_count == 0)
   {
      return NULL;
   }

   bucket = (size_t)(pc_hash_string(subject) &
      (unsigned long)(index->bucket_count - 1));
   for(entry = index->buckets[bucket]; entry != NULL; entry = entry->next)
   {
      if(pc_string_equal(entry->subject, subject))
      {
         return entry;
      }
   }
   return NULL;
}

static int pc_index_rehash(pc_subject_index* index)
{
   size_t new_count = index->bucket_count * 2;
   pc_subject_entry** new_buckets = (pc_subject_entry**)calloc(
      new_count, sizeof(pc_subject_entry*));
   size_t i;

   if(new_buckets == NULL)
   {
      return 0;
   }

   for(i = 0; i < index->bucket_count; i++)
   {
      pc_subject_entry* entry = index->buckets[i];
      while(entry != NULL)
      {
         pc_subject_entry* next = entry->next;
         size_t bucket = (size_t)(pc_hash_string(entry->subject) &
            (unsigned long)(new_count - 1));
         entry->next = new_buckets[bucket];
         new_buckets[bucket] = entry;
         entry = next;
      }
   }

   free(index->buckets);
   index->buckets = new_buckets;
   index->bucket_count = new_count;
   return 1;
}

static int pc_index_add(
   pc_subject_index* index, const char* subject, size_t triple_index)
{
   pc_subject_entry* entry = pc_index_lookup(index, subject);
   size_t bucket;

   if(entry == NULL)
   {
      if((index->entry_count + 1) * 4 >= index->bucket_count * 3)
      {
         if(!pc_index_rehash(index))
         {
            return 0;
         }
      }

      bucket = (size_t)(pc_hash_string(subject) &
         (unsigned long)(index->bucket_count - 1));
      entry = (pc_subject_entry*)calloc(1, sizeof(pc_subject_entry));
      if(entry == NULL)
      {
         return 0;
      }
      entry->subject = subject;
      entry->next = index->buckets[bucket];
      index->buckets[bucket] = entry;
      index->entry_count++;
   }

   if(entry->count >= entry->capacity)
   {
      size_t new_capacity = entry->capacity == 0 ? 4 : entry->capacity * 2;
      size_t* new_indices = (size_t*)realloc(
         entry->indices, sizeof(size_t) * new_capacity);
      if(new_indices == NULL)
      {
         return 0;
      }
      entry->indices = new_indices;
      entry->capacity = new_capacity;
   }

   entry->indices[entry->count++] = triple_index;
   return 1;
}

static int pc_index_build(
   const rdfa_property_copy_state* state, pc_subject_index* index)
{
   size_t i;
   if(!pc_index_init(index))
   {
      return 0;
   }
   for(i = 0; i < state->count; i++)
   {
      if(!pc_index_add(index, state->triples[i]->subject, i))
      {
         pc_index_free(index);
         return 0;
      }
   }
   return 1;
}

/**
 * Determines whether the subject currently has an rdf:type rdfa:Pattern
 * triple in the graph.
 */
static int pc_subject_is_pattern(
   const pc_subject_index* index, const rdfa_property_copy_state* state,
   const char* subject)
{
   pc_subject_entry* entry = pc_index_lookup(index, subject);
   size_t i;

   if(entry == NULL)
   {
      return 0;
   }

   for(i = 0; i < entry->count; i++)
   {
      if(pc_is_pattern_type_triple(state->triples[entry->indices[i]]))
      {
         return 1;
      }
   }
   return 0;
}

/* ------------------------------------------------------------------ */
/* Copy closure                                                        */
/* ------------------------------------------------------------------ */

/**
 * Applies property copying until no new triple is produced.  Each round
 * scans the whole graph, so definitions may appear before or after the
 * copy relation, and chained or mutually recursive patterns converge.
 */
static void pc_compute_closure(
   rdfa_property_copy_state* state, pc_subject_index* index)
{
   int changed;

   do
   {
      size_t edge_index;
      changed = 0;

      for(edge_index = 0; edge_index < state->count; edge_index++)
      {
         rdftriple* edge = state->triples[edge_index];
         const char* target;
         pc_subject_entry* target_triples;
         size_t i;

         if(!pc_is_copy_edge(edge))
         {
            continue;
         }

         target = edge->object;
         if(!pc_subject_is_pattern(index, state, target))
         {
            continue;
         }

         target_triples = pc_index_lookup(index, target);
         if(target_triples == NULL)
         {
            continue;
         }

         for(i = 0; i < target_triples->count; i++)
         {
            rdftriple* source = state->triples[target_triples->indices[i]];
            rdftriple* copy;
            size_t copy_index;

            if(!pc_string_equal(source->subject, target))
            {
               continue;
            }

            copy = rdfa_create_triple(
               edge->subject, source->predicate, source->object,
               source->object_type, source->datatype, source->language);
            if(copy == NULL)
            {
               continue;
            }

            copy_index = pc_store(state, copy);
            if(copy_index == PC_NO_INDEX)
            {
               continue;
            }

            if(!pc_index_add(index, copy->subject, copy_index))
            {
               /* Out of memory: stop the fixed point rather than spin. */
               return;
            }
            changed = 1;

            /* appending a triple may have moved the subject entries;
             * re-resolve the target for the next iteration. */
            target_triples = pc_index_lookup(index, target);
            if(target_triples == NULL)
            {
               break;
            }
         }
      }
   }
   while(changed);
}

void rdfa_property_copy_run(
   rdfa_property_copy_state* state, triple_handler_fp callback,
   void* callback_data)
{
   pc_subject_index index;
   unsigned char* removed;
   size_t* copy_edges;
   size_t copy_edge_count = 0;
   size_t i;

   if(state == NULL || state->delivered)
   {
      return;
   }

   if(!pc_index_build(state, &index))
   {
      /* Without an index the graph cannot be processed safely.  Mark the
       * state delivered so the triples are only discarded on free. */
      state->delivered = 1;
      return;
   }

   pc_compute_closure(state, &index);

   removed = (unsigned char*)calloc(
      state->count == 0 ? 1 : state->count, sizeof(unsigned char));
   copy_edges = (size_t*)malloc(
      sizeof(size_t) * (state->count == 0 ? 1 : state->count));
   if(removed == NULL || copy_edges == NULL)
   {
      free(removed);
      free(copy_edges);
      pc_index_free(&index);
      state->delivered = 1;
      return;
   }

   /* Determine every effective copy relation from the closed, not yet
    * pruned, graph.  A relation is effective when its target currently
    * has rdf:type rdfa:Pattern. */
   for(i = 0; i < state->count; i++)
   {
      rdftriple* triple = state->triples[i];
      if(pc_is_copy_edge(triple) &&
         pc_subject_is_pattern(&index, state, triple->object))
      {
         copy_edges[copy_edge_count++] = i;
      }
   }

   for(i = 0; i < copy_edge_count; i++)
   {
      rdftriple* edge = state->triples[copy_edges[i]];
      const char* subject = edge->subject;
      const char* target = edge->object;
      pc_subject_entry* target_triples;
      pc_subject_entry* subject_triples;
      size_t j;

      /* Remove the copy relation itself. */
      removed[copy_edges[i]] = 1;

      /* Remove all outgoing edges of the target pattern. */
      target_triples = pc_index_lookup(&index, target);
      if(target_triples != NULL)
      {
         for(j = 0; j < target_triples->count; j++)
         {
            removed[target_triples->indices[j]] = 1;
         }
      }

      /* The subject of a copy is not itself a pattern anymore. */
      subject_triples = pc_index_lookup(&index, subject);
      if(subject_triples != NULL)
      {
         for(j = 0; j < subject_triples->count; j++)
         {
            size_t triple_index = subject_triples->indices[j];
            if(pc_is_pattern_type_triple(state->triples[triple_index]))
            {
               removed[triple_index] = 1;
            }
         }
      }
   }

   free(copy_edges);
   pc_index_free(&index);

   /* With the closure and pruning complete, deliver the remaining graph.
    * Ownership of each triple passes to the handler. */
   state->delivered = 1;

   for(i = 0; i < state->count; i++)
   {
      if(removed[i] && state->triples[i] != NULL)
      {
         rdfa_free_triple(state->triples[i]);
         state->triples[i] = NULL;
      }
   }
   free(removed);

   for(i = 0; i < state->count; i++)
   {
      rdftriple* triple = state->triples[i];
      if(triple == NULL)
      {
         continue;
      }
      state->triples[i] = NULL;
      if(callback != NULL)
      {
         callback(triple, callback_data);
      }
      else
      {
         rdfa_free_triple(triple);
      }
   }
}

rdfa_property_copy_state* rdfa_property_copy_state_create(void)
{
   rdfa_property_copy_state* state =
      (rdfa_property_copy_state*)calloc(1, sizeof(rdfa_property_copy_state));

   if(state == NULL)
   {
      return NULL;
   }

   state->table = (pc_table_entry*)calloc(
      PC_INITIAL_TABLE_SIZE, sizeof(pc_table_entry));
   if(state->table == NULL)
   {
      free(state);
      return NULL;
   }
   state->table_size = PC_INITIAL_TABLE_SIZE;

   return state;
}

int rdfa_property_copy_is_delivered(
   const rdfa_property_copy_state* state)
{
   return state != NULL && state->delivered;
}

void rdfa_property_copy_state_free(rdfa_property_copy_state* state)
{
   size_t i;
   if(state == NULL)
   {
      return;
   }

   for(i = 0; i < state->count; i++)
   {
      if(state->triples[i] != NULL)
      {
         rdfa_free_triple(state->triples[i]);
      }
   }
   free(state->triples);
   free(state->table);
   free(state);
}

/* ------------------------------------------------------------------ */
/* Default graph routing                                               */
/* ------------------------------------------------------------------ */

void rdfa_emit_default_triple(rdfacontext* context, rdftriple* triple)
{
   if(triple == NULL)
   {
      return;
   }

   if(context->property_copying)
   {
      if(context->property_copy_state != NULL)
      {
         rdfa_property_copy_collect(context->property_copy_state, triple);
      }
      else
      {
         /* The buffering option is enabled but the graph is unavailable;
          * dropping the triple is safer than delivering it early. */
         rdfa_free_triple(triple);
      }
      return;
   }

   if(context->default_graph_triple_callback != NULL)
   {
      context->default_graph_triple_callback(triple, context->callback_data);
   }
   else
   {
      rdfa_free_triple(triple);
   }
}
