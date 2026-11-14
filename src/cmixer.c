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


typedef struct cm_Edge {
  struct cm_Edge *next;         /* Next edge in the source's outgoing list */
  struct cm_Edge *innext;       /* Next edge in the destination's incoming list */
  cm_Bus *from;                 /* Upstream bus */
  cm_Bus *to;                   /* Downstream bus */
  double gain;                  /* Edge gain */
} cm_Edge;


struct cm_Bus {
  cm_Bus *next;                 /* Next bus in the list */
  int delay;                    /* Delay in output frames (0 = plain bus) */
  double feedback;              /* Delay feedback, in [0, 1) */
  double gain;                  /* Bus output gain */
  int nofree;                   /* Set for the permanent master bus */
  double *histx;                /* Effect input history, interleaved L/R */
  double *histy;                /* Effect output history, interleaved L/R */
  int histlen;                  /* Delay history length in frames */
  int pos;                      /* Current delay history write index */
  int done;                     /* Set while mixing the current chunk */
  double *acc;                  /* Current chunk input sum */
  double *outbuf;               /* Current chunk output */
  cm_Edge *in_edges;            /* Incoming (upstream) edges */
  cm_Edge *out_edges;           /* Outgoing (downstream) edges */
};


struct cm_Source {
  cm_Source *next;              /* Next source in the playing list */
  cm_Source *allnext;           /* Next source in the list of all sources */
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
  double gain;          /* Gain set by `cm_set_gain()` */
  double pan;           /* Pan set by `cm_set_pan()` */
  cm_Bus *bus;          /* Destination bus */
};


static struct {
  const char *lasterror;        /* Last error message */
  cm_EventHandler lock;         /* Event handler for lock/unlock events */
  cm_Source *sources;           /* Linked list of active (playing) sources */
  cm_Source *allsources;        /* Linked list of all live sources */
  cm_Int32 buffer[BUFFER_SIZE]; /* Internal master buffer */
  int samplerate;               /* Master samplerate */
  int gain;                     /* Master gain (fixed point) */
  cm_Bus *buses;                /* Linked list of all buses */
  cm_Bus master;                /* Permanent master output bus */
  double master_acc[BUFFER_SIZE];  /* Master bus input accumulator */
  double master_out[BUFFER_SIZE];  /* Master bus output */
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


static void cm_dsp_init(void) {
  memset(&cmixer.master, 0, sizeof(cmixer.master));
  cmixer.master.gain = 1.;
  cmixer.master.nofree = 1;
  cmixer.master.next = NULL;
  cmixer.master.acc = cmixer.master_acc;
  cmixer.master.outbuf = cmixer.master_out;
  cmixer.buses = &cmixer.master;
}


void cm_init(int samplerate) {
  cmixer.samplerate = samplerate;
  cmixer.lock = dummy_handler;
  cmixer.sources = NULL;
  cmixer.allsources = NULL;
  cmixer.gain = FX_UNIT;
  cm_dsp_init();
}


/*============================================================================
** Bus routing
**============================================================================*/

static cm_Edge* find_edge(cm_Bus *from, cm_Bus *to) {
  cm_Edge *e;
  for (e = from->out_edges; e; e = e->next) {
    if (e->to == to) {
      return e;
    }
  }
  return NULL;
}


static int is_finite(double x) {
  /* Written without relying on `isfinite()` so that it also builds without
  ** `-lm` on platforms that lack it; NaN and infinity both fail here. */
  return x == x && x - x == 0.;
}


/* Returns non-zero if `to` can be reached from `from` by following edges. */
static int reaches(cm_Bus *from, cm_Bus *to) {
  cm_Edge *e;
  for (e = from->out_edges; e; e = e->next) {
    if (e->to == to || reaches(e->to, to)) {
      return 1;
    }
  }
  return 0;
}


/* Pulls the output of one bus for the current chunk. All upstream buses must
** already have been mixed; their outputs are summed into this bus input.
** `len` is rounded down to a whole number of stereo frames, matching the
** source mixer's handling of an odd sample count. */
static void mix_bus(cm_Bus *bus, int len) {
  cm_Edge *e;
  int i;

  len &= ~1;
  for (e = bus->in_edges; e; e = e->innext) {
    for (i = 0; i < len; i++) {
      bus->acc[i] += e->from->outbuf[i] * e->gain;
    }
  }

  for (i = 0; i < len; i += 2) {
    if (bus->delay > 0) {
      double l, r;
      int rp = bus->pos - bus->delay;
      if (rp < 0) {
        rp += bus->histlen;
      }
      /* y[n] = x[n-D] + f * y[n-D], per channel; an input frame with a
      ** negative index reads zero. The current frame's input is stored
      ** afterwards and never fed back, and bus/edge gains are applied to
      ** the output only, never to this state. */
      l = bus->histx[rp * 2    ] + bus->feedback * bus->histy[rp * 2    ];
      r = bus->histx[rp * 2 + 1] + bus->feedback * bus->histy[rp * 2 + 1];
      bus->histx[bus->pos * 2    ] = bus->acc[i    ];
      bus->histx[bus->pos * 2 + 1] = bus->acc[i + 1];
      bus->histy[bus->pos * 2    ] = l;
      bus->histy[bus->pos * 2 + 1] = r;
      bus->pos++;
      if (bus->pos >= bus->histlen) {
        bus->pos = 0;
      }
      bus->outbuf[i    ] = l * bus->gain;
      bus->outbuf[i + 1] = r * bus->gain;
    } else {
      /* A plain bus outputs the sum of its current inputs */
      bus->outbuf[i    ] = bus->acc[i    ] * bus->gain;
      bus->outbuf[i + 1] = bus->acc[i + 1] * bus->gain;
    }
  }
  bus->done = 1;
}


/* Rounds a mixed sample to the nearest long, keeping huge (but finite) values
** away from the undefined floating-to-integer conversion range. */
static cm_Int32 to_int32(double x) {
  if (!(x > -2147483000.)) {
    return x != x ? 0 : -2147483000;
  }
  if (x > 2147483000.) {
    return 2147483000;
  }
  return (cm_Int32) (x + (x >= 0. ? 0.5 : -0.5));
}


/* Mixes the whole graph. The order is computed with Kahn's algorithm over the
** bus linked lists, so no allocation or recursion is needed during rendering.
** Every bus is mixed once per chunk, even if it is disconnected from the
** master: this keeps the effects' clocks advancing. */
static void mix_graph(int len) {
  int count = 0, pass;
  cm_Bus *b;

  for (b = cmixer.buses; b; b = b->next) {
    b->done = 0;
    count++;
  }
  for (pass = 0; pass < count; pass++) {
    int progressed = 0;
    for (b = cmixer.buses; b; b = b->next) {
      cm_Edge *e;
      int ready = 1;
      if (b->done) {
        continue;
      }
      for (e = b->in_edges; e; e = e->innext) {
        if (!e->from->done) {
          ready = 0;
          break;
        }
      }
      if (ready) {
        mix_bus(b, len);
        progressed = 1;
      }
    }
    if (!progressed) {
      break;
    }
  }
}


/*============================================================================
** Auxiliary buses
**============================================================================*/

static int valid_bus(cm_Bus *bus) {
  cm_Bus *b;
  for (b = cmixer.buses; b; b = b->next) {
    if (b == bus) {
      return 1;
    }
  }
  return 0;
}


static cm_Bus* new_bus(int delay, double feedback, double gain) {
  cm_Bus *bus = calloc(1, sizeof(*bus));
  if (!bus) {
    error("allocation failed");
    return NULL;
  }
  bus->delay = delay;
  bus->feedback = feedback;
  bus->gain = gain;
  bus->acc = calloc(BUFFER_SIZE, sizeof(bus->acc[0]));
  bus->outbuf = calloc(BUFFER_SIZE, sizeof(bus->outbuf[0]));
  if (delay > 0) {
    bus->histlen = delay;
    bus->histx = calloc((size_t) bus->histlen * 2, sizeof(bus->histx[0]));
    bus->histy = calloc((size_t) bus->histlen * 2, sizeof(bus->histy[0]));
  }
  if (!bus->acc || !bus->outbuf || (delay > 0 && (!bus->histx || !bus->histy))) {
    free(bus->acc);
    free(bus->outbuf);
    free(bus->histx);
    free(bus->histy);
    free(bus);
    error("allocation failed");
    return NULL;
  }
  bus->next = cmixer.master.next;
  cmixer.master.next = bus;
  return bus;
}


cm_Bus* cm_get_master_bus(void) {
  return &cmixer.master;
}


cm_Bus* cm_new_bus(double gain) {
  if (!is_finite(gain)) {
    error("invalid gain");
    return NULL;
  }
  return new_bus(0, 0., gain);
}


cm_Bus* cm_new_delay_bus(int delay, double feedback) {
  if (delay <= 0) {
    error("invalid delay");
    return NULL;
  }
  if (!is_finite(feedback) || feedback < 0. || feedback >= 1.) {
    error("invalid feedback");
    return NULL;
  }
  return new_bus(delay, feedback, 1.);
}


void cm_set_bus_gain(cm_Bus *bus, double gain) {
  if (!valid_bus(bus) || !is_finite(gain)) {
    error("invalid bus or gain");
    return;
  }
  bus->gain = gain;
}


int cm_destroy_bus(cm_Bus *bus) {
  cm_Bus *b;
  cm_Source *src;

  if (!valid_bus(bus) || bus->nofree) {
    error("cannot destroy bus");
    return 0;
  }
  for (src = cmixer.allsources; src; src = src->allnext) {
    if (src->bus == bus) {
      error("bus is in use");
      return 0;
    }
  }
  if (bus->in_edges || bus->out_edges) {
    error("bus has edges");
    return 0;
  }
  for (b = cmixer.buses; b; b = b->next) {
    if (b->next == bus) {
      b->next = bus->next;
      break;
    }
  }
  free(bus->acc);
  free(bus->outbuf);
  free(bus->histx);
  free(bus->histy);
  free(bus);
  return 1;
}


cm_Bus* cm_get_source_bus(cm_Source *src) {
  return src->bus;
}


void cm_set_source_bus(cm_Source *src, cm_Bus *bus) {
  if (!valid_bus(bus)) {
    error("invalid bus");
    return;
  }
  src->bus = bus;
}


void cm_set_source_master(cm_Source *src) {
  src->bus = &cmixer.master;
}


/* Returns non-zero if a new edge `from -> to` would close a cycle. */
static int would_cycle(cm_Bus *from, cm_Bus *to) {
  return from == to || reaches(to, from);
}


int cm_connect_bus(cm_Bus *from, cm_Bus *to, double gain) {
  cm_Edge *e;

  if (!valid_bus(from) || !valid_bus(to)) {
    error("invalid bus");
    return 0;
  }
  if (!is_finite(gain)) {
    error("invalid gain");
    return 0;
  }
  if (from == &cmixer.master) {
    error("master output cannot be an edge source");
    return 0;
  }
  if (from == to) {
    error("edge would create a cycle");
    return 0;
  }
  e = find_edge(from, to);
  if (e) {
    e->gain = gain;
    return 1;
  }
  if (would_cycle(from, to)) {
    error("edge would create a cycle");
    return 0;
  }
  e = calloc(1, sizeof(*e));
  if (!e) {
    error("allocation failed");
    return 0;
  }
  e->from = from;
  e->to = to;
  e->gain = gain;
  e->next = from->out_edges;
  from->out_edges = e;
  e->innext = to->in_edges;
  to->in_edges = e;
  return 1;
}


int cm_set_edge_gain(cm_Bus *from, cm_Bus *to, double gain) {
  cm_Edge *e;
  if (!valid_bus(from) || !valid_bus(to)) {
    error("invalid bus");
    return 0;
  }
  e = find_edge(from, to);
  if (!e) {
    error("no such edge");
    return 0;
  }
  if (!is_finite(gain)) {
    error("invalid gain");
    return 0;
  }
  e->gain = gain;
  return 1;
}


int cm_disconnect_bus(cm_Bus *from, cm_Bus *to) {
  cm_Edge **p;
  cm_Edge **q;
  cm_Edge *e;

  if (!valid_bus(from) || !valid_bus(to)) {
    error("invalid bus");
    return 0;
  }
  p = &from->out_edges;
  while (*p && (*p)->to != to) {
    p = &(*p)->next;
  }
  e = *p;
  if (!e) {
    error("no such edge");
    return 0;
  }
  *p = e->next;
  q = &to->in_edges;
  while (*q && *q != e) {
    q = &(*q)->innext;
  }
  if (*q) {
    *q = e->innext;
  }
  free(e);
  return 1;
}


void cm_clear_bus_history(cm_Bus *bus) {
  if (!valid_bus(bus) || bus->delay <= 0) {
    return;
  }
  memset(bus->histx, 0, (size_t) bus->histlen * 2 * sizeof(bus->histx[0]));
  memset(bus->histy, 0, (size_t) bus->histlen * 2 * sizeof(bus->histy[0]));
  bus->pos = 0;
}


void cm_set_lock(cm_EventHandler lock) {
  cmixer.lock = lock;
}


void cm_set_master_gain(double gain) {
  cmixer.gain = FX_FROM_FLOAT(gain);
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


static void process_source(cm_Source *src, int len, double *dst) {
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

    /* Add audio to the destination bus */
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


void cm_process(cm_Int16 *dst, int len) {
  int i;
  cm_Source **s;

  /* Process in chunks of BUFFER_SIZE if `len` is larger than BUFFER_SIZE */
  while (len > BUFFER_SIZE) {
    cm_process(dst, BUFFER_SIZE);
    dst += BUFFER_SIZE;
    len -= BUFFER_SIZE;
  }

  /* Zeroset internal buffer */
  memset(cmixer.buffer, 0, len * sizeof(cmixer.buffer[0]));

  /* Clear the input accumulators of all buses. The source loop below runs
  ** before the graph mix, so only the non-master buses need clearing here. */
  {
    cm_Bus *b;
    for (b = cmixer.buses; b; b = b->next) {
      if (b->acc) {
        memset(b->acc, 0, len * sizeof(b->acc[0]));
      }
    }
  }

  /* Process active sources into their destination buses */
  lock();
  s = &cmixer.sources;
  while (*s) {
    process_source(*s, len, (*s)->bus->acc);
    /* Remove source from list if it is no longer playing */
    if ((*s)->state != CM_STATE_PLAYING) {
      (*s)->active = 0;
      *s = (*s)->next;
    } else {
      s = &(*s)->next;
    }
  }
  unlock();

  /* Mix the bus graph. Every node is mixed after all of its upstream nodes;
  ** effects advance for every chunk, even if nothing is connected to them. */
  mix_graph(len);
  for (i = 0; i < len; i++) {
    cmixer.buffer[i] = to_int32(cmixer.master.outbuf[i]);
  }

  /* Copy internal buffer to destination and clip */
  for (i = 0; i < len; i++) {
    int x = (cmixer.buffer[i] * cmixer.gain) >> FX_BITS;
    dst[i] = CLAMP(x, -32768, 32767);
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
  cm_set_gain(src, 1);
  cm_set_pan(src, 0);
  cm_set_pitch(src, 1);
  cm_set_loop(src, 0);
  src->bus = &cmixer.master;
  src->allnext = cmixer.allsources;
  cmixer.allsources = src;
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
  cm_Source **a;
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
  /* Detach from the list of all live sources so that routing never refers
  ** to freed memory */
  a = &cmixer.allsources;
  while (*a) {
    if (*a == src) {
      *a = src->allnext;
      break;
    }
    a = &(*a)->allnext;
  }
  unlock();
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
  src->lgain = FX_FROM_FLOAT(l);
  src->rgain = FX_FROM_FLOAT(r);
}


void cm_set_gain(cm_Source *src, double gain) {
  src->gain = gain;
  recalc_source_gains(src);
}


void cm_set_pan(cm_Source *src, double pan) {
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
  src->rate = FX_FROM_FLOAT(rate);
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
