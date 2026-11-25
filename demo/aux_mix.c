#include "cmixer.h"

#include <stdio.h>

struct square {
  int position;
};

static void square_handler(cm_Event *e) {
  struct square *s = (struct square *) e->udata;
  int i;

  if (e->type == CM_EVENT_REWIND) {
    s->position = 0;
  } else if (e->type == CM_EVENT_SAMPLES) {
    for (i = 0; i < e->length; i += 2) {
      int phase = s->position++ % 96;
      e->buffer[i] = phase < 48 ? 9000 : -9000;
      e->buffer[i + 1] = phase < 48 ? 5000 : -5000;
    }
  }
}

int main(void) {
  enum { BLOCK_FRAMES = 256, TOTAL_FRAMES = 96000 };
  struct square state = {0};
  cm_Int16 buffer[BLOCK_FRAMES * 2];
  cm_SourceInfo info;
  cm_Source *src;
  cm_Bus *mix, *delay, *master;
  int block, peak = 0;

  cm_init(48000);

  info.handler = square_handler;
  info.udata = &state;
  info.samplerate = 48000;
  info.length = 24000; /* half a second, then natural end */
  src = cm_new_source(&info);

  mix = cm_new_bus();
  delay = cm_new_delay_bus(4800, 0.35); /* 4800 frames = 100 ms */
  master = cm_master_bus();
  if (!src || !mix || !delay) {
    fprintf(stderr, "auxiliary setup failed: %s\n", cm_get_error());
    return 1;
  }

  /* The source is decoded once into mix. mix splits into dry and wet paths. */
  cm_set_source_bus(src, mix);
  cm_connect_bus(mix, master, 0.8);
  cm_connect_bus(mix, delay, 0.5);
  cm_connect_bus(delay, master, 0.35);

  cm_play(src);
  for (block = 0; block < TOTAL_FRAMES / BLOCK_FRAMES; block++) {
    int i;
    cm_process(buffer, BLOCK_FRAMES * 2);
    for (i = 0; i < BLOCK_FRAMES * 2; i++) {
      int magnitude = buffer[i] < 0 ? -buffer[i] : buffer[i];
      if (magnitude > peak) {
        peak = magnitude;
      }
    }
  }

  printf("auxiliary dry/wet example rendered %d frames, peak=%d\n",
         TOTAL_FRAMES, peak);

  cm_destroy_source(src);
  cm_disconnect_bus(mix, master);
  cm_disconnect_bus(mix, delay);
  cm_disconnect_bus(delay, master);
  cm_destroy_bus(delay);
  cm_destroy_bus(mix);
  return 0;
}
