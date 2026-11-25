/*
** Copyright (c) 2017 rxi
**
** Permission is hereby granted, free of charge, to any person obtaining a copy
** of this software and associated documentation files (the "Software"), to
** deal in the Software without restriction, including without limitation the
** rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
** sell copies of the Software, and to permit persons to whom the Software is
** furnished to do so, subject to the following conditions:
**
** The above copyright notice and this permission notice shall be included in
** all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
** AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
** LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
** FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
** IN THE SOFTWARE.
**/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <math.h>

#include "cmixer.h"

#define UNUSED(x)         ((void) (x))
#define CLAMP(x, a, b)    ((x) < (a) ? (a) : (x) > (b) ? (b) : (x))
#define MIN(a, b)         ((a) < (b) ? (a) : (b))
#define MAX(a, b)         ((a) > (b) ? (a) : (b))

#define FX_BITS           (12)
#define FX_UNIT           (1 << FX_BITS)
#define FX_MASK           (FX_UNIT - 1)
#define FX_FROM_FLOAT(f)  ((f) * FX_UNIT)
#define FX_LERP(a, b, p)  ((a) + ((((b) - (a)) * (p)) >> FX_BITS))

#define BUFFER_SIZE       (512)
#define BUFFER_MASK       (BUFFER_SIZE - 1)


struct cm_Source {
  cm_Source *next;              /* Next source in list */
  cm_Int16 buffer[BUFFER_SIZE]; /* Internal buffer with raw stereo PCM */
  cm_EventHandler handler;      /* Event handler */
  void *udata;          /* Stream's udata (from cm_SourceInfo) */
  int samplerate;       /* Stream's native samplerate */
  int length;           /* Stream's length in frames */
  int end;              /* End index for the current play-through */
  int state;            /* Current state (playing|paused|stopped) */
  cm_Int64 position;    /* Current playhead position (fixed point) */
  int lgain, rgain;     /* Left and right gain (fixed point) */
  int rate;             /* Playback rate (fixed point) */
  int nextfill;         /* Next frame idx where the buffer needs to be filled */
  int loop;             /* Whether the source will loop when `end` is reached */
  int rewind;           /* Whether the source will rewind before playing */
  int active;           /* Whether the source is part of `sources` list */
  cm_Bus *target;       /* Bus that receives the final stereo contribution */
  double gain;          /* Gain set by `cm_set_gain()` */
  double pan;           /* Pan set by `cm_set_pan()` */
};


typedef struct cm_Edge {
  cm_Bus *to;
  double gain;
} cm_Edge;


struct cm_Bus {
  int type;
  int is_master;
  double gain;
  double feedback;
  int delay;
  int cursor;
  int topo_indegree;
  unsigned mark;
  int incoming;
  int source_refs;
  cm_Edge *edges;
  int edge_count;
  int edge_capacity;
  double *delay_x;
  double *delay_y;
  double input[BUFFER_SIZE];
  double output[BUFFER_SIZE];
};


static struct {
  const char *lasterror;        /* Last error message */
  cm_EventHandler lock;         /* Event handler for lock/unlock events */
  cm_Source *sources;           /* Linked list of active (playing) sources */
  cm_Int32 buffer[BUFFER_SIZE]; /* Internal master buffer */
  int samplerate;               /* Master samplerate */
  int gain;                     /* Master gain (fixed point) */
  cm_Bus master;                /* Permanent final sink */
  cm_Bus **buses;               /* User-created buses */
  int bus_count;
  int bus_capacity;
  cm_Bus **order;               /* Cached topological render order */
  int order_count;
  unsigned visit_serial;
} cmixer;


static void dummy_handler(cm_Event *e) {
  UNUSED(e);
}


static void lock(void) {
  cm_Event e;
  e.type = CM_EVENT_LOCK;
  cmixer.lock(&e);
}


static void unlock(void) {
  cm_Event e;
  e.type = CM_EVENT_UNLOCK;
  cmixer.lock(&e);
}


const char* cm_get_error(void) {
  const char *res = cmixer.lasterror;
  cmixer.lasterror = NULL;
  return res;
}


static const char* error(const char *msg) {
  cmixer.lasterror = msg;
  return msg;
}


static int fixed_from_double(double value) {
  double scaled = value * FX_UNIT;
  if (scaled >= (double) INT_MAX) {
    return INT_MAX;
  }
  if (scaled <= (double) INT_MIN) {
    return INT_MIN;
  }
  return (int) scaled;
}


void cm_init(int samplerate) {
  cmixer.samplerate = samplerate;
  cmixer.lock = dummy_handler;
  cmixer.sources = NULL;
  cmixer.gain = FX_UNIT;
  cmixer.bus_count = 0;
  cmixer.visit_serial = 0;
  memset(&cmixer.master, 0, sizeof(cmixer.master));
  cmixer.master.type = CM_BUS_NORMAL;
  cmixer.master.is_master = 1;
  cmixer.master.gain = 1.0;
}


void cm_set_lock(cm_EventHandler lock) {
  cmixer.lock = lock;
}


void cm_set_master_gain(double gain) {
  if (!isfinite(gain)) {
    error("gain must be finite");
    return;
  }
  cmixer.gain = fixed_from_double(gain);
  cmixer.master.gain = gain;
}


static void rewind_source(cm_Source *src) {
  cm_Event e;
  e.type = CM_EVENT_REWIND;
  e.udata = src->udata;
  src->handler(&e);
  src->position = 0;
  src->rewind = 0;
  src->end = src->length;
  src->nextfill = 0;
}


static void fill_source_buffer(cm_Source *src, int offset, int length) {
  cm_Event e;
  e.type = CM_EVENT_SAMPLES;
  e.udata = src->udata;
  e.buffer = src->buffer + offset;
  e.length = length;
  src->handler(&e);
}


static void process_source(cm_Source *src, int len, cm_Int32 *dst) {
  int i, n, a, b, p;
  int frame, count;

  /* Do rewind if flag is set */
  if (src->rewind) {
    rewind_source(src);
  }

  /* Don't process if not playing */
  if (src->state != CM_STATE_PLAYING) {
    return;
  }

  /* Process audio */
  while (len > 0) {
    /* Get current position frame */
    frame = src->position >> FX_BITS;

    /* Fill buffer if required */
    if (frame + 3 >= src->nextfill) {
      fill_source_buffer(src, (src->nextfill*2) & BUFFER_MASK, BUFFER_SIZE/2);
      src->nextfill += BUFFER_SIZE / 4;
    }

    /* Handle reaching the end of the playthrough */
    if (frame >= src->end) {
      /* As streams continiously fill the raw buffer in a loop we simply
      ** increment the end idx by one length and continue reading from it for
      ** another play-through */
      src->end = frame + src->length;
      /* Set state and stop processing if we're not set to loop */
      if (!src->loop) {
        src->state = CM_STATE_STOPPED;
        break;
      }
    }

    /* Work out how many frames we should process in the loop */
    n = MIN(src->nextfill - 2, src->end) - frame;
    count = (n << FX_BITS) / src->rate;
    count = MAX(count, 1);
    count = MIN(count, len / 2);
    len -= count * 2;

    /* Add audio to master buffer */
    if (src->rate == FX_UNIT) {
      /* Add audio to buffer -- basic */
      n = frame * 2;
      for (i = 0; i < count; i++) {
        dst[0] += (src->buffer[(n    ) & BUFFER_MASK] * src->lgain) >> FX_BITS;
        dst[1] += (src->buffer[(n + 1) & BUFFER_MASK] * src->rgain) >> FX_BITS;
        n += 2;
        dst += 2;
      }
      src->position += count * FX_UNIT;

    } else {
      /* Add audio to buffer -- interpolated */
      for (i = 0; i < count; i++) {
        n = (src->position >> FX_BITS) * 2;
        p = src->position & FX_MASK;
        a = src->buffer[(n    ) & BUFFER_MASK];
        b = src->buffer[(n + 2) & BUFFER_MASK];
        dst[0] += (FX_LERP(a, b, p) * src->lgain) >> FX_BITS;
        n++;
        a = src->buffer[(n    ) & BUFFER_MASK];
        b = src->buffer[(n + 2) & BUFFER_MASK];
        dst[1] += (FX_LERP(a, b, p) * src->rgain) >> FX_BITS;
        src->position += src->rate;
        dst += 2;
      }
    }

  }
}


static void cm_process_legacy(cm_Int16 *dst, int len) {
  int i;
  cm_Source **s;

  /* Zeroset internal buffer */
  memset(cmixer.buffer, 0, len * sizeof(cmixer.buffer[0]));

  /* Process active sources */
  lock();
  s = &cmixer.sources;
  while (*s) {
    process_source(*s, len, cmixer.buffer);
    /* Remove source from list if it is no longer playing */
    if ((*s)->state != CM_STATE_PLAYING) {
      (*s)->active = 0;
      *s = (*s)->next;
    } else {
      s = &(*s)->next;
    }
  }
  unlock();

  /* Copy internal buffer to destination and clip */
  for (i = 0; i < len; i++) {
    int x = (cmixer.buffer[i] * cmixer.gain) >> FX_BITS;
    dst[i] = CLAMP(x, -32768, 32767);
  }
}


static void rebuild_bus_order(void) {
  int i, head, tail;

  cmixer.order_count = 0;
  tail = 0;

  for (i = 0; i < cmixer.bus_count; i++) {
    cm_Bus *bus = cmixer.buses[i];
    bus->topo_indegree = bus->incoming;
    if (bus->topo_indegree == 0) {
      cmixer.order[tail++] = bus;
    }
  }

  head = 0;
  while (head < tail) {
    int j;
    cm_Bus *bus = cmixer.order[head++];
    for (j = 0; j < bus->edge_count; j++) {
      cm_Bus *to = bus->edges[j].to;
      if (to->is_master) {
        continue;
      }
      to->topo_indegree--;
      if (to->topo_indegree == 0) {
        cmixer.order[tail++] = to;
      }
    }
  }

  cmixer.order_count = tail;
}


static cm_Edge* find_bus_edge(cm_Bus *from, cm_Bus *to, int *index) {
  int i;
  for (i = 0; i < from->edge_count; i++) {
    if (from->edges[i].to == to) {
      if (index) {
        *index = i;
      }
      return &from->edges[i];
    }
  }
  return NULL;
}


static void add_source_contributions(int len) {
  cm_Source **s;
  /* One source block is small enough for the fixed mixer chunk. */
  cm_Int32 source_buffer[BUFFER_SIZE];

  lock();
  s = &cmixer.sources;
  while (*s) {
    int i;
    cm_Bus *target = (*s)->target ? (*s)->target : &cmixer.master;

    memset(source_buffer, 0, len * sizeof(source_buffer[0]));
    process_source(*s, len, source_buffer);

    for (i = 0; i < len; i++) {
      target->input[i] += (double) source_buffer[i];
    }

    /* Remove source from list if it is no longer playing */
    if ((*s)->state != CM_STATE_PLAYING) {
      (*s)->active = 0;
      *s = (*s)->next;
    } else {
      s = &(*s)->next;
    }
  }
  unlock();
}


static void process_bus(cm_Bus *bus, int len) {
  int i;

  if (bus->type == CM_BUS_DELAY) {
    int frame, frames = len / 2;
    for (frame = 0; frame < frames; frame++) {
      int channel;
      int slot = bus->cursor * 2;
      for (channel = 0; channel < 2; channel++) {
        int idx = slot + channel;
        double x = bus->input[frame * 2 + channel];
        double delayed_x = bus->delay_x[idx];
        double delayed_y = bus->delay_y[idx];
        double y = delayed_x + bus->feedback * delayed_y;
        bus->delay_x[idx] = x;
        bus->delay_y[idx] = y;
        bus->output[frame * 2 + channel] = y * bus->gain;
      }
      bus->cursor++;
      if (bus->cursor == bus->delay) {
        bus->cursor = 0;
      }
    }
    /* Keep the final sample defined if a caller ever passes an odd length. */
    if (len & 1) {
      bus->output[len - 1] = bus->input[len - 1] * bus->gain;
    }
  } else {
    for (i = 0; i < len; i++) {
      bus->output[i] = bus->input[i] * bus->gain;
    }
  }

  for (i = 0; i < bus->edge_count; i++) {
    int sample;
    cm_Edge *edge = &bus->edges[i];
    cm_Bus *to = edge->to;
    for (sample = 0; sample < len; sample++) {
      to->input[sample] += bus->output[sample] * edge->gain;
    }
  }
}


static void cm_process_graph(cm_Int16 *dst, int len) {
  int i;

  memset(cmixer.master.input, 0, len * sizeof(double));
  for (i = 0; i < cmixer.bus_count; i++) {
    memset(cmixer.buses[i]->input, 0, len * sizeof(double));
  }

  add_source_contributions(len);

  for (i = 0; i < cmixer.order_count; i++) {
    process_bus(cmixer.order[i], len);
  }

  for (i = 0; i < len; i++) {
    double value = floor(cmixer.master.input[i] *
                         (double) cmixer.gain / FX_UNIT);
    if (!isfinite(value)) {
      dst[i] = value > 0.0 ? 32767 : (value < 0.0 ? -32768 : 0);
    } else if (value >= 32767.0) {
      dst[i] = 32767;
    } else if (value <= -32768.0) {
      dst[i] = -32768;
    } else {
      dst[i] = (cm_Int16) value;
    }
  }
}


void cm_process(cm_Int16 *dst, int len) {
  /* Process in chunks of BUFFER_SIZE if `len` is larger than BUFFER_SIZE */
  while (len > BUFFER_SIZE) {
    cm_process(dst, BUFFER_SIZE);
    dst += BUFFER_SIZE;
    len -= BUFFER_SIZE;
  }

  if (cmixer.bus_count == 0) {
    cm_process_legacy(dst, len);
  } else {
    cm_process_graph(dst, len);
  }
}


cm_Source* cm_new_source(const cm_SourceInfo *info) {
  cm_Source *src = calloc(1, sizeof(*src));
  if (!src) {
    error("allocation failed");
    return NULL;
  }
  src->handler = info->handler;
  src->length = info->length;
  src->samplerate = info->samplerate;
  src->udata = info->udata;
  src->target = &cmixer.master;
  cmixer.master.source_refs++;
  cm_set_gain(src, 1);
  cm_set_pan(src, 0);
  cm_set_pitch(src, 1);
  cm_set_loop(src, 0);
  cm_stop(src);
  return src;
}


static const char* wav_init(cm_SourceInfo *info, void *data, int len, int ownsdata);

#ifdef CM_USE_STB_VORBIS
static const char* ogg_init(cm_SourceInfo *info, void *data, int len, int ownsdata);
#endif


static int check_header(void *data, int size, char *str, int offset) {
  int len = strlen(str);
  return (size >= offset + len) && !memcmp((char*) data + offset, str, len);
}


static cm_Source* new_source_from_mem(void *data, int size, int ownsdata) {
  const char *err;
  cm_SourceInfo info;

  if (check_header(data, size, "WAVE", 8)) {
    err = wav_init(&info, data, size, ownsdata);
    if (err) {
      return NULL;
    }
    return cm_new_source(&info);
  }

#ifdef CM_USE_STB_VORBIS
  if (check_header(data, size, "OggS", 0)) {
    err = ogg_init(&info, data, size, ownsdata);
    if (err) {
      return NULL;
    }
    return cm_new_source(&info);
  }
#endif

  error("unknown format or invalid data");
  return NULL;
}


static void* load_file(const char *filename, int *size) {
  FILE *fp;
  void *data;
  int n;

  fp = fopen(filename, "rb");
  if (!fp) {
    return NULL;
  }

  /* Get size */
  fseek(fp, 0, SEEK_END);
  *size = ftell(fp);
  rewind(fp);

  /* Malloc, read and return data */
  data = malloc(*size);
  if (!data) {
    fclose(fp);
    return NULL;
  }
  n = fread(data, 1, *size, fp);
  fclose(fp);
  if (n != *size) {
    free(data);
    return NULL;
  }

  return data;
}


cm_Source* cm_new_source_from_file(const char *filename) {
  int size;
  cm_Source *src;
  void *data;

  /* Load file into memory */
  data = load_file(filename, &size);
  if (!data) {
    error("could not load file");
    return NULL;
  }

  /* Try to load and return */
  src = new_source_from_mem(data, size, 1);
  if (!src) {
    free(data);
    return NULL;
  }

  return src;
}


cm_Source* cm_new_source_from_mem(void *data, int size) {
  return new_source_from_mem(data, size, 0);
}


void cm_destroy_source(cm_Source *src) {
  cm_Event e;
  lock();
  if (src->active) {
    cm_Source **s = &cmixer.sources;
    while (*s) {
      if (*s == src) {
        *s = src->next;
        break;
      }
    }
  }
  unlock();
  if (src->target) {
    src->target->source_refs--;
  }
  e.type = CM_EVENT_DESTROY;
  e.udata = src->udata;
  src->handler(&e);
  free(src);
}


double cm_get_length(cm_Source *src) {
  return src->length / (double) src->samplerate;
}


double cm_get_position(cm_Source *src) {
  return ((src->position >> FX_BITS) % src->length) / (double) src->samplerate;
}


int cm_get_state(cm_Source *src) {
  return src->state;
}


static void recalc_source_gains(cm_Source *src) {
  double l, r;
  double pan = src->pan;
  l = src->gain * (pan <= 0. ? 1. : 1. - pan);
  r = src->gain * (pan >= 0. ? 1. : 1. + pan);
  src->lgain = fixed_from_double(l);
  src->rgain = fixed_from_double(r);
}


void cm_set_gain(cm_Source *src, double gain) {
  if (!src || !isfinite(gain)) {
    error("gain must be finite");
    return;
  }
  src->gain = gain;
  recalc_source_gains(src);
}


void cm_set_pan(cm_Source *src, double pan) {
  if (!src || !isfinite(pan)) {
    error("pan must be finite");
    return;
  }
  src->pan = CLAMP(pan, -1.0, 1.0);
  recalc_source_gains(src);
}


void cm_set_pitch(cm_Source *src, double pitch) {
  double rate;
  if (pitch > 0.) {
    rate = src->samplerate / (double) cmixer.samplerate * pitch;
  } else {
    rate = 0.001;
  }
  src->rate = fixed_from_double(rate);
}


void cm_set_loop(cm_Source *src, int loop) {
  src->loop = loop;
}


void cm_play(cm_Source *src) {
  lock();
  src->state = CM_STATE_PLAYING;
  if (!src->active) {
    src->active = 1;
    src->next = cmixer.sources;
    cmixer.sources = src;
  }
  unlock();
}


void cm_pause(cm_Source *src) {
  src->state = CM_STATE_PAUSED;
}


void cm_stop(cm_Source *src) {
  src->state = CM_STATE_STOPPED;
  src->rewind = 1;
}


/*============================================================================
** Auxiliary buses and routing graph
**============================================================================*/

cm_Bus* cm_master_bus(void) {
  return &cmixer.master;
}


static int ensure_bus_capacity(void) {
  int new_capacity;
  cm_Bus **new_buses;
  cm_Bus **new_order;

  if (cmixer.bus_count < cmixer.bus_capacity) {
    return 1;
  }
  if (cmixer.bus_capacity > 0 && cmixer.bus_capacity > INT_MAX / 2) {
    error("too many buses");
    return 0;
  }

  new_capacity = cmixer.bus_capacity ? cmixer.bus_capacity * 2 : 8;

  /* Grow the render-order array first; failure leaves the old allocation
  ** and capacity valid. */
  new_order = realloc(cmixer.order,
                      (size_t) new_capacity * sizeof(*new_order));
  if (!new_order) {
    error("allocation failed");
    return 0;
  }

  new_buses = realloc(cmixer.buses,
                      (size_t) new_capacity * sizeof(*new_buses));
  if (!new_buses) {
    cmixer.order = new_order;
    error("allocation failed");
    return 0;
  }

  cmixer.order = new_order;
  cmixer.buses = new_buses;
  cmixer.bus_capacity = new_capacity;
  return 1;
}


static cm_Bus* allocate_bus(int type) {
  cm_Bus *bus;

  if (!ensure_bus_capacity()) {
    return NULL;
  }

  bus = calloc(1, sizeof(*bus));
  if (!bus) {
    error("allocation failed");
    return NULL;
  }

  bus->type = type;
  bus->gain = 1.0;
  cmixer.buses[cmixer.bus_count++] = bus;
  rebuild_bus_order();
  return bus;
}


cm_Bus* cm_new_bus(void) {
  return allocate_bus(CM_BUS_NORMAL);
}


cm_Bus* cm_new_delay_bus(int delay_frames, double feedback) {
  cm_Bus *bus;
  size_t state_count;

  if (delay_frames <= 0) {
    error("delay must be a positive number of frames");
    return NULL;
  }
  if (!isfinite(feedback) || feedback < 0.0 || feedback >= 1.0) {
    error("feedback must be finite and in [0, 1)");
    return NULL;
  }

  bus = calloc(1, sizeof(*bus));
  if (!bus) {
    error("allocation failed");
    return NULL;
  }
  bus->type = CM_BUS_DELAY;
  bus->gain = 1.0;
  bus->feedback = feedback;
  bus->delay = delay_frames;

  state_count = (size_t) delay_frames * 2;
  bus->delay_x = calloc(state_count, sizeof(*bus->delay_x));
  bus->delay_y = calloc(state_count, sizeof(*bus->delay_y));
  if (!bus->delay_x || !bus->delay_y) {
    free(bus->delay_x);
    free(bus->delay_y);
    free(bus);
    error("allocation failed");
    return NULL;
  }

  if (!ensure_bus_capacity()) {
    free(bus->delay_x);
    free(bus->delay_y);
    free(bus);
    return NULL;
  }

  cmixer.buses[cmixer.bus_count++] = bus;
  rebuild_bus_order();
  return bus;
}


int cm_destroy_bus(cm_Bus *bus) {
  int i;

  if (!bus || bus->is_master) {
    error("invalid bus");
    return CM_ERROR;
  }
  if (bus->source_refs != 0 || bus->edge_count != 0 || bus->incoming != 0) {
    error("bus still has source references or edges");
    return CM_ERROR;
  }

  for (i = 0; i < cmixer.bus_count; i++) {
    if (cmixer.buses[i] == bus) {
      cmixer.buses[i] = cmixer.buses[--cmixer.bus_count];
      rebuild_bus_order();
      free(bus->delay_x);
      free(bus->delay_y);
      free(bus->edges);
      free(bus);
      return CM_OK;
    }
  }

  error("unknown bus");
  return CM_ERROR;
}


int cm_set_bus_gain(cm_Bus *bus, double gain) {
  if (!bus || !isfinite(gain)) {
    error("invalid bus gain");
    return CM_ERROR;
  }
  bus->gain = gain;
  if (bus->is_master) {
    cmixer.gain = fixed_from_double(gain);
  }
  return CM_OK;
}


int cm_get_bus_gain(cm_Bus *bus, double *gain) {
  if (!bus || !gain) {
    error("invalid bus gain output");
    return CM_ERROR;
  }
  *gain = bus->gain;
  return CM_OK;
}


int cm_set_delay_feedback(cm_Bus *bus, double feedback) {
  if (!bus || bus->type != CM_BUS_DELAY) {
    error("invalid delay bus");
    return CM_ERROR;
  }
  if (!isfinite(feedback) || feedback < 0.0 || feedback >= 1.0) {
    error("feedback must be finite and in [0, 1)");
    return CM_ERROR;
  }
  bus->feedback = feedback;
  return CM_OK;
}


int cm_clear_delay_history(cm_Bus *bus) {
  if (!bus || bus->type != CM_BUS_DELAY) {
    error("invalid delay bus");
    return CM_ERROR;
  }
  memset(bus->delay_x, 0, (size_t) bus->delay * 2 * sizeof(*bus->delay_x));
  memset(bus->delay_y, 0, (size_t) bus->delay * 2 * sizeof(*bus->delay_y));
  bus->cursor = 0;
  return CM_OK;
}


int cm_set_source_bus(cm_Source *src, cm_Bus *bus) {
  if (!src) {
    error("invalid source");
    return CM_ERROR;
  }
  if (!bus) {
    bus = &cmixer.master;
  }
  if (src->target == bus) {
    return CM_OK;
  }
  src->target->source_refs--;
  src->target = bus;
  bus->source_refs++;
  return CM_OK;
}


int cm_use_master_bus(cm_Source *src) {
  return cm_set_source_bus(src, &cmixer.master);
}


cm_Bus* cm_get_source_bus(cm_Source *src) {
  return src ? src->target : NULL;
}


/* Returns 1 when goal is reachable from start, 0 when not, -1 on failure. */
static int bus_reaches(cm_Bus *start, cm_Bus *goal) {
  cm_Bus **stack;
  int capacity = cmixer.bus_count + 1;
  int sp = 0;
  unsigned serial;

  stack = malloc((size_t) capacity * sizeof(*stack));
  if (!stack) {
    error("allocation failed");
    return -1;
  }

  cmixer.visit_serial++;
  if (cmixer.visit_serial == 0) {
    int i;
    cmixer.master.mark = 0;
    for (i = 0; i < cmixer.bus_count; i++) {
      cmixer.buses[i]->mark = 0;
    }
    cmixer.visit_serial = 1;
  }
  serial = cmixer.visit_serial;

  stack[sp++] = start;
  start->mark = serial;

  while (sp > 0) {
    int i;
    cm_Bus *bus = stack[--sp];
    if (bus == goal) {
      free(stack);
      return 1;
    }
    for (i = 0; i < bus->edge_count; i++) {
      cm_Bus *next = bus->edges[i].to;
      if (next->mark != serial) {
        next->mark = serial;
        stack[sp++] = next;
      }
    }
  }

  free(stack);
  return 0;
}


int cm_connect_bus(cm_Bus *from, cm_Bus *to, double gain) {
  cm_Edge *edge;
  int reaches;

  if (!from || !to || from->is_master) {
    error("invalid bus connection");
    return CM_ERROR;
  }
  if (!isfinite(gain)) {
    error("edge gain must be finite");
    return CM_ERROR;
  }

  edge = find_bus_edge(from, to, NULL);
  if (edge) {
    edge->gain = gain;
    return CM_OK;
  }

  if (from == to) {
    error("bus connection would create a cycle");
    return CM_ERROR;
  }
  reaches = bus_reaches(to, from);
  if (reaches < 0) {
    return CM_ERROR;
  }
  if (reaches) {
    error("bus connection would create a cycle");
    return CM_ERROR;
  }

  if (from->edge_count == from->edge_capacity) {
    int new_capacity = from->edge_capacity ? from->edge_capacity * 2 : 4;
    cm_Edge *new_edges = realloc(from->edges,
                                 (size_t) new_capacity * sizeof(*new_edges));
    if (!new_edges) {
      error("allocation failed");
      return CM_ERROR;
    }
    from->edges = new_edges;
    from->edge_capacity = new_capacity;
  }

  from->edges[from->edge_count].to = to;
  from->edges[from->edge_count].gain = gain;
  from->edge_count++;
  to->incoming++;
  rebuild_bus_order();
  return CM_OK;
}


int cm_set_edge_gain(cm_Bus *from, cm_Bus *to, double gain) {
  cm_Edge *edge;

  if (!from || !to || !isfinite(gain)) {
    error("invalid edge gain");
    return CM_ERROR;
  }
  edge = find_bus_edge(from, to, NULL);
  if (!edge) {
    error("bus edge does not exist");
    return CM_ERROR;
  }
  edge->gain = gain;
  return CM_OK;
}


int cm_get_edge_gain(cm_Bus *from, cm_Bus *to, double *gain) {
  cm_Edge *edge;

  if (!from || !to || !gain) {
    error("invalid bus edge");
    return CM_ERROR;
  }
  edge = find_bus_edge(from, to, NULL);
  if (!edge) {
    error("bus edge does not exist");
    return CM_ERROR;
  }
  *gain = edge->gain;
  return CM_OK;
}


int cm_disconnect_bus(cm_Bus *from, cm_Bus *to) {
  int index;
  cm_Edge *edge;

  if (!from || !to) {
    error("invalid bus disconnection");
    return CM_ERROR;
  }
  edge = find_bus_edge(from, to, &index);
  if (!edge) {
    error("bus edge does not exist");
    return CM_ERROR;
  }

  *edge = from->edges[--from->edge_count];
  to->incoming--;
  rebuild_bus_order();
  return CM_OK;
}


/*============================================================================
** Wav stream
**============================================================================*/

typedef struct {
  void *data;
  int bitdepth;
  int samplerate;
  int channels;
  int length;
} Wav;

typedef struct {
  Wav wav;
  void *data;
  int idx;
} WavStream;


static char* find_subchunk(char *data, int len, char *id, int *size) {
  /* TODO : Error handling on malformed wav file */
  int idlen = strlen(id);
  char *p = data + 12;
next:
  *size = *((cm_UInt32*) (p + 4));
  if (memcmp(p, id, idlen)) {
    p += 8 + *size;
    if (p > data + len) return NULL;
    goto next;
  }
  return p + 8;
}


static const char* read_wav(Wav *w, void *data, int len) {
  int bitdepth, channels, samplerate, format;
  int sz;
  char *p = data;
  memset(w, 0, sizeof(*w));

  /* Check header */
  if (memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) {
    return error("bad wav header");
  }
  /* Find fmt subchunk */
  p = find_subchunk(data, len, "fmt", &sz);
  if (!p) {
    return error("no fmt subchunk");
  }

  /* Load fmt info */
  format      = *((cm_UInt16*) (p));
  channels    = *((cm_UInt16*) (p + 2));
  samplerate  = *((cm_UInt32*) (p + 4));
  bitdepth    = *((cm_UInt16*) (p + 14));
  if (format != 1) {
    return error("unsupported format");
  }
  if (channels == 0 || samplerate == 0 || bitdepth == 0) {
    return error("bad format");
  }

  /* Find data subchunk */
  p = find_subchunk(data, len, "data", &sz);
  if (!p) {
    return error("no data subchunk");
  }

  /* Init struct */
  w->data = (void*) p;
  w->samplerate = samplerate;
  w->channels = channels;
  w->length = (sz / (bitdepth / 8)) / channels;
  w->bitdepth = bitdepth;
  /* Done */
  return NULL;
}


#define WAV_PROCESS_LOOP(X) \
  while (n--) {             \
    X                       \
    dst += 2;               \
    s->idx++;               \
  }

static void wav_handler(cm_Event *e) {
  int x, n;
  cm_Int16 *dst;
  WavStream *s = e->udata;
  int len;

  switch (e->type) {

    case CM_EVENT_DESTROY:
      free(s->data);
      free(s);
      break;

    case CM_EVENT_SAMPLES:
      dst = e->buffer;
      len = e->length / 2;
fill:
      n = MIN(len, s->wav.length - s->idx);
      len -= n;
      if (s->wav.bitdepth == 16 && s->wav.channels == 1) {
        WAV_PROCESS_LOOP({
          dst[0] = dst[1] = ((cm_Int16*) s->wav.data)[s->idx];
        });
      } else if (s->wav.bitdepth == 16 && s->wav.channels == 2) {
        WAV_PROCESS_LOOP({
          x = s->idx * 2;
          dst[0] = ((cm_Int16*) s->wav.data)[x    ];
          dst[1] = ((cm_Int16*) s->wav.data)[x + 1];
        });
      } else if (s->wav.bitdepth == 8 && s->wav.channels == 1) {
        WAV_PROCESS_LOOP({
          dst[0] = dst[1] = (((cm_UInt8*) s->wav.data)[s->idx] - 128) << 8;
        });
      } else if (s->wav.bitdepth == 8 && s->wav.channels == 2) {
        WAV_PROCESS_LOOP({
          x = s->idx * 2;
          dst[0] = (((cm_UInt8*) s->wav.data)[x    ] - 128) << 8;
          dst[1] = (((cm_UInt8*) s->wav.data)[x + 1] - 128) << 8;
        });
      }
      /* Loop back and continue filling buffer if we didn't fill the buffer */
      if (len > 0) {
        s->idx = 0;
        goto fill;
      }
      break;

    case CM_EVENT_REWIND:
      s->idx = 0;
      break;
  }
}


static const char* wav_init(cm_SourceInfo *info, void *data, int len, int ownsdata) {
  WavStream *stream;
  Wav wav;

  const char *err = read_wav(&wav, data, len);
  if (err != NULL) {
    return err;
  }

  if (wav.channels > 2 || (wav.bitdepth != 16 && wav.bitdepth != 8)) {
    return error("unsupported wav format");
  }

  stream = calloc(1, sizeof(*stream));
  if (!stream) {
    return error("allocation failed");
  }
  stream->wav = wav;

  if (ownsdata) {
    stream->data = data;
  }
  stream->idx = 0;

  info->udata = stream;
  info->handler = wav_handler;
  info->samplerate = wav.samplerate;
  info->length = wav.length;

  /* Return NULL (no error) for success */
  return NULL;
}


/*============================================================================
** Ogg stream
**============================================================================*/

#ifdef CM_USE_STB_VORBIS

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

typedef struct {
  stb_vorbis *ogg;
  void *data;
} OggStream;


static void ogg_handler(cm_Event *e) {
  int n, len;
  OggStream *s = e->udata;
  cm_Int16 *buf;

  switch (e->type) {

    case CM_EVENT_DESTROY:
      stb_vorbis_close(s->ogg);
      free(s->data);
      free(s);
      break;

    case CM_EVENT_SAMPLES:
      len = e->length;
      buf = e->buffer;
fill:
      n = stb_vorbis_get_samples_short_interleaved(s->ogg, 2, buf, len);
      n *= 2;
      /* rewind and fill remaining buffer if we reached the end of the ogg
      ** before filling it */
      if (len != n) {
        stb_vorbis_seek_start(s->ogg);
        buf += n;
        len -= n;
        goto fill;
      }
      break;

    case CM_EVENT_REWIND:
      stb_vorbis_seek_start(s->ogg);
      break;
  }
}


static const char* ogg_init(cm_SourceInfo *info, void *data, int len, int ownsdata) {
  OggStream *stream;
  stb_vorbis *ogg;
  stb_vorbis_info ogginfo;
  int err;

  ogg = stb_vorbis_open_memory(data, len, &err, NULL);
  if (!ogg) {
    return error("invalid ogg data");
  }

  stream = calloc(1, sizeof(*stream));
  if (!stream) {
    stb_vorbis_close(ogg);
    return error("allocation failed");
  }

  stream->ogg = ogg;
  if (ownsdata) {
    stream->data = data;
  }

  ogginfo = stb_vorbis_get_info(ogg);

  info->udata = stream;
  info->handler = ogg_handler;
  info->samplerate = ogginfo.sample_rate;
  info->length = stb_vorbis_stream_length_in_samples(ogg);

  /* Return NULL (no error) for success */
  return NULL;
}


#endif
