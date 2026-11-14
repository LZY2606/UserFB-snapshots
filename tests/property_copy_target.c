/*
 * Target tests for HTML+RDFa 1.1 property copying.
 *
 * The tests exercise the three required aspects of the optional mode:
 *
 *   copy_closure   - forward declarations, chains and mutual references
 *                    converge to the same fixed point.
 *   copy_terms     - IRI/bnode identity, literal equality components and
 *                    shared list chains survive copying without merging.
 *   copy_lifecycle - every end-of-input path shares one finalization
 *                    semantics, and contexts never share a graph.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rdfa.h"
#include "rdfa_utils.h"
#include "task_support/check.h"

#define MAX_TRIPLES 256

typedef struct test_triple
{
   char* subject;
   char* predicate;
   char* object;
   rdfresource_t object_type;
   char* datatype;
   char* language;
} test_triple;

typedef struct triple_bag
{
   test_triple items[MAX_TRIPLES];
   size_t count;
   size_t overflow;
} triple_bag;

static char* test_strdup(const char* value)
{
   size_t length;
   char* copy;

   if(value == NULL)
   {
      return NULL;
   }

   length = strlen(value);
   copy = (char*)malloc(length + 1);
   if(copy != NULL)
   {
      memcpy(copy, value, length + 1);
   }
   return copy;
}

static void bag_init(triple_bag* bag)
{
   memset(bag, 0, sizeof(triple_bag));
}

static void bag_add(triple_bag* bag, const rdftriple* triple)
{
   test_triple* item;

   if(bag->count >= MAX_TRIPLES)
   {
      bag->overflow++;
      return;
   }

   item = &bag->items[bag->count++];
   item->subject = test_strdup(triple->subject);
   item->predicate = test_strdup(triple->predicate);
   item->object = test_strdup(triple->object);
   item->object_type = triple->object_type;
   item->datatype = test_strdup(triple->datatype);
   item->language = test_strdup(triple->language);
}

static void bag_free(triple_bag* bag)
{
   size_t i;
   for(i = 0; i < bag->count; i++)
   {
      free(bag->items[i].subject);
      free(bag->items[i].predicate);
      free(bag->items[i].object);
      free(bag->items[i].datatype);
      free(bag->items[i].language);
   }
   bag->count = 0;
}

/* The handler takes ownership of the triple, as the public contract
 * requires, and frees it immediately. */
static void collect_triple(rdftriple* triple, void* callback_data)
{
   bag_add((triple_bag*)callback_data, triple);
   rdfa_free_triple(triple);
}

static int string_is(const char* value, const char* expected)
{
   if(value == NULL || expected == NULL)
   {
      return value == expected;
   }
   return strcmp(value, expected) == 0;
}

static int bag_has(
   const triple_bag* bag, const char* subject, const char* predicate,
   const char* object)
{
   size_t i;
   for(i = 0; i < bag->count; i++)
   {
      const test_triple* item = &bag->items[i];
      if(string_is(item->subject, subject) &&
         string_is(item->predicate, predicate) &&
         string_is(item->object, object))
      {
         return 1;
      }
   }
   return 0;
}

static int bag_has_literal(
   const triple_bag* bag, const char* subject, const char* predicate,
   const char* object, rdfresource_t object_type,
   const char* datatype, const char* language)
{
   size_t i;
   for(i = 0; i < bag->count; i++)
   {
      const test_triple* item = &bag->items[i];
      if(string_is(item->subject, subject) &&
         string_is(item->predicate, predicate) &&
         string_is(item->object, object) &&
         item->object_type == object_type &&
         string_is(item->datatype, datatype) &&
         string_is(item->language, language))
      {
         return 1;
      }
   }
   return 0;
}

static size_t bag_count_predicate(const triple_bag* bag, const char* predicate)
{
   size_t i;
   size_t count = 0;
   for(i = 0; i < bag->count; i++)
   {
      if(string_is(bag->items[i].predicate, predicate))
      {
         count++;
      }
   }
   return count;
}

static size_t bag_count_subject(const triple_bag* bag, const char* subject)
{
   size_t i;
   size_t count = 0;
   for(i = 0; i < bag->count; i++)
   {
      if(string_is(bag->items[i].subject, subject))
      {
         count++;
      }
   }
   return count;
}

static size_t bag_count(const triple_bag* bag, const char* subject,
   const char* predicate, const char* object)
{
   size_t i;
   size_t count = 0;
   for(i = 0; i < bag->count; i++)
   {
      const test_triple* item = &bag->items[i];
      if(string_is(item->subject, subject) &&
         string_is(item->predicate, predicate) &&
         string_is(item->object, object))
      {
         count++;
      }
   }
   return count;
}

static int bag_equal(const triple_bag* left, const triple_bag* right)
{
   size_t i;

   if(left->count != right->count || left->overflow || right->overflow)
   {
      return 0;
   }

   for(i = 0; i < left->count; i++)
   {
      const test_triple* item = &left->items[i];
      if(!bag_has_literal(right, item->subject, item->predicate, item->object,
            item->object_type, item->datatype, item->language))
      {
         return 0;
      }
   }
   return 1;
}

static int bag_has_any_bnode_object(
   const triple_bag* bag, const char* subject, const char* predicate)
{
   size_t i;
   for(i = 0; i < bag->count; i++)
   {
      const test_triple* item = &bag->items[i];
      if(string_is(item->subject, subject) &&
         string_is(item->predicate, predicate) &&
         item->object != NULL && item->object[0] == '_' &&
         item->object[1] == ':')
      {
         return 1;
      }
   }
   return 0;
}

static const test_triple* bag_find(
   const triple_bag* bag, const char* subject, const char* predicate)
{
   size_t i;
   for(i = 0; i < bag->count; i++)
   {
      const test_triple* item = &bag->items[i];
      if(string_is(item->subject, subject) &&
         string_is(item->predicate, predicate))
      {
         return item;
      }
   }
   return NULL;
}

/* ------------------------------------------------------------------ */
/* Documents                                                           */
/* ------------------------------------------------------------------ */

#define PREFIX_HEAD \
   "<html xmlns=\"http://www.w3.org/1999/xhtml\"" \
   " version=\"XHTML+RDFa 1.1\"" \
   " prefix=\"rdfa: http://www.w3.org/ns/rdfa# ex: https://v.test/" \
   " xsd: http://www.w3.org/2001/XMLSchema#\">" \
   "<head><title>Test</title></head><body>"

#define DOC_END "</body></html>"

/* Copy relation appears before the pattern definition. */
static const char* DOC_FORWARD =
   PREFIX_HEAD
   "<div about=\"https://doc.test/event\">"
   "<link property=\"rdfa:copy\" href=\"https://doc.test/venue\"/></div>"
   "<div about=\"https://doc.test/venue\" typeof=\"rdfa:Pattern\">"
   "<span property=\"ex:name\">Town Hall</span>"
   "<span property=\"ex:capacity\">400</span></div>"
   DOC_END;

/* Pattern definition appears before the copy relation. */
static const char* DOC_BACKWARD =
   PREFIX_HEAD
   "<div about=\"https://doc.test/venue\" typeof=\"rdfa:Pattern\">"
   "<span property=\"ex:name\">Town Hall</span>"
   "<span property=\"ex:capacity\">400</span></div>"
   "<div about=\"https://doc.test/event\">"
   "<link property=\"rdfa:copy\" href=\"https://doc.test/venue\"/></div>"
   DOC_END;

/* event -> a -> b, with the payload only on b. */
static const char* DOC_CHAIN =
   PREFIX_HEAD
   "<div about=\"https://doc.test/event\">"
   "<link property=\"rdfa:copy\" href=\"https://doc.test/a\"/></div>"
   "<div about=\"https://doc.test/a\" typeof=\"rdfa:Pattern\">"
   "<link property=\"rdfa:copy\" href=\"https://doc.test/b\"/></div>"
   "<div about=\"https://doc.test/b\" typeof=\"rdfa:Pattern\">"
   "<span property=\"ex:name\">Town Hall</span>"
   "<span property=\"ex:capacity\">400</span></div>"
   DOC_END;

/* event -> a <-> b, each with its own payload. */
static const char* DOC_CYCLE =
   PREFIX_HEAD
   "<div about=\"https://doc.test/event\">"
   "<link property=\"rdfa:copy\" href=\"https://doc.test/a\"/></div>"
   "<div about=\"https://doc.test/a\" typeof=\"rdfa:Pattern\">"
   "<link property=\"rdfa:copy\" href=\"https://doc.test/b\"/>"
   "<span property=\"ex:aname\">A</span></div>"
   "<div about=\"https://doc.test/b\" typeof=\"rdfa:Pattern\">"
   "<link property=\"rdfa:copy\" href=\"https://doc.test/a\"/>"
   "<span property=\"ex:bname\">B</span></div>"
   DOC_END;

/* Same text with distinct language/datatype, plus a plain literal. */
static const char* DOC_TERMS =
   PREFIX_HEAD
   "<div about=\"https://doc.test/event\">"
   "<link property=\"rdfa:copy\" href=\"https://doc.test/pattern\"/></div>"
   "<div about=\"https://doc.test/pattern\" typeof=\"rdfa:Pattern\">"
   "<span property=\"ex:name\" xml:lang=\"en\">Hall</span>"
   "<span property=\"ex:name\" xml:lang=\"fr\">Salle</span>"
   "<span property=\"ex:name\" datatype=\"xsd:string\">Hall</span>"
   "<span property=\"ex:name\" datatype=\"xsd:integer\">5</span>"
   "<span property=\"ex:name\">Hall</span>"
   "</div>"
   DOC_END;

/* A list with two identical members and a different third one. */
static const char* DOC_LIST =
   PREFIX_HEAD
   "<div about=\"https://doc.test/event\">"
   "<link property=\"rdfa:copy\" href=\"https://doc.test/pattern\"/></div>"
   "<div about=\"https://doc.test/pattern\" typeof=\"rdfa:Pattern\">"
   "<span property=\"ex:tag\" inlist=\"\">A</span>"
   "<span property=\"ex:tag\" inlist=\"\">A</span>"
   "<span property=\"ex:tag\" inlist=\"\">B</span>"
   "</div>"
   DOC_END;

/* A pattern nested several levels deep, ensuring element closing does
 * not release the page graph early. */
static const char* DOC_NESTED =
   PREFIX_HEAD
   "<div about=\"https://doc.test/event\">"
   "<link property=\"rdfa:copy\" href=\"https://doc.test/pattern\"/></div>"
   "<div about=\"https://doc.test/pattern\" typeof=\"rdfa:Pattern\">"
   "<div><div><div><span property=\"ex:name\">Town Hall</span></div></div>"
   "</div></div>"
   DOC_END;

/* A second page with a different event/venue, used to detect crosstalk. */
static const char* DOC_OTHER =
   PREFIX_HEAD
   "<div about=\"https://doc.test/other-event\">"
   "<link property=\"rdfa:copy\" href=\"https://doc.test/other-venue\"/></div>"
   "<div about=\"https://doc.test/other-venue\" typeof=\"rdfa:Pattern\">"
   "<span property=\"ex:name\">Arena</span></div>"
   DOC_END;

#define COPY_IRI "http://www.w3.org/ns/rdfa#copy"
#define TYPE_IRI "http://www.w3.org/1999/02/22-rdf-syntax-ns#type"
#define PATTERN_IRI "http://www.w3.org/ns/rdfa#Pattern"
#define FIRST_IRI "http://www.w3.org/1999/02/22-rdf-syntax-ns#first"
#define REST_IRI "http://www.w3.org/1999/02/22-rdf-syntax-ns#rest"
#define NIL_IRI "http://www.w3.org/1999/02/22-rdf-syntax-ns#nil"

/* ------------------------------------------------------------------ */
/* Driving helpers                                                     */
/* ------------------------------------------------------------------ */

static int run_chunks(const char* document, size_t split, int enable_copy,
   triple_bag* bag)
{
   size_t length = strlen(document);
   rdfacontext* context;
   int status;

   bag_init(bag);
   context = rdfa_create_context("https://doc.test/page");
   if(context == NULL)
   {
      return RDFA_PARSE_FAILED;
   }
   context->callback_data = bag;
   rdfa_set_default_graph_triple_handler(context, collect_triple);
   rdfa_set_property_copying(context, enable_copy);

   status = rdfa_parse_start(context);
   if(status == RDFA_PARSE_SUCCESS)
   {
      if(split >= length)
      {
         status = rdfa_parse_chunk(context, (char*)document, length, 1);
      }
      else
      {
         status = rdfa_parse_chunk(context, (char*)document, split, 0);
         if(status == RDFA_PARSE_SUCCESS)
         {
            status = rdfa_parse_chunk(
               context, (char*)document + split, length - split, 1);
         }
      }
   }
   rdfa_parse_end(context);
   rdfa_free_context(context);
   return status;
}

static int run_parse_buffer(
   const char* document, size_t split, triple_bag* bag)
{
   size_t length = strlen(document);
   rdfacontext* context;
   int status;

   bag_init(bag);
   context = rdfa_create_context("https://doc.test/page");
   if(context == NULL)
   {
      return RDFA_PARSE_FAILED;
   }
   context->callback_data = bag;
   rdfa_set_default_graph_triple_handler(context, collect_triple);
   rdfa_set_property_copying(context, 1);

   status = rdfa_parse_start(context);
   if(status == RDFA_PARSE_SUCCESS)
   {
      size_t buffer_length = 0;
      char* buffer = rdfa_get_buffer(context, &buffer_length);

      if(split > length)
      {
         split = length;
      }
      memcpy(buffer, document, split);
      status = rdfa_parse_buffer(context, split);
      if(status == RDFA_PARSE_SUCCESS)
      {
         memcpy(buffer, document + split, length - split);
         status = rdfa_parse_buffer(context, length - split);
      }
      if(status == RDFA_PARSE_SUCCESS)
      {
         status = rdfa_parse_buffer(context, 0);
      }
   }
   rdfa_parse_end(context);
   rdfa_free_context(context);
   return status;
}

typedef struct filler_state
{
   const char* document;
   size_t offset;
   triple_bag* bag;
} filler_state;

static size_t fill_buffer(char* buffer, size_t length, void* callback_data)
{
   filler_state* state = (filler_state*)callback_data;
   size_t total = strlen(state->document);
   size_t remaining = total - state->offset;
   size_t take = remaining < length ? remaining : length;

   if(take > 0)
   {
      memcpy(buffer, state->document + state->offset, take);
      state->offset += take;
   }
   return take;
}

/* rdfa_parse() uses one callback_data pointer for both the buffer filler
 * and the triple handler, so the graph lives inside the filler state. */
static void collect_from_filler(rdftriple* triple, void* callback_data)
{
   filler_state* state = (filler_state*)callback_data;
   bag_add(state->bag, triple);
   rdfa_free_triple(triple);
}

static int run_parse(const char* document, triple_bag* bag)
{
   filler_state state;
   rdfacontext* context;
   int status;

   bag_init(bag);
   state.document = document;
   state.offset = 0;
   state.bag = bag;

   context = rdfa_create_context("https://doc.test/page");
   if(context == NULL)
   {
      return RDFA_PARSE_FAILED;
   }
   context->callback_data = &state;
   rdfa_set_default_graph_triple_handler(context, collect_from_filler);
   rdfa_set_buffer_filler(context, fill_buffer);
   rdfa_set_property_copying(context, 1);

   status = rdfa_parse(context);
   rdfa_free_context(context);
   return status;
}

/* The copied pattern is also a copy subject of another pattern.  Its
 * marker is removed as a copy subject, but it must still be treated as
 * a pattern target when pruning. */
static const char* DOC_NESTED_COPY =
   PREFIX_HEAD
   "<div about=\"https://doc.test/event\">"
   "<link property=\"rdfa:copy\" href=\"https://doc.test/a\"/></div>"
   "<div about=\"https://doc.test/a\" typeof=\"rdfa:Pattern\">"
   "<link property=\"rdfa:copy\" href=\"https://doc.test/b\"/>"
   "<span property=\"ex:aname\">A</span></div>"
   "<div about=\"https://doc.test/b\" typeof=\"rdfa:Pattern\">"
   "<span property=\"ex:name\">Town Hall</span></div>"
   DOC_END;

/* A copy relation that only appears after another copy has run: the
 * payload on x is reachable through a copy edge that p contributes. */
static const char* DOC_PROPAGATED =
   PREFIX_HEAD
   "<div about=\"https://doc.test/event\">"
   "<link property=\"rdfa:copy\" href=\"https://doc.test/p\"/></div>"
   "<div about=\"https://doc.test/p\" typeof=\"rdfa:Pattern\">"
   "<link property=\"rdfa:copy\" href=\"https://doc.test/x\"/></div>"
   "<div about=\"https://doc.test/x\" typeof=\"rdfa:Pattern\">"
   "<span property=\"ex:name\">Town Hall</span></div>"
   DOC_END;

/* An unused pattern (no copy edge targets it) and a copy edge whose
 * target is not a pattern. */
static const char* DOC_UNUSED =
   PREFIX_HEAD
   "<div about=\"https://doc.test/unused\" typeof=\"rdfa:Pattern\">"
   "<span property=\"ex:name\">Unused Hall</span></div>"
   "<div about=\"https://doc.test/other\">"
   "<link property=\"rdfa:copy\" href=\"https://doc.test/plain\"/></div>"
   "<div about=\"https://doc.test/plain\">"
   "<span property=\"ex:name\">Plain Hall</span></div>"
   DOC_END;

/* ------------------------------------------------------------------ */
/* copy_closure                                                        */
/* ------------------------------------------------------------------ */

static int case_forward_chain(void)
{
   triple_bag bag;

   TASK_CHECK(run_chunks(DOC_FORWARD, strlen(DOC_FORWARD), 1, &bag) ==
      RDFA_PARSE_SUCCESS);
   TASK_CHECK(bag_has(&bag, "https://doc.test/event",
      "https://v.test/name", "Town Hall"));
   TASK_CHECK(bag_has(&bag, "https://doc.test/event",
      "https://v.test/capacity", "400"));
   TASK_CHECK(bag_count_predicate(&bag, COPY_IRI) == 0);
   TASK_CHECK(bag_count_predicate(&bag, TYPE_IRI) == 0);
   TASK_CHECK(bag_count_subject(&bag, "https://doc.test/venue") == 0);

   bag_free(&bag);
   return 0;
}

static int case_definition_order(void)
{
   triple_bag forward;
   triple_bag backward;

   TASK_CHECK(run_chunks(DOC_FORWARD, strlen(DOC_FORWARD), 1, &forward) ==
      RDFA_PARSE_SUCCESS);
   TASK_CHECK(run_chunks(DOC_BACKWARD, strlen(DOC_BACKWARD), 1, &backward) ==
      RDFA_PARSE_SUCCESS);

   /* Declaration order must not matter: both graphs are identical. */
   TASK_CHECK(forward.count > 0);
   TASK_CHECK(bag_equal(&forward, &backward));

   bag_free(&forward);
   bag_free(&backward);
   return 0;
}

static int case_transitive_chain(void)
{
   triple_bag bag;

   TASK_CHECK(run_chunks(DOC_CHAIN, strlen(DOC_CHAIN), 1, &bag) ==
      RDFA_PARSE_SUCCESS);
   TASK_CHECK(bag_has(&bag, "https://doc.test/event",
      "https://v.test/name", "Town Hall"));
   TASK_CHECK(bag_has(&bag, "https://doc.test/event",
      "https://v.test/capacity", "400"));
   TASK_CHECK(bag_count_predicate(&bag, COPY_IRI) == 0);
   TASK_CHECK(bag_count_predicate(&bag, TYPE_IRI) == 0);
   TASK_CHECK(bag_count_subject(&bag, "https://doc.test/a") == 0);
   TASK_CHECK(bag_count_subject(&bag, "https://doc.test/b") == 0);
   TASK_CHECK(bag.count == 2);

   bag_free(&bag);
   return 0;
}

static int case_mutual_cycle(void)
{
   triple_bag bag;

   TASK_CHECK(run_chunks(DOC_CYCLE, strlen(DOC_CYCLE), 1, &bag) ==
      RDFA_PARSE_SUCCESS);
   TASK_CHECK(bag_has(&bag, "https://doc.test/event",
      "https://v.test/aname", "A"));
   TASK_CHECK(bag_has(&bag, "https://doc.test/event",
      "https://v.test/bname", "B"));
   TASK_CHECK(bag_count_predicate(&bag, COPY_IRI) == 0);
   TASK_CHECK(bag_count_predicate(&bag, TYPE_IRI) == 0);
   TASK_CHECK(bag_count_subject(&bag, "https://doc.test/event") == 2);
   TASK_CHECK(bag.count == 2);

   bag_free(&bag);
   return 0;
}

static int case_pattern_and_copy_removed(void)
{
   triple_bag bag;

   TASK_CHECK(run_chunks(DOC_FORWARD, strlen(DOC_FORWARD), 1, &bag) ==
      RDFA_PARSE_SUCCESS);
   TASK_CHECK(bag_has(&bag, "https://doc.test/event",
      "https://v.test/name", "Town Hall"));
   TASK_CHECK(bag_count_predicate(&bag, COPY_IRI) == 0);
   TASK_CHECK(bag_count_predicate(&bag, TYPE_IRI) == 0);

   bag_free(&bag);
   return 0;
}

static int case_target_also_copies(void)
{
   triple_bag bag;

   TASK_CHECK(run_chunks(DOC_NESTED_COPY, strlen(DOC_NESTED_COPY), 1, &bag) ==
      RDFA_PARSE_SUCCESS);
   TASK_CHECK(bag_has(&bag, "https://doc.test/event",
      "https://v.test/aname", "A"));
   TASK_CHECK(bag_has(&bag, "https://doc.test/event",
      "https://v.test/name", "Town Hall"));
   TASK_CHECK(bag.count == 2);
   TASK_CHECK(bag_count_predicate(&bag, COPY_IRI) == 0);
   TASK_CHECK(bag_count_predicate(&bag, TYPE_IRI) == 0);
   TASK_CHECK(bag_count_subject(&bag, "https://doc.test/a") == 0);
   TASK_CHECK(bag_count_subject(&bag, "https://doc.test/b") == 0);

   bag_free(&bag);
   return 0;
}

static int case_propagated_copy_edge(void)
{
   triple_bag bag;

   TASK_CHECK(run_chunks(DOC_PROPAGATED, strlen(DOC_PROPAGATED), 1, &bag) ==
      RDFA_PARSE_SUCCESS);

   /* event -> p -> x only reaches the payload if the copy edge that p
    * contributes is itself copied and then followed. */
   TASK_CHECK(bag_has(&bag, "https://doc.test/event",
      "https://v.test/name", "Town Hall"));
   TASK_CHECK(bag.count == 1);
   TASK_CHECK(bag_count_predicate(&bag, COPY_IRI) == 0);
   TASK_CHECK(bag_count_predicate(&bag, TYPE_IRI) == 0);
   TASK_CHECK(bag_count_subject(&bag, "https://doc.test/p") == 0);
   TASK_CHECK(bag_count_subject(&bag, "https://doc.test/x") == 0);

   bag_free(&bag);
   return 0;
}

static int case_unused_pattern_preserved(void)
{
   triple_bag bag;

   TASK_CHECK(run_chunks(DOC_UNUSED, strlen(DOC_UNUSED), 1, &bag) ==
      RDFA_PARSE_SUCCESS);

   /* The unused pattern keeps both its type marker and its attribute. */
   TASK_CHECK(bag_has(&bag, "https://doc.test/unused", TYPE_IRI,
      PATTERN_IRI));
   TASK_CHECK(bag_has(&bag, "https://doc.test/unused",
      "https://v.test/name", "Unused Hall"));

   /* A copy edge whose target is not a pattern is left untouched, and
    * the target's attributes are not copied to its subject. */
   TASK_CHECK(bag_has(&bag, "https://doc.test/other", COPY_IRI,
      "https://doc.test/plain"));
   TASK_CHECK(bag_has(&bag, "https://doc.test/plain",
      "https://v.test/name", "Plain Hall"));
   TASK_CHECK(!bag_has(&bag, "https://doc.test/other",
      "https://v.test/name", "Plain Hall"));
   TASK_CHECK(bag_count_subject(&bag, "https://doc.test/event") == 0);
   bag_free(&bag);
   return 0;
}

/* ------------------------------------------------------------------ */
/* copy_terms                                                          */
/* ------------------------------------------------------------------ */

static int case_literal_identity(void)
{
   triple_bag bag;

   TASK_CHECK(run_chunks(DOC_TERMS, strlen(DOC_TERMS), 1, &bag) ==
      RDFA_PARSE_SUCCESS);

   /* Distinct language tags are never merged. */
   TASK_CHECK(bag_has_literal(&bag, "https://doc.test/event",
      "https://v.test/name", "Hall", RDF_TYPE_PLAIN_LITERAL, NULL, "en"));
   TASK_CHECK(bag_has_literal(&bag, "https://doc.test/event",
      "https://v.test/name", "Salle", RDF_TYPE_PLAIN_LITERAL, NULL, "fr"));

   /* Typed literals keep datatype and object type. */
   TASK_CHECK(bag_has_literal(&bag, "https://doc.test/event",
      "https://v.test/name", "Hall", RDF_TYPE_TYPED_LITERAL,
      "http://www.w3.org/2001/XMLSchema#string", NULL));
   TASK_CHECK(bag_has_literal(&bag, "https://doc.test/event",
      "https://v.test/name", "5", RDF_TYPE_TYPED_LITERAL,
      "http://www.w3.org/2001/XMLSchema#integer", NULL));

   /* The same text as a plain literal stays distinct from the typed one. */
   TASK_CHECK(bag_has_literal(&bag, "https://doc.test/event",
      "https://v.test/name", "Hall", RDF_TYPE_PLAIN_LITERAL, NULL, NULL));

   TASK_CHECK(bag_count_predicate(&bag, "https://v.test/name") == 5);
   TASK_CHECK(bag_count_predicate(&bag, COPY_IRI) == 0);
   TASK_CHECK(bag_count_predicate(&bag, TYPE_IRI) == 0);
   TASK_CHECK(bag.count == 5);

   bag_free(&bag);
   return 0;
}

static int case_shared_list_chain(void)
{
   triple_bag bag;
   const test_triple* head;
   const char* head_name;
   size_t first_a = 0;
   size_t first_b = 0;
   size_t rest_count;
   size_t steps = 0;
   const char* cursor;
   char* visited[8];

   TASK_CHECK(run_chunks(DOC_LIST, strlen(DOC_LIST), 1, &bag) ==
      RDFA_PARSE_SUCCESS);

   /* Exactly one list-head triple belongs to the event and it points at
    * a shared blank node, not at a newly built copy of the list. */
   TASK_CHECK(bag_count(&bag, "https://doc.test/event",
      "https://v.test/tag", "_:bnode0") == 1);
   TASK_CHECK(bag_has_any_bnode_object(&bag, "https://doc.test/event",
      "https://v.test/tag"));
   TASK_CHECK(bag_count_subject(&bag, "https://doc.test/pattern") == 0);

   /* Duplicate members are not collapsed by value. */
   {
      size_t i;
      for(i = 0; i < bag.count; i++)
      {
         if(string_is(bag.items[i].predicate, FIRST_IRI))
         {
            if(string_is(bag.items[i].object, "A"))
            {
               first_a++;
            }
            else if(string_is(bag.items[i].object, "B"))
            {
               first_b++;
            }
         }
      }
   }
   TASK_CHECK(first_a == 2);
   TASK_CHECK(first_b == 1);
   TASK_CHECK(bag_count_predicate(&bag, FIRST_IRI) == 3);

   /* The original chain is reused: walk first/rest from the head to nil
    * and observe every member exactly once with three rest links. */
   head = bag_find(&bag, "https://doc.test/event", "https://v.test/tag");
   TASK_CHECK(head != NULL);
   if(head == NULL)
   {
      bag_free(&bag);
      return 0;
   }
   head_name = head->object;
   cursor = head_name;
   TASK_CHECK(bag_has(&bag, cursor, FIRST_IRI, "A") ||
      bag_has(&bag, cursor, FIRST_IRI, "B"));

   memset(visited, 0, sizeof(visited));
   while(steps < 8 && !string_is(cursor, NIL_IRI))
   {
      const test_triple* rest = bag_find(&bag, cursor, REST_IRI);
      size_t i;
      int seen = 0;

      TASK_CHECK(rest != NULL);
      if(rest == NULL)
      {
         break;
      }
      TASK_CHECK(bag_find(&bag, cursor, FIRST_IRI) != NULL);
      for(i = 0; i < steps; i++)
      {
         if(string_is(visited[i], cursor))
         {
            seen = 1;
         }
      }
      TASK_CHECK(!seen);
      visited[steps] = (char*)cursor;
      steps++;
      cursor = rest->object;
   }
   TASK_CHECK(string_is(cursor, NIL_IRI));
   TASK_CHECK(steps == 3);

   rest_count = bag_count_predicate(&bag, REST_IRI);
   TASK_CHECK(rest_count == 3);
   TASK_CHECK(bag_count_predicate(&bag, COPY_IRI) == 0);
   TASK_CHECK(bag_count_predicate(&bag, TYPE_IRI) == 0);

   bag_free(&bag);
   return 0;
}

static int case_bnode_identity(void)
{
   triple_bag bag;
   const test_triple* head;
   const test_triple* first;
   const test_triple* rest;

   TASK_CHECK(run_chunks(DOC_LIST, strlen(DOC_LIST), 1, &bag) ==
      RDFA_PARSE_SUCCESS);

   head = bag_find(&bag, "https://doc.test/event", "https://v.test/tag");
   TASK_CHECK(head != NULL);
   if(head == NULL)
   {
      bag_free(&bag);
      return 0;
   }

   /* The list head keeps its blank-node identity, so the chain the event
    * points at is the very chain the pattern defined. */
   first = bag_find(&bag, head->object, FIRST_IRI);
   rest = bag_find(&bag, head->object, REST_IRI);
   TASK_CHECK(first != NULL);
   TASK_CHECK(rest != NULL);
   TASK_CHECK(first != NULL && string_is(first->object, "A"));
   TASK_CHECK(rest != NULL && rest->object[0] == '_' && rest->object[1] == ':');
   TASK_CHECK(rest != NULL &&
      bag_find(&bag, rest->object, FIRST_IRI) != NULL);

   bag_free(&bag);
   return 0;
}

/* ------------------------------------------------------------------ */
/* copy_lifecycle                                                      */
/* ------------------------------------------------------------------ */

static int case_split_and_buffer_paths(void)
{
   triple_bag whole;
   triple_bag split;
   triple_bag buffered;
   triple_bag single;

   TASK_CHECK(run_chunks(DOC_FORWARD, strlen(DOC_FORWARD), 1, &whole) ==
      RDFA_PARSE_SUCCESS);
   TASK_CHECK(whole.count == 2);

   TASK_CHECK(run_chunks(DOC_FORWARD, 37, 1, &split) == RDFA_PARSE_SUCCESS);
   TASK_CHECK(bag_equal(&whole, &split));

   TASK_CHECK(run_parse_buffer(DOC_FORWARD, 41, &buffered) ==
      RDFA_PARSE_SUCCESS);
   TASK_CHECK(bag_equal(&whole, &buffered));

   /* First chunk already carrying done=1. */
   TASK_CHECK(run_chunks(DOC_FORWARD, strlen(DOC_FORWARD), 1, &single) ==
      RDFA_PARSE_SUCCESS);
   TASK_CHECK(bag_equal(&whole, &single));

   bag_free(&whole);
   bag_free(&split);
   bag_free(&buffered);
   bag_free(&single);
   return 0;
}

static int case_parse_convenience_path(void)
{
   triple_bag bag;
   triple_bag whole;

   TASK_CHECK(run_chunks(DOC_NESTED, strlen(DOC_NESTED), 1, &whole) ==
      RDFA_PARSE_SUCCESS);
   TASK_CHECK(whole.count == 1);
   TASK_CHECK(bag_has(&whole, "https://doc.test/event",
      "https://v.test/name", "Town Hall"));

   TASK_CHECK(run_parse(DOC_NESTED, &bag) == RDFA_PARSE_SUCCESS);
   TASK_CHECK(bag_equal(&whole, &bag));

   bag_free(&bag);
   bag_free(&whole);
   return 0;
}

static int case_parse_end_does_not_redeliver(void)
{
   rdfacontext* context;
   triple_bag bag;
   size_t count_after_end;
   size_t length = strlen(DOC_FORWARD);

   bag_init(&bag);
   context = rdfa_create_context("https://doc.test/page");
   TASK_CHECK(context != NULL);
   if(context == NULL)
   {
      return 0;
   }
   context->callback_data = &bag;
   rdfa_set_default_graph_triple_handler(context, collect_triple);
   rdfa_set_property_copying(context, 1);

   TASK_CHECK(rdfa_parse_start(context) == RDFA_PARSE_SUCCESS);
   TASK_CHECK(rdfa_parse_chunk(context, (char*)DOC_FORWARD, length, 1) ==
      RDFA_PARSE_SUCCESS);

   /* The graph is delivered on the successful end of input, before
    * rdfa_parse_end() is called. */
   TASK_CHECK(bag.count == 2);

   rdfa_parse_end(context);
   count_after_end = bag.count;
   TASK_CHECK(count_after_end == 2);
   TASK_CHECK(bag_has(&bag, "https://doc.test/event",
      "https://v.test/name", "Town Hall"));

   rdfa_free_context(context);
   bag_free(&bag);
   return 0;
}

static int case_incomplete_input_discarded(void)
{
   rdfacontext* context;
   triple_bag bag;
   size_t length = strlen(DOC_FORWARD);

   bag_init(&bag);
   context = rdfa_create_context("https://doc.test/page");
   TASK_CHECK(context != NULL);
   if(context == NULL)
   {
      return 0;
   }
   context->callback_data = &bag;
   rdfa_set_default_graph_triple_handler(context, collect_triple);
   rdfa_set_property_copying(context, 1);

   TASK_CHECK(rdfa_parse_start(context) == RDFA_PARSE_SUCCESS);

   /* A partial input must not call the default graph handler at all. */
   TASK_CHECK(rdfa_parse_chunk(context, (char*)DOC_FORWARD, length / 2, 0) ==
      RDFA_PARSE_SUCCESS);
   TASK_CHECK(bag.count == 0);

   /* Releasing without a successful end discards the buffered graph. */
   rdfa_parse_end(context);
   rdfa_free_context(context);
   TASK_CHECK(bag.count == 0);

   bag_free(&bag);
   return 0;
}

static int case_parse_error_discarded(void)
{
   triple_bag bag;
   int status;

   bag_init(&bag);
   {
      rdfacontext* context = rdfa_create_context("https://doc.test/page");
      TASK_CHECK(context != NULL);
      if(context == NULL)
      {
         return 0;
      }
      context->callback_data = &bag;
      rdfa_set_default_graph_triple_handler(context, collect_triple);
      rdfa_set_property_copying(context, 1);

      TASK_CHECK(rdfa_parse_start(context) == RDFA_PARSE_SUCCESS);
      /* The document is missing its closing </html>: malformed input. */
      status = rdfa_parse_chunk(
         context, (char*)DOC_FORWARD, strlen(DOC_FORWARD) - 7, 1);
      rdfa_parse_end(context);
      rdfa_free_context(context);
   }

   TASK_CHECK(status != RDFA_PARSE_SUCCESS);
   TASK_CHECK(bag.count == 0);

   bag_free(&bag);
   return 0;
}

static int case_interleaved_contexts(void)
{
   triple_bag first;
   triple_bag second;
   rdfacontext* one;
   rdfacontext* two;
   size_t length_one = strlen(DOC_FORWARD);
   size_t length_two = strlen(DOC_OTHER);

   bag_init(&first);
   bag_init(&second);

   one = rdfa_create_context("https://doc.test/page");
   two = rdfa_create_context("https://doc.test/page");
   TASK_CHECK(one != NULL && two != NULL);
   if(one == NULL || two == NULL)
   {
      rdfa_free_context(one);
      rdfa_free_context(two);
      return 0;
   }

   one->callback_data = &first;
   two->callback_data = &second;
   rdfa_set_default_graph_triple_handler(one, collect_triple);
   rdfa_set_default_graph_triple_handler(two, collect_triple);
   rdfa_set_property_copying(one, 1);
   rdfa_set_property_copying(two, 1);

   TASK_CHECK(rdfa_parse_start(one) == RDFA_PARSE_SUCCESS);
   TASK_CHECK(rdfa_parse_start(two) == RDFA_PARSE_SUCCESS);

   TASK_CHECK(rdfa_parse_chunk(one, (char*)DOC_FORWARD, 33, 0) ==
      RDFA_PARSE_SUCCESS);
   TASK_CHECK(rdfa_parse_chunk(two, (char*)DOC_OTHER, 29, 0) ==
      RDFA_PARSE_SUCCESS);
   TASK_CHECK(rdfa_parse_chunk(
      one, (char*)DOC_FORWARD + 33, length_one - 33, 1) ==
      RDFA_PARSE_SUCCESS);
   TASK_CHECK(rdfa_parse_chunk(
      two, (char*)DOC_OTHER + 29, length_two - 29, 1) ==
      RDFA_PARSE_SUCCESS);

   rdfa_parse_end(one);
   rdfa_parse_end(two);
   rdfa_free_context(one);
   rdfa_free_context(two);

   /* Each context delivered its own event and no trace of the other. */
   TASK_CHECK(bag_has(&first, "https://doc.test/event",
      "https://v.test/name", "Town Hall"));
   TASK_CHECK(bag_has(&second, "https://doc.test/other-event",
      "https://v.test/name", "Arena"));
   TASK_CHECK(bag_count_subject(&first, "https://doc.test/other-event") == 0);
   TASK_CHECK(bag_count_subject(&second, "https://doc.test/event") == 0);
   TASK_CHECK(first.count == 2);
   TASK_CHECK(second.count == 1);

   bag_free(&first);
   bag_free(&second);
   return 0;
}

static int case_disabled_streaming_unchanged(void)
{
   triple_bag bag;

   /* With the option off the existing streaming shape is preserved:
    * the copy relation and the pattern definition are delivered. */
   TASK_CHECK(run_chunks(DOC_FORWARD, strlen(DOC_FORWARD), 0, &bag) ==
      RDFA_PARSE_SUCCESS);
   TASK_CHECK(bag_has(&bag, "https://doc.test/event", COPY_IRI,
      "https://doc.test/venue"));
   TASK_CHECK(bag_has(&bag, "https://doc.test/venue", TYPE_IRI,
      PATTERN_IRI));
   TASK_CHECK(bag_has(&bag, "https://doc.test/venue",
      "https://v.test/name", "Town Hall"));
   TASK_CHECK(bag_has(&bag, "https://doc.test/venue",
      "https://v.test/capacity", "400"));
   TASK_CHECK(bag.count == 4);

   bag_free(&bag);
   return 0;
}

const struct task_case task_cases[] =
{
   {"copy_closure", "forward_chain", case_forward_chain},
   {"copy_closure", "definition_order", case_definition_order},
   {"copy_closure", "transitive_chain", case_transitive_chain},
   {"copy_closure", "mutual_cycle", case_mutual_cycle},
   {"copy_closure", "pattern_and_copy_removed", case_pattern_and_copy_removed},
   {"copy_closure", "target_also_copies", case_target_also_copies},
   {"copy_closure", "propagated_copy_edge", case_propagated_copy_edge},
   {"copy_closure", "unused_pattern_preserved", case_unused_pattern_preserved},
   {"copy_terms", "literal_identity", case_literal_identity},
   {"copy_terms", "shared_list_chain", case_shared_list_chain},
   {"copy_terms", "bnode_identity", case_bnode_identity},
   {"copy_lifecycle", "split_and_buffer_paths", case_split_and_buffer_paths},
   {"copy_lifecycle", "parse_convenience_path", case_parse_convenience_path},
   {"copy_lifecycle", "parse_end_does_not_redeliver",
      case_parse_end_does_not_redeliver},
   {"copy_lifecycle", "incomplete_input_discarded",
      case_incomplete_input_discarded},
   {"copy_lifecycle", "parse_error_discarded", case_parse_error_discarded},
   {"copy_lifecycle", "interleaved_contexts", case_interleaved_contexts},
   {"copy_lifecycle", "disabled_streaming_unchanged",
      case_disabled_streaming_unchanged}
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
