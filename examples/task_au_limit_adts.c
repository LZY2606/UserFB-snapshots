/*
 * Per-access-unit AAC-LC byte limit example for libxaac.
 *
 * Build (from the repository root, after configuring task_support):
 *   cmake -S task_support -B .task-build/cmake -DCMAKE_BUILD_TYPE=Release
 *   cmake --build .task-build/cmake --target libxaacenc -j
 *   cc -std=c99 -O2 -I. -Icommon -Iencoder -Iencoder/drc_src \
 *     examples/task_au_limit_adts.c .task-build/cmake/libxaacenc.a -lm \
 *     -o .task-build/task_au_limit_adts
 *
 * Run:
 *   .task-build/task_au_limit_adts output.adts
 *
 * The program writes a continuous ADTS file and prints the total packet size
 * (including the 7-byte ADTS header) and raw_data_block size for every AU.
 */
#define _POSIX_C_SOURCE 200112L

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ixheaac_type_def.h"
#include "ixheaac_error_standards.h"
#include "ixheaace_api.h"

#define SAMPLE_RATE 48000
#define CHANNELS 2
#define BITRATE 96000
#define FRAMES 48

static void *aligned_alloc_audio(UWORD32 size, UWORD32 alignment) {
  void *ptr = NULL;
  if (alignment < sizeof(void *)) alignment = sizeof(void *);
  return posix_memalign(&ptr, alignment, size ? size : 1) == 0 ? ptr : NULL;
}

static void aligned_free_audio(void *ptr) { free(ptr); }

static int16_t sample_at(long sample_index, int channel) {
  long frame = sample_index / 1024;
  long in_frame = sample_index % 1024;
  int section = (int)(frame % 12);
  double time = (double)sample_index / SAMPLE_RATE;
  uint32_t rng = (uint32_t)sample_index * 2654435761u +
                 (uint32_t)(channel + 1) * 2246822519u + 0x9e3779b9u;
  double noise = (double)((int)(rng >> 17) - 16384) * 0.45;
  double value = noise * 0.16;

  if (section < 2) {
    value = 0.0; /* silence */
  } else if (section < 6) {
    long impulse_pos = in_frame % 96;
    double envelope = exp(-(double)impulse_pos / 12.0);
    value += envelope * 11000.0 *
             sin(2.0 * 3.14159265358979323846 * (520.0 + 160.0 * (frame % 4)) *
                 time);
  } else if (section < 9) {
    value += 6500.0 *
             sin(2.0 * 3.14159265358979323846 * (330.0 + 110.0 * channel) * time);
  } else {
    value += noise * 1.4; /* independent L/R noise */
  }

  if (value > 32767.0) value = 32767.0;
  if (value < -32768.0) value = -32768.0;
  return (int16_t)lrint(value);
}

static WORD32 average_bytes(void) {
  return (WORD32)(((WORD64)BITRATE * 1024 + 8LL * SAMPLE_RATE - 1) /
                  (8LL * SAMPLE_RATE));
}

int main(int argc, char **argv) {
  const char *output_path = argc > 1 ? argv[1] : "task_au_limit.adts";
  FILE *output_file = fopen(output_path, "wb");
  ixheaace_input_config input;
  ixheaace_output_config output;
  WORD32 min_au_bytes = average_bytes() + 64;
  WORD32 max_au_bytes = 768 * CHANNELS;
  int frame;

  if (!output_file) {
    fprintf(stderr, "cannot open %s\n", output_path);
    return 1;
  }

  memset(&input, 0, sizeof(input));
  memset(&output, 0, sizeof(output));

  input.ui_pcm_wd_sz = 16;
  input.i_bitrate = BITRATE;
  input.aot = AOT_AAC_LC;
  input.i_channels = CHANNELS;
  input.i_samp_freq = SAMPLE_RATE;
  input.i_use_es = 1;
  input.i_use_adts = 1;
  input.frame_length = 1024;
  input.frame_cmd_flag = 1;
  input.max_au_bytes = 0;
  input.aac_config.bitrate = BITRATE;
  input.aac_config.inv_quant = 2;
  input.aac_config.bitreservoir_size = 768;
  input.aac_config.length = 64 * 1024;

  output.malloc_xheaace = aligned_alloc_audio;
  output.free_xheaace = aligned_free_audio;

  IA_ERRORCODE err = ixheaace_create(&input, &output);
  if (err != IA_NO_ERROR) {
    fprintf(stderr, "ixheaace_create failed: 0x%08x\n", (unsigned)err);
    fclose(output_file);
    return 2;
  }

  printf("writing %s\n", output_path);
  printf("valid max_au_bytes range at create: %d..%d\n", min_au_bytes,
         max_au_bytes);
  printf("frame cap_bytes total_bytes raw_au_bytes\n");

  for (frame = 0; frame < FRAMES; ++frame) {
    int16_t *pcm =
        (int16_t *)output.mem_info_table[IA_MEMTYPE_INPUT].mem_ptr;
    int sample_count = output.input_size / (int)sizeof(int16_t);
    int i;
    int cap_choice = (frame / 4) % 5;

    for (i = 0; i < sample_count; ++i) {
      int channel = i % CHANNELS;
      long sample_in_frame = i / CHANNELS;
      pcm[i] = sample_at((long)frame * 1024 + sample_in_frame, channel);
    }

    /* Repeatedly tighten to the lower endpoint and relax in steps. */
    if (cap_choice == 0) input.max_au_bytes = min_au_bytes;
    else if (cap_choice == 1) input.max_au_bytes = min_au_bytes + 32;
    else if (cap_choice == 2) input.max_au_bytes = (min_au_bytes + max_au_bytes) / 2;
    else if (cap_choice == 3) input.max_au_bytes = max_au_bytes;
    else input.max_au_bytes = 0; /* original bit-reservoir behavior */

    err = ixheaace_process(output.pv_ia_process_api_obj, &input, &output);
    if (err != IA_NO_ERROR) {
      fprintf(stderr, "process frame %d failed: 0x%08x\n", frame,
              (unsigned)err);
      ixheaace_delete(&output);
      fclose(output_file);
      return 3;
    }
    if (output.i_out_bytes <= 7) {
      fprintf(stderr, "frame %d produced no ADTS packet\n", frame);
      ixheaace_delete(&output);
      fclose(output_file);
      return 4;
    }

    if (fwrite(output.mem_info_table[IA_MEMTYPE_OUTPUT].mem_ptr, 1,
               (size_t)output.i_out_bytes, output_file) !=
        (size_t)output.i_out_bytes) {
      fprintf(stderr, "write failed\n");
      ixheaace_delete(&output);
      fclose(output_file);
      return 5;
    }

    printf("%5d %9d %11d %12d\n", frame, input.max_au_bytes,
           output.i_out_bytes, output.i_out_bytes - 7);
  }

  ixheaace_delete(&output);
  if (fclose(output_file) != 0) return 6;
  return 0;
}
