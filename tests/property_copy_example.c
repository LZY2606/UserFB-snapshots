/*
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
 * A small example that parses an HTML+RDFa 1.1 document in property
 * copying mode.  Documents such as event pages routinely reuse venue,
 * ticket and similar attributes through rdfa:Pattern and rdfa:copy;
 * enabling property copying makes the copied attributes appear on the
 * subject that references the pattern, and removes the pattern
 * definitions themselves from the delivered graph.
 *
 * Build (from the librdfa source directory):
 *
 *    cc -std=c99 -Ic -Ivendor/libxml2/include example.c \
 *       librdfa.a libxml2.a -lm -lpthread -o property_copy
 *
 * Usage:
 *
 *    ./property_copy [input.html]
 *
 * With no argument a built-in sample document is used.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rdfa.h"
#include "rdfa_utils.h"

static const char* SAMPLE_DOCUMENT =
   "<html xmlns=\"http://www.w3.org/1999/xhtml\""
   " version=\"XHTML+RDFa 1.1\""
   " prefix=\"rdfa: http://www.w3.org/ns/rdfa# ex: https://example.test/\">"
   "<head><title>Event</title></head>"
   "<body>"
   "<div about=\"https://example.test/event\">"
   "  <link property=\"rdfa:copy\" href=\"https://example.test/venue\"/>"
   "</div>"
   "<div about=\"https://example.test/venue\" typeof=\"rdfa:Pattern\">"
   "  <span property=\"ex:name\">Town Hall</span>"
   "  <span property=\"ex:capacity\" datatype=\"xsd:integer\">400</span>"
   "</div>"
   "</body></html>";

/**
 * The buffer status struct is used to keep track of where we are in
 * the current input buffer.
 */
typedef struct buffer_status
{
   const char* buffer;
   size_t current_offset;
   size_t total_length;
} buffer_status;

static void default_graph_triple(rdftriple* triple, void* callback_data)
{
   /* property copying delivers the finished default graph; no pattern or
    * copy triples remain by the time we get here. */
   rdfa_print_triple(triple);
   rdfa_free_triple(triple);
}

static void processor_graph_triple(rdftriple* triple, void* callback_data)
{
   printf("Processor Graph Triple:\n");
   rdfa_print_triple(triple);
   rdfa_free_triple(triple);
}

static size_t fill_buffer(
   char* buffer, size_t buffer_length, void* callback_data)
{
   buffer_status* status = (buffer_status*)callback_data;
   size_t remaining = status->total_length - status->current_offset;
   size_t take = remaining < buffer_length ? remaining : buffer_length;

   if(take > 0)
   {
      memcpy(buffer, status->buffer + status->current_offset, take);
      status->current_offset += take;
   }

   return take;
}

int main(int argc, char** argv)
{
#ifdef LIBRDFA_IN_RAPTOR
   raptor_init();
#endif

   {
      buffer_status status;
      char* file_contents = NULL;
      const char* base_uri = "https://example.test/page";
      rdfacontext* context;

      if(argc > 1)
      {
         FILE* input = fopen(argv[1], "rb");

         if(input == NULL)
         {
            perror("failed to open file");
            return 1;
         }

         fseek(input, 0, SEEK_END);
         {
            long size = ftell(input);
            fseek(input, 0, SEEK_SET);
            file_contents = (char*)malloc((size_t)size + 1);
            if(file_contents == NULL)
            {
               fclose(input);
               return 1;
            }
            fread(file_contents, 1, (size_t)size, input);
            file_contents[size] = '\0';
         }
         fclose(input);

         status.buffer = file_contents;
         status.total_length = strlen(file_contents);
      }
      else
      {
         status.buffer = SAMPLE_DOCUMENT;
         status.total_length = strlen(SAMPLE_DOCUMENT);
      }
      status.current_offset = 0;

      context = rdfa_create_context(base_uri);
      if(context == NULL)
      {
         free(file_contents);
         return 1;
      }

      context->callback_data = &status;
      rdfa_set_default_graph_triple_handler(
         context, &default_graph_triple);
      rdfa_set_processor_graph_triple_handler(
         context, &processor_graph_triple);
      rdfa_set_buffer_filler(context, &fill_buffer);

      /* Enable HTML+RDFa 1.1 property copying before parsing starts. */
      rdfa_set_property_copying(context, 1);

      rdfa_parse(context);
      rdfa_free_context(context);

      free(file_contents);
   }

#ifdef LIBRDFA_IN_RAPTOR
   raptor_finish();
#endif
   return 0;
}
