/*
 * sample_edit.c - usage example for the editable ssdeep buffer.
 *
 * fuzzy_edit keeps the ssdeep summary of an in-memory byte buffer while
 * bytes are inserted, deleted and replaced, so a redaction preview can be
 * re-hashed locally instead of re-running the whole streaming hash after
 * every edit.  This example builds a buffer, edits it in place and prints
 * the signature before and after, together with the cumulative work
 * counters exposed by fuzzy_edit_stats().
 *
 * Compile against an installed libfuzzy:
 *
 *   cc -Wall -I/usr/local/include sample_edit.c -lfuzzy -o sample_edit
 *
 * or straight from the source tree:
 *
 *   cc -Wall -I. sample_edit.c fuzzy_edit.c fuzzy.c edit_dist.c -o sample_edit
 *
 * The data here is generated benign pseudo-random bytes; the API treats
 * every byte, including NUL and high bytes, as ordinary data.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuzzy.h"
#include "fuzzy_edit.h"

#define INITIAL_SIZE 200000u

static unsigned int rng_state = 0x12345678u;

static unsigned char next_byte(void)
{
  rng_state = rng_state * 1664525u + 1013904223u;
  return (unsigned char)(rng_state >> 24);
}

int main(void)
{
  unsigned char *data = malloc(INITIAL_SIZE);
  char before[FUZZY_MAX_RESULT], after[FUZZY_MAX_RESULT];
  struct fuzzy_edit *edit;
  uint64_t raw_bytes, node_visits;
  unsigned char patch[256];
  size_t i;
  int status;

  if (data == NULL)
  {
    fprintf(stderr, "out of memory\n");
    return EXIT_FAILURE;
  }
  for (i = 0; i < INITIAL_SIZE; ++i)
    data[i] = next_byte();

  /* The constructor copies the input; data could be freed immediately. */
  edit = fuzzy_edit_new(data, INITIAL_SIZE);
  if (edit == NULL)
  {
    fprintf(stderr, "fuzzy_edit_new failed\n");
    free(data);
    return EXIT_FAILURE;
  }

  status = fuzzy_edit_digest(edit, before, 0);
  if (status != 0)
  {
    fprintf(stderr, "fuzzy_edit_digest failed\n");
    fuzzy_edit_free(edit);
    free(data);
    return EXIT_FAILURE;
  }
  printf("before: %s\n", before);

  /* Replace a 1024 byte redaction window in the middle. */
  for (i = 0; i < sizeof(patch); ++i)
    patch[i] = next_byte();
  if (fuzzy_edit_splice(edit, INITIAL_SIZE / 2u, 1024u,
			patch, sizeof(patch)) != 0)
  {
    fprintf(stderr, "replace splice failed\n");
    fuzzy_edit_free(edit);
    free(data);
    return EXIT_FAILURE;
  }

  /* Insert a short marker near the front (offset, remove nothing). */
  if (fuzzy_edit_splice(edit, 17u, 0u,
			(const unsigned char *)"EDIT", 4u) != 0)
  {
    fprintf(stderr, "insert splice failed\n");
    fuzzy_edit_free(edit);
    free(data);
    return EXIT_FAILURE;
  }

  if (fuzzy_edit_digest(edit, after, FUZZY_FLAG_NOTRUNC) != 0)
  {
    fprintf(stderr, "second fuzzy_edit_digest failed\n");
    fuzzy_edit_free(edit);
    free(data);
    return EXIT_FAILURE;
  }
  printf("after : %s\n", after);
  printf("match score: %d\n", fuzzy_compare(before, after));

  fuzzy_edit_stats(edit, &raw_bytes, &node_visits);
  printf("cumulative raw_bytes=%llu node_visits=%llu buffer_size=%zu\n",
	 (unsigned long long)raw_bytes,
	 (unsigned long long)node_visits,
	 fuzzy_edit_size(edit));

  /* fuzzy_edit_free accepts NULL, mirroring free(). */
  fuzzy_edit_free(edit);
  free(data);
  return EXIT_SUCCESS;
}
