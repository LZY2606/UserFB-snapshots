/*
 * sparse_example.c - minimal use of the sparse stream interface.
 *
 * Builds a short island of stereo audio at source frame 100000, leaves
 * everything before and after it silent, and prints the events that come
 * out when the source stream is 48 kHz and the destination is 44.1 kHz.
 *
 * The printed timeline is exactly ceil(200000 * 44100 / 48000) output
 * frames long, as alternating silence and PCM events.  Nothing here
 * allocates memory proportional to the length of the timeline.
 */

#include "../include/sparse_resample.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

static void drain(lrs_sparse *st, float *buf, size_t cap, int channels)
{
   uint64_t start, count;
   int silence, e, i;

   for (;;) {
      e = lrs_sparse_pull(st, buf, cap, &start, &count, &silence);
      if (e == LRS_SPARSE_ERROR) {
         fprintf(stderr, "sparse_example: pull failed\n");
         return;
      }
      if (count == 0)
         break;                         /* nothing more right now */
      printf("event at %llu: %s %llu frames",
             (unsigned long long)start,
             silence ? "silence" : "pcm",
             (unsigned long long)count);
      if (!silence) {
         printf(":");
         for (i = 0; i < (int)(count * (uint64_t)channels) && i < 8; ++i)
            printf(" %.6f", buf[i]);
         if (count * (uint64_t)channels > 8)
            printf(" ...");
      }
      printf("\n");
   }
}

int main(void)
{
   lrs_sparse *st;
   float buf[4096];
   int i;

   st = lrs_sparse_open(48000, 44100, 2);
   if (!st) {
      fprintf(stderr, "sparse_example: open failed\n");
      return 1;
   }

   /* A 1000-frame stereo island at source frame 100000. */
   for (i = 0; i < 1000; ++i) {
      buf[i * 2 + 0] = (float)( 0.25 * sin(i * 0.05));
      buf[i * 2 + 1] = (float)(-0.25 * sin(i * 0.05));
   }
   if (lrs_sparse_push(st, 100000, 1000, buf) != LRS_SPARSE_OK) {
      fprintf(stderr, "sparse_example: push failed\n");
      lrs_sparse_close(st);
      return 1;
   }
   drain(st, buf, 4096, 2);

   /* End the source timeline at 200000 frames. */
   if (lrs_sparse_finish(st, 200000) != LRS_SPARSE_OK) {
      fprintf(stderr, "sparse_example: finish failed\n");
      lrs_sparse_close(st);
      return 1;
   }
   drain(st, buf, 4096, 2);

   printf("total output frames: %llu\n",
          (unsigned long long)lrs_sparse_output_length(st));

   lrs_sparse_close(st);
   return 0;
}
