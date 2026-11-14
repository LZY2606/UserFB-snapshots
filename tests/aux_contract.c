#include "task_support/check.h"
#include "src/cmixer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*============================================================================
** Deterministic test signals
**============================================================================*/

/* The generated signal depends only on the absolute frame index and channel,
** so it is unaffected by how the library chunks its processing. */
static int gen_at(int frame, int channel, int seed) {
  unsigned v = (unsigned) (frame + seed * 7919) * 1103515245u;
  v += 12345u + (unsigned) channel * 2654435761u;
  v ^= v >> 13;
  v *= 2654435761u;
  return (int) ((v >> 16) & 0x7ffu) - 1024;
}

typedef struct {
  int pos;
  int seed;
} TestStream;

/* Streams live for the whole test process so handlers never see stale memory */
static TestStream stream_pool[64];
static int stream_used;

static void stream_reset(void) {
  stream_used = 0;
}

static TestStream* stream_new(int seed) {
  TestStream *s = &stream_pool[stream_used++];
  memset(s, 0, sizeof(*s));
  s->seed = seed;
  return s;
}

static void test_handler(cm_Event *e) {
  TestStream *s = e->udata;
  int i;
  switch (e->type) {
    case CM_EVENT_REWIND:
      s->pos = 0;
      break;
    case CM_EVENT_SAMPLES:
      for (i = 0; i < e->length; i += 2) {
        e->buffer[i    ] = (cm_Int16) gen_at(s->pos, 0, s->seed);
        e->buffer[i + 1] = (cm_Int16) gen_at(s->pos, 1, s->seed);
        s->pos++;
      }
      break;
    default:
      break;
  }
}

static cm_Source* new_test_source(int samplerate, int length, int seed) {
  cm_SourceInfo info;
  TestStream *s = stream_new(seed);
  info.handler = test_handler;
  info.udata = s;
  info.samplerate = samplerate;
  info.length = length;
  return cm_new_source(&info);
}

/* Round the way cm_process does for the master output */
static int round_i16(double v) {
  double r = v + (v >= 0. ? 0.5 : -0.5);
  r = (double) (long) r;
  if (r > 32767.) r = 32767.;
  if (r < -32768.) r = -32768.;
  return (int) r;
}

static int near_i16(cm_Int16 actual, double expected) {
  int diff = (int) actual - round_i16(expected);
  return diff >= -1 && diff <= 1;
}

/*============================================================================
** Independent delay reference: y[n] = x[n-D] + f*y[n-D]
**============================================================================*/

#define REF_CAP 512

typedef struct {
  double x[REF_CAP][2];
  double y[REF_CAP][2];
  int d;
  int pos;
} RefDelay;

static void ref_reset(RefDelay *r, int d) {
  memset(r, 0, sizeof(*r));
  r->d = d;
  r->pos = 0;
}

static void ref_step(RefDelay *r, double inl, double inr, double feedback,
                     double *outl, double *outr) {
  int rp = r->pos - r->d;
  if (rp < 0) rp += REF_CAP;
  *outl = r->x[rp][0] + feedback * r->y[rp][0];
  *outr = r->x[rp][1] + feedback * r->y[rp][1];
  r->x[r->pos][0] = inl;
  r->x[r->pos][1] = inr;
  r->y[r->pos][0] = *outl;
  r->y[r->pos][1] = *outr;
  r->pos = (r->pos + 1) % REF_CAP;
}

/*============================================================================
** Generated WAV support
**============================================================================*/

/* Writes a 16-bit stereo PCM WAV into `data` and returns its size. */
static int make_wav(unsigned char *data, int frames, int samplerate, int seed) {
  int i, n = frames * 4;
  unsigned int riff = 36 + (unsigned) n;
  unsigned int sz16 = 16, szdata = (unsigned) n;
  unsigned int sr = (unsigned) samplerate;
  unsigned int br = sr * 4;
  unsigned short fmt = 1, chans = 2, align = 4, bits = 16;
  memcpy(data + 0, "RIFF", 4);
  memcpy(data + 4, &riff, 4);
  memcpy(data + 8, "WAVE", 4);
  memcpy(data + 12, "fmt ", 4);
  memcpy(data + 16, &sz16, 4);
  memcpy(data + 20, &fmt, 2);
  memcpy(data + 22, &chans, 2);
  memcpy(data + 24, &sr, 4);
  memcpy(data + 28, &br, 4);
  memcpy(data + 32, &align, 2);
  memcpy(data + 34, &bits, 2);
  memcpy(data + 36, "data", 4);
  memcpy(data + 40, &szdata, 4);
  for (i = 0; i < frames; i++) {
    cm_Int16 l = (cm_Int16) gen_at(i, 0, seed);
    cm_Int16 r = (cm_Int16) gen_at(i, 1, seed);
    memcpy(data + 44 + i * 4, &l, 2);
    memcpy(data + 44 + i * 4 + 2, &r, 2);
  }
  return 44 + n;
}

/*============================================================================
** routing cases
**============================================================================*/

/* Two sources sum into one plain bus; the bus output is the input sum and a
** routed source is consumed exactly once. */
static int case_routing_sum(void) {
  cm_Source *s1, *s2;
  cm_Bus *bus;
  cm_Int16 out[400];
  int i;
  cm_init(48000);
  stream_reset();
  s1 = new_test_source(48000, 4096, 3);
  s2 = new_test_source(48000, 4096, 11);
  TASK_CHECK(s1 && s2);
  bus = cm_new_bus(1.);
  TASK_CHECK(bus != NULL);
  TASK_CHECK(cm_connect_bus(bus, cm_get_master_bus(), 1.));
  cm_set_source_bus(s1, bus);
  cm_set_source_bus(s2, bus);
  cm_play(s1);
  cm_play(s2);
  cm_process(out, 400);
  for (i = 0; i < 200; i++) {
    TASK_CHECK(near_i16(out[i * 2    ], gen_at(i, 0, 3) + gen_at(i, 0, 11)));
    TASK_CHECK(near_i16(out[i * 2 + 1], gen_at(i, 1, 3) + gen_at(i, 1, 11)));
  }
  TASK_CHECK(fabs(cm_get_position(s1) - 200.0 / 48000.) < 1e-9);
  TASK_CHECK(fabs(cm_get_position(s2) - 200.0 / 48000.) < 1e-9);
  TASK_CHECK(out[0] != out[1]);
  return 0;
}

/* A diamond: source -> dry bus; dry feeds master directly and also a delay
** bus that feeds master. Checked frame by frame against an independent
** simulation, including that both paths hear the same single decode pass. */
static int case_routing_diamond(void) {
  RefDelay wet;
  cm_Source *src;
  cm_Bus *dry, *wetbus;
  cm_Int16 out[600];
  double dry_gain = 1., wet_gain = 0.75;
  double dry_edge = 0.4, wet_edge = 0.9;
  double fb = 0.375;
  int i;
  cm_init(48000);
  stream_reset();
  src = new_test_source(48000, 4096, 5);
  dry = cm_new_bus(dry_gain);
  wetbus = cm_new_delay_bus(9, fb);
  TASK_CHECK(src && dry && wetbus);
  TASK_CHECK(cm_connect_bus(dry, wetbus, wet_gain));
  TASK_CHECK(cm_connect_bus(wetbus, cm_get_master_bus(), wet_edge));
  TASK_CHECK(cm_connect_bus(dry, cm_get_master_bus(), dry_edge));
  cm_set_source_bus(src, dry);
  cm_play(src);
  cm_process(out, 600);
  ref_reset(&wet, 9);
  for (i = 0; i < 300; i++) {
    double xl = gen_at(i, 0, 5) * dry_gain;
    double xr = gen_at(i, 1, 5) * dry_gain;
    double wl, wr;
    ref_step(&wet, xl, xr, fb, &wl, &wr);
    TASK_CHECK(near_i16(out[i * 2    ],
        xl * dry_edge + (wl * wet_gain) * wet_edge));
    TASK_CHECK(near_i16(out[i * 2 + 1],
        xr * dry_edge + (wr * wet_gain) * wet_edge));
  }
  TASK_CHECK(fabs(cm_get_position(src) - 300.0 / 48000.) < 1e-9);
  return 0;
}

/* A plain bus with unit gains is transparent: routing a source through it
** must not reapply source gain, pan or pitch. Compared against the same source
** mixed directly into the master. */
static int case_routing_transparent(void) {
  cm_Int16 direct[1200];
  cm_Int16 routed[1200];
  double direct_pos;

  cm_init(48000);
  stream_reset();
  {
    cm_Source *src = new_test_source(32000, 4096, 73);
    TASK_CHECK(src != NULL);
    cm_set_gain(src, 0.8);
    cm_set_pan(src, -0.5);
    cm_set_pitch(src, 1.25);
    cm_play(src);
    cm_process(direct, 1200);
    direct_pos = cm_get_position(src);
    TASK_CHECK(direct_pos > 0.);
  }

  cm_init(48000);
  stream_reset();
  {
    cm_Source *src = new_test_source(32000, 4096, 73);
    cm_Bus *bus = cm_new_bus(1.);
    TASK_CHECK(src && bus);
    TASK_CHECK(cm_connect_bus(bus, cm_get_master_bus(), 1.));
    cm_set_gain(src, 0.8);
    cm_set_pan(src, -0.5);
    cm_set_pitch(src, 1.25);
    cm_set_source_bus(src, bus);
    cm_play(src);
    cm_process(routed, 1200);
    TASK_CHECK(fabs(cm_get_position(src) - direct_pos) < 1e-9);
  }
  TASK_CHECK(memcmp(direct, routed, sizeof(direct)) == 0);
  return 0;
}

/* A half-rate source crossing internal buffer boundaries: one long read and
** unequal short reads must produce the identical sound. */
static int case_routing_chunking(void) {
  cm_Int16 whole[1600];
  cm_Int16 pieces[1600];
  static const int lens[] = {2, 6, 12, 30, 64, 100, 170, 214, 256, 300, 384, 62};
  unsigned k;
  int off, i;

  cm_init(48000);
  stream_reset();
  {
    cm_Source *src = new_test_source(24000, 4096, 7);
    cm_Bus *dry = cm_new_bus(1.);
    cm_Bus *del = cm_new_delay_bus(37, 0.3);
    TASK_CHECK(src && dry && del);
    TASK_CHECK(cm_connect_bus(dry, del, 1.));
    TASK_CHECK(cm_connect_bus(del, cm_get_master_bus(), 1.));
    TASK_CHECK(cm_connect_bus(dry, cm_get_master_bus(), 0.5));
    cm_set_source_bus(src, dry);
    cm_play(src);
    cm_process(whole, 1600);
  }

  cm_init(48000);
  stream_reset();
  {
    cm_Source *src = new_test_source(24000, 4096, 7);
    cm_Bus *dry = cm_new_bus(1.);
    cm_Bus *del = cm_new_delay_bus(37, 0.3);
    TASK_CHECK(src && dry && del);
    TASK_CHECK(cm_connect_bus(dry, del, 1.));
    TASK_CHECK(cm_connect_bus(del, cm_get_master_bus(), 1.));
    TASK_CHECK(cm_connect_bus(dry, cm_get_master_bus(), 0.5));
    cm_set_source_bus(src, dry);
    cm_play(src);
    memset(pieces, 0, sizeof(pieces));
    off = 0;
    for (k = 0; k < sizeof(lens) / sizeof(lens[0]) && off < 1600; k++) {
      int n = lens[k];
      if (off + n > 1600) n = 1600 - off;
      cm_process(pieces + off, n);
      off += n;
    }
  }
  for (i = 0; i < 1600; i++) {
    TASK_CHECK(whole[i] == pieces[i]);
  }
  return 0;
}

/* Two very large branches cancel at the master: intermediate values are far
** outside int16, so any premature clipping would be visible. */
static int case_routing_cancel(void) {
  cm_Source *src;
  cm_Bus *a, *b;
  cm_Int16 out[512];
  int i;
  cm_init(48000);
  stream_reset();
  src = new_test_source(48000, 4096, 13);
  a = cm_new_bus(1.);
  b = cm_new_bus(-1.);
  TASK_CHECK(src && a && b);
  TASK_CHECK(cm_connect_bus(a, cm_get_master_bus(), 400.));
  TASK_CHECK(cm_connect_bus(a, b, 1.));
  TASK_CHECK(cm_connect_bus(b, cm_get_master_bus(), 400.));
  cm_set_source_bus(src, a);
  cm_play(src);
  cm_process(out, 512);
  for (i = 0; i < 512; i++) {
    TASK_CHECK(out[i] == 0);
  }
  return 0;
}

/* Two delay levels of different lengths and feedback, checked frame by frame
** against independent per-channel delay states. Bus and edge gains sit
** between the levels and must not leak into the feedback state. */
static int case_routing_two_levels(void) {
  RefDelay d1, d2;
  cm_Source *src;
  cm_Bus *src_bus, *first, *second;
  cm_Int16 out[700];
  double f1 = 0.5, f2 = 0.25;
  double g1 = 0.75, g2 = 1.25, e1 = 0.6, e2 = 0.8;
  int i;
  cm_init(48000);
  stream_reset();
  src = new_test_source(48000, 4096, 17);
  src_bus = cm_new_bus(1.);
  first = cm_new_delay_bus(5, f1);
  second = cm_new_delay_bus(13, f2);
  TASK_CHECK(src && src_bus && first && second);
  TASK_CHECK(cm_connect_bus(src_bus, first, e1));
  TASK_CHECK(cm_connect_bus(first, second, e2));
  TASK_CHECK(cm_connect_bus(second, cm_get_master_bus(), e2));
  cm_set_bus_gain(first, g1);
  cm_set_bus_gain(second, g2);
  cm_set_source_bus(src, src_bus);
  cm_play(src);
  cm_process(out, 700);
  ref_reset(&d1, 5);
  ref_reset(&d2, 13);
  for (i = 0; i < 350; i++) {
    double xl = gen_at(i, 0, 17);
    double xr = gen_at(i, 1, 17);
    double y1l, y1r, y2l, y2r;
    ref_step(&d1, xl * e1, xr * e1, f1, &y1l, &y1r);
    ref_step(&d2, y1l * g1 * e2, y1r * g1 * e2, f2, &y2l, &y2r);
    TASK_CHECK(near_i16(out[i * 2    ], y2l * g2 * e2));
    TASK_CHECK(near_i16(out[i * 2 + 1], y2r * g2 * e2));
  }
  return 0;
}

/* A WAV source created from generated memory data, fed through a delay bus
** and checked against the same independent reference. */
static int case_routing_wav(void) {
  static unsigned char wav[44 + 300 * 4];
  RefDelay ref;
  cm_Source *src;
  cm_Bus *del;
  cm_Int16 out[608];
  int size, i;
  cm_init(48000);
  size = make_wav(wav, 300, 48000, 23);
  src = cm_new_source_from_mem(wav, size);
  TASK_CHECK(src != NULL);
  del = cm_new_delay_bus(17, 0.375);
  TASK_CHECK(del != NULL);
  TASK_CHECK(cm_connect_bus(del, cm_get_master_bus(), 1.));
  cm_set_source_bus(src, del);
  cm_play(src);
  cm_process(out, 608);
  ref_reset(&ref, 17);
  for (i = 0; i < 300; i++) {
    double yl, yr;
    ref_step(&ref, gen_at(i, 0, 23), gen_at(i, 1, 23), 0.375, &yl, &yr);
    TASK_CHECK(near_i16(out[i * 2    ], yl));
    TASK_CHECK(near_i16(out[i * 2 + 1], yr));
  }
  TASK_CHECK(cm_get_state(src) == CM_STATE_STOPPED);
  cm_destroy_source(src);
  TASK_CHECK(cm_disconnect_bus(del, cm_get_master_bus()) == 1);
  TASK_CHECK(cm_destroy_bus(del) == 1);
  return 0;
}

/*============================================================================
** tails cases
**============================================================================*/

/* A source that naturally stops mid-call is no longer consumed, but the
** delay tail keeps decaying for every produced frame. */
static int case_tails_stop(void) {
  RefDelay ref;
  cm_Source *src;
  cm_Bus *del;
  cm_Int16 out[80];
  int i, nonzero = 0;
  cm_init(48000);
  stream_reset();
  src = new_test_source(48000, 4, 29);
  del = cm_new_delay_bus(3, 0.5);
  TASK_CHECK(src && del);
  TASK_CHECK(cm_connect_bus(del, cm_get_master_bus(), 1.));
  cm_set_source_bus(src, del);
  cm_play(src);
  cm_process(out, 80);
  TASK_CHECK(cm_get_state(src) == CM_STATE_STOPPED);
  TASK_CHECK(fabs(cm_get_position(src)) < 1e-9);
  ref_reset(&ref, 3);
  for (i = 0; i < 40; i++) {
    double yl, yr;
    double xl = i < 4 ? gen_at(i, 0, 29) : 0.;
    double xr = i < 4 ? gen_at(i, 1, 29) : 0.;
    ref_step(&ref, xl, xr, 0.5, &yl, &yr);
    TASK_CHECK(near_i16(out[i * 2    ], yl));
    TASK_CHECK(near_i16(out[i * 2 + 1], yr));
    if (out[i * 2] != 0) nonzero = 1;
  }
  TASK_CHECK(nonzero);
  TASK_CHECK(abs((int) out[30 * 2]) >= abs((int) out[39 * 2]));
  /* Empty-graph reads still advance the effect clock */
  {
    cm_Int16 a[4], b[4];
    cm_process(a, 4);
    cm_process(b, 4);
    TASK_CHECK(abs((int) b[0]) <= abs((int) a[0]));
  }
  return 0;
}

/* Pausing stops source consumption and does not move the playhead; after
** resuming it continues from the same position while the delay tail keeps
** running on every produced frame. */
static int case_tails_pause(void) {
  RefDelay ref;
  cm_Source *src;
  cm_Bus *del;
  cm_Int16 out[12];
  cm_Int16 first[20];
  cm_Int16 scratch[64];
  int i;
  cm_init(48000);
  stream_reset();
  src = new_test_source(48000, 4096, 31);
  del = cm_new_delay_bus(4, 0.5);
  TASK_CHECK(src && del);
  TASK_CHECK(cm_connect_bus(del, cm_get_master_bus(), 1.));
  cm_set_source_bus(src, del);
  cm_play(src);
  cm_process(first, 20);                   /* 10 frames consumed */
  TASK_CHECK(fabs(cm_get_position(src) - 10.0 / 48000.) < 1e-9);
  cm_pause(src);
  TASK_CHECK(cm_get_state(src) == CM_STATE_PAUSED);
  cm_process(scratch, 64);                 /* paused: source not consumed */
  TASK_CHECK(fabs(cm_get_position(src) - 10.0 / 48000.) < 1e-9);
  cm_play(src);
  cm_process(out, 12);                     /* six resumed frames */
  ref_reset(&ref, 4);
  for (i = 0; i < 10; i++) {
    double yl, yr;
    ref_step(&ref, gen_at(i, 0, 31), gen_at(i, 1, 31), 0.5, &yl, &yr);
  }
  for (i = 0; i < 32; i++) {               /* 32 paused frames */
    double yl, yr;
    ref_step(&ref, 0., 0., 0.5, &yl, &yr);
  }
  for (i = 0; i < 6; i++) {
    double yl, yr;
    ref_step(&ref, gen_at(10 + i, 0, 31), gen_at(10 + i, 1, 31), 0.5,
             &yl, &yr);
    TASK_CHECK(near_i16(out[i * 2    ], yl));
    TASK_CHECK(near_i16(out[i * 2 + 1], yr));
  }
  TASK_CHECK(fabs(cm_get_position(src) - 16.0 / 48000.) < 1e-9);
  TASK_CHECK(cm_get_state(src) == CM_STATE_PLAYING);
  return 0;
}

/* Disconnecting an input edge keeps the delay history; reconnecting must not
** replay anything from the disconnected period. */
static int case_tails_reconnect(void) {
  RefDelay ref;
  cm_Source *src;
  cm_Bus *dry, *del;
  cm_Int16 out[120];
  int i;
  cm_init(48000);
  stream_reset();
  src = new_test_source(48000, 4096, 37);
  dry = cm_new_bus(1.);
  del = cm_new_delay_bus(4, 0.5);
  TASK_CHECK(src && dry && del);
  TASK_CHECK(cm_connect_bus(dry, del, 1.));
  TASK_CHECK(cm_connect_bus(del, cm_get_master_bus(), 1.));
  cm_set_source_bus(src, dry);
  cm_play(src);
  cm_process(out, 40);                     /* 20 input frames */
  TASK_CHECK(cm_disconnect_bus(dry, del) == 1);
  cm_process(out + 40, 20);                /* 10 frames disconnected */
  TASK_CHECK(cm_connect_bus(dry, del, 1.) == 1);
  cm_process(out + 60, 60);                /* 30 frames reconnected */
  ref_reset(&ref, 4);
  for (i = 0; i < 60; i++) {
    double xl = (i < 20 || i >= 30) ? gen_at(i, 0, 37) : 0.;
    double xr = (i < 20 || i >= 30) ? gen_at(i, 1, 37) : 0.;
    double yl, yr;
    ref_step(&ref, xl, xr, 0.5, &yl, &yr);
    TASK_CHECK(near_i16(out[i * 2    ], yl));
    TASK_CHECK(near_i16(out[i * 2 + 1], yr));
  }
  return 0;
}

/* Destroying a source while a tail is in flight leaves no dangling route; the
** effect keeps running and the bus becomes destroyable once its edge is cut. */
static int case_tails_destroy(void) {
  RefDelay ref;
  cm_Source *src;
  cm_Bus *del;
  cm_Int16 out[64];
  int i;
  cm_init(48000);
  stream_reset();
  src = new_test_source(48000, 4096, 41);
  del = cm_new_delay_bus(4, 0.5);
  TASK_CHECK(src && del);
  TASK_CHECK(cm_connect_bus(del, cm_get_master_bus(), 1.));
  cm_set_source_bus(src, del);
  cm_play(src);
  cm_process(out, 20);                     /* 10 input frames */
  cm_destroy_source(src);
  TASK_CHECK(cm_destroy_bus(del) == 0);    /* still has an output edge */
  cm_process(out, 64);                     /* tail continues, no crash */
  ref_reset(&ref, 4);
  for (i = 0; i < 10; i++) {
    double yl, yr;
    ref_step(&ref, gen_at(i, 0, 41), gen_at(i, 1, 41), 0.5, &yl, &yr);
  }
  for (i = 10; i < 32; i++) {
    double yl, yr;
    ref_step(&ref, 0., 0., 0.5, &yl, &yr);
    TASK_CHECK(near_i16(out[(i - 10) * 2    ], yl));
    TASK_CHECK(near_i16(out[(i - 10) * 2 + 1], yr));
  }
  TASK_CHECK(cm_disconnect_bus(del, cm_get_master_bus()) == 1);
  TASK_CHECK(cm_destroy_bus(del) == 1);
  return 0;
}

/* Clearing one delay bus only resets that bus's channel state; the source
** position and a second delay bus are untouched. */
static int case_tails_clear_isolated(void) {
  RefDelay ra, rb;
  cm_Int16 with_clear[1100];
  cm_Int16 without_clear[1100];
  double pos_a, pos_b;
  int i, differ = 0;

  cm_init(48000);
  stream_reset();
  {
    cm_Source *a_src = new_test_source(48000, 4096, 43);
    cm_Source *b_src = new_test_source(48000, 4096, 47);
    cm_Bus *a = cm_new_delay_bus(5, 0.5);
    cm_Bus *b = cm_new_delay_bus(9, 0.25);
    TASK_CHECK(a_src && b_src && a && b);
    TASK_CHECK(cm_connect_bus(a, cm_get_master_bus(), 1.));
    TASK_CHECK(cm_connect_bus(b, cm_get_master_bus(), 1.));
    cm_set_source_bus(a_src, a);
    cm_set_source_bus(b_src, b);
    cm_play(a_src);
    cm_play(b_src);
    cm_process(without_clear, 100);
    pos_a = cm_get_position(a_src);
    pos_b = cm_get_position(b_src);
    TASK_CHECK(fabs(pos_a - 50.0 / 48000.) < 1e-9);
    TASK_CHECK(fabs(pos_b - 50.0 / 48000.) < 1e-9);
    /* The identical second half: no clear happened */
    cm_process(without_clear + 100, 1000);
  }

  cm_init(48000);
  stream_reset();
  {
    cm_Source *a_src = new_test_source(48000, 4096, 43);
    cm_Source *b_src = new_test_source(48000, 4096, 47);
    cm_Bus *a = cm_new_delay_bus(5, 0.5);
    cm_Bus *b = cm_new_delay_bus(9, 0.25);
    TASK_CHECK(a_src && b_src && a && b);
    TASK_CHECK(cm_connect_bus(a, cm_get_master_bus(), 1.));
    TASK_CHECK(cm_connect_bus(b, cm_get_master_bus(), 1.));
    cm_set_source_bus(a_src, a);
    cm_set_source_bus(b_src, b);
    cm_play(a_src);
    cm_play(b_src);
    cm_process(with_clear, 100);
    TASK_CHECK(fabs(cm_get_position(a_src) - pos_a) < 1e-9);
    TASK_CHECK(fabs(cm_get_position(b_src) - pos_b) < 1e-9);
    cm_clear_bus_history(a);
    TASK_CHECK(fabs(cm_get_position(a_src) - pos_a) < 1e-9);
    TASK_CHECK(fabs(cm_get_position(b_src) - pos_b) < 1e-9);
    cm_process(with_clear + 100, 1000);
  }

  /* Only bus `a` changed, so exactly its old tail differs */
  for (i = 100; i < 1100; i++) {
    if (with_clear[i] != without_clear[i]) differ = 1;
  }
  TASK_CHECK(differ);
  /* First five frames after the clear: `a`'s history is empty (but its input
  ** keeps arriving), while `b` continues normally. */
  ref_reset(&ra, 5);
  ref_reset(&rb, 9);
  for (i = 0; i < 50; i++) {
    double yl, yr;
    ref_step(&rb, gen_at(i, 0, 47), gen_at(i, 1, 47), 0.25, &yl, &yr);
  }
  for (i = 0; i < 5; i++) {
    double ayl, ayr, byl, byr;
    ref_step(&ra, gen_at(50 + i, 0, 43), gen_at(50 + i, 1, 43), 0.5,
             &ayl, &ayr);
    ref_step(&rb, gen_at(50 + i, 0, 47), gen_at(50 + i, 1, 47), 0.25,
             &byl, &byr);
    TASK_CHECK(near_i16(with_clear[100 + i * 2    ], ayl + byl));
    TASK_CHECK(near_i16(with_clear[100 + i * 2 + 1], ayr + byr));
  }
  return 0;
}

/*============================================================================
** lifecycle cases
**============================================================================*/

/* Builds a three-bus chain fed by one source. Returns the source via `out`. */
static int build_chain(cm_Bus **a, cm_Bus **b, cm_Bus **c, int seed,
                       cm_Source **out) {
  cm_Source *src = new_test_source(48000, 4096, seed);
  *a = cm_new_bus(1.);
  *b = cm_new_delay_bus(6, 0.5);
  *c = cm_new_bus(1.);
  if (!src || !*a || !*b || !*c) return 0;
  if (!cm_connect_bus(*a, *b, 1.)) return 0;
  if (!cm_connect_bus(*b, *c, 0.7)) return 0;
  if (!cm_connect_bus(*c, cm_get_master_bus(), 0.9)) return 0;
  cm_set_source_bus(src, *a);
  cm_play(src);
  *out = src;
  return 1;
}

/* Every attempt to add a cycle must fail and leave the graph bit-identical.
** Rendered against a control graph that never suffers the rejected calls. */
static int case_lifecycle_reject_cycle(void) {
  cm_Bus *a, *b, *c;
  cm_Source *src;
  cm_Int16 control[200], probed[200];

  cm_init(48000);
  stream_reset();
  TASK_CHECK(build_chain(&a, &b, &c, 53, &src));
  cm_process(control, 200);

  cm_init(48000);
  stream_reset();
  TASK_CHECK(build_chain(&a, &b, &c, 53, &src));
  TASK_CHECK(cm_connect_bus(c, a, 1.) == 0);   /* long cycle */
  TASK_CHECK(cm_get_error() != NULL);
  TASK_CHECK(cm_connect_bus(a, a, 1.) == 0);   /* self edge */
  TASK_CHECK(cm_connect_bus(b, a, 1.) == 0);   /* short cycle */
  TASK_CHECK(cm_connect_bus(cm_get_master_bus(), a, 1.) == 0);
  /* No rejected edge was retained */
  TASK_CHECK(cm_set_edge_gain(c, a, 1.) == 0);
  TASK_CHECK(cm_set_edge_gain(b, a, 1.) == 0);
  TASK_CHECK(cm_set_edge_gain(a, b, 1.) == 1);
  TASK_CHECK(cm_set_edge_gain(b, c, 0.7) == 1);
  TASK_CHECK(cm_set_edge_gain(c, cm_get_master_bus(), 0.9) == 1);
  cm_process(probed, 200);
  TASK_CHECK(memcmp(control, probed, sizeof(control)) == 0);
  return 0;
}

/* Connecting the same pair twice only updates the edge gain. */
static int case_lifecycle_duplicate_edge(void) {
  cm_Int16 one[128], two[128];
  cm_Source *src;
  cm_Bus *a, *b;
  cm_init(48000);
  stream_reset();
  src = new_test_source(48000, 4096, 59);
  a = cm_new_bus(1.);
  b = cm_new_delay_bus(3, 0.5);
  TASK_CHECK(src && a && b);
  TASK_CHECK(cm_connect_bus(a, b, 1.));
  TASK_CHECK(cm_connect_bus(a, b, 0.25));
  TASK_CHECK(cm_connect_bus(b, cm_get_master_bus(), 1.));
  cm_set_source_bus(src, a);
  cm_play(src);
  cm_process(one, 128);

  cm_init(48000);
  stream_reset();
  src = new_test_source(48000, 4096, 59);
  a = cm_new_bus(1.);
  b = cm_new_delay_bus(3, 0.5);
  TASK_CHECK(cm_connect_bus(a, b, 1.));
  TASK_CHECK(cm_set_edge_gain(a, b, 0.25) == 1);
  TASK_CHECK(cm_connect_bus(b, cm_get_master_bus(), 1.));
  cm_set_source_bus(src, a);
  cm_play(src);
  cm_process(two, 128);
  TASK_CHECK(memcmp(one, two, sizeof(one)) == 0);

  TASK_CHECK(cm_disconnect_bus(a, b) == 1);
  TASK_CHECK(cm_disconnect_bus(a, b) == 0);
  return 0;
}

/* Invalid delay lengths, feedback values and gains are rejected before any
** state is modified; the sound of a running graph is unchanged. */
static int case_lifecycle_invalid_params(void) {
  cm_Int16 control[128], probed[128];
  volatile double zero = 0.;
  double inf = 1. / zero;
  double nan = zero / zero;
  cm_Source *src;
  cm_Bus *b;

  cm_init(48000);
  stream_reset();
  TASK_CHECK(cm_new_delay_bus(0, 0.5) == NULL);
  TASK_CHECK(cm_new_delay_bus(-2, 0.5) == NULL);
  TASK_CHECK(cm_new_delay_bus(4, -0.01) == NULL);
  TASK_CHECK(cm_new_delay_bus(4, 1.0) == NULL);
  TASK_CHECK(cm_new_delay_bus(4, nan) == NULL);
  TASK_CHECK(cm_new_delay_bus(4, inf) == NULL);
  TASK_CHECK(cm_new_bus(nan) == NULL);
  TASK_CHECK(cm_new_bus(inf) == NULL);

  src = new_test_source(48000, 4096, 61);
  b = cm_new_delay_bus(4, 0.5);
  TASK_CHECK(src && b);
  TASK_CHECK(cm_connect_bus(b, cm_get_master_bus(), 1.));
  cm_set_source_bus(src, b);
  cm_play(src);
  cm_set_bus_gain(b, 2.);
  cm_set_edge_gain(b, cm_get_master_bus(), 1.);
  cm_process(control, 128);

  cm_init(48000);
  stream_reset();
  src = new_test_source(48000, 4096, 61);
  b = cm_new_delay_bus(4, 0.5);
  TASK_CHECK(cm_connect_bus(b, cm_get_master_bus(), 1.));
  cm_set_source_bus(src, b);
  cm_play(src);
  cm_set_bus_gain(b, 2.);
  cm_set_edge_gain(b, cm_get_master_bus(), 1.);
  cm_set_bus_gain(b, nan);
  TASK_CHECK(cm_get_error() != NULL);
  cm_set_bus_gain(b, inf);
  TASK_CHECK(cm_get_error() != NULL);
  TASK_CHECK(cm_connect_bus(b, cm_get_master_bus(), nan) == 0);
  TASK_CHECK(cm_set_edge_gain(b, cm_get_master_bus(), inf) == 0);
  cm_process(probed, 128);
  TASK_CHECK(memcmp(control, probed, sizeof(control)) == 0);
  return 0;
}

/* Ownership: buses with source references or edges cannot be freed; a freed
** bus disappears from the graph; the master is permanent. */
static int case_lifecycle_ownership(void) {
  cm_Int16 control[128], probed[128];
  cm_Source *src;
  cm_Bus *a, *b;

  /* Control: plain graph, no rejected frees */
  cm_init(48000);
  stream_reset();
  src = new_test_source(48000, 4096, 67);
  a = cm_new_bus(1.);
  b = cm_new_bus(1.);
  TASK_CHECK(src && a && b);
  TASK_CHECK(cm_connect_bus(b, a, 1.));
  TASK_CHECK(cm_connect_bus(a, cm_get_master_bus(), 1.));
  cm_set_source_bus(src, b);
  cm_play(src);
  cm_process(control, 128);

  cm_init(48000);
  stream_reset();
  TASK_CHECK(cm_destroy_bus(cm_get_master_bus()) == 0);
  src = new_test_source(48000, 4096, 67);
  a = cm_new_bus(1.);
  b = cm_new_bus(1.);
  TASK_CHECK(src && a && b);
  TASK_CHECK(cm_connect_bus(b, a, 1.));
  TASK_CHECK(cm_connect_bus(a, cm_get_master_bus(), 1.));
  cm_set_source_bus(src, b);
  cm_play(src);
  TASK_CHECK(cm_destroy_bus(b) == 0);      /* source reference */
  TASK_CHECK(cm_destroy_bus(a) == 0);      /* in/out edges */
  cm_process(probed, 128);
  TASK_CHECK(memcmp(control, probed, sizeof(control)) == 0);

  /* Restoring the default route releases the source reference */
  cm_set_source_master(src);
  TASK_CHECK(cm_get_source_bus(src) == cm_get_master_bus());
  TASK_CHECK(cm_destroy_bus(b) == 0);      /* still has the b->a edge */
  TASK_CHECK(cm_disconnect_bus(b, a) == 1);
  TASK_CHECK(cm_destroy_bus(b) == 1);
  TASK_CHECK(cm_destroy_bus(b) == 0);      /* no longer a live bus */
  TASK_CHECK(cm_connect_bus(b, a, 1.) == 0);   /* dangling pointer rejected */
  TASK_CHECK(cm_disconnect_bus(a, cm_get_master_bus()) == 1);
  TASK_CHECK(cm_destroy_bus(a) == 1);
  cm_process(probed, 64);
  return 0;
}

/* The library's lock callback must never be entered recursively, and routing
** operations must not invoke it at all. */
static int lock_depth;
static int lock_violation;
static int lock_calls;

static void lock_handler(cm_Event *e) {
  lock_calls++;
  if (e->type == CM_EVENT_LOCK) {
    lock_depth++;
    if (lock_depth > 1) lock_violation = 1;
  } else if (e->type == CM_EVENT_UNLOCK) {
    lock_depth--;
  }
}

static int case_lifecycle_lock_not_nested(void) {
  cm_Source *src;
  cm_Bus *a, *b, *d;
  cm_Int16 out[64];
  int calls_before;
  cm_init(48000);
  stream_reset();
  lock_depth = lock_violation = lock_calls = 0;
  cm_set_lock(lock_handler);
  src = new_test_source(48000, 4096, 71);
  a = cm_new_bus(1.);
  b = cm_new_delay_bus(4, 0.5);
  d = cm_new_delay_bus(7, 0.25);
  TASK_CHECK(src && a && b && d);
  calls_before = lock_calls;
  TASK_CHECK(cm_connect_bus(a, b, 1.));
  TASK_CHECK(cm_connect_bus(b, d, 1.));
  TASK_CHECK(cm_connect_bus(d, cm_get_master_bus(), 1.));
  cm_set_bus_gain(a, 0.5);
  cm_set_edge_gain(a, b, 0.75);
  cm_set_source_bus(src, a);
  TASK_CHECK(lock_calls == calls_before);  /* routing does not take the lock */
  cm_play(src);                            /* play does */
  TASK_CHECK(lock_calls > calls_before);
  cm_process(out, 64);
  cm_pause(src);
  cm_process(out, 64);
  cm_play(src);
  cm_process(out, 64);
  TASK_CHECK(cm_disconnect_bus(a, b) == 1);
  TASK_CHECK(cm_connect_bus(a, b, 0.75) == 1);
  cm_process(out, 64);
  cm_destroy_source(src);
  cm_process(out, 64);
  TASK_CHECK(lock_depth == 0);
  TASK_CHECK(lock_violation == 0);
  return 0;
}

/*============================================================================
** Case table
**============================================================================*/

const struct task_case task_cases[] = {
  {"routing", "sum", case_routing_sum},
  {"routing", "diamond", case_routing_diamond},
  {"routing", "transparent", case_routing_transparent},
  {"routing", "chunking", case_routing_chunking},
  {"routing", "cancel", case_routing_cancel},
  {"routing", "two_levels", case_routing_two_levels},
  {"routing", "wav", case_routing_wav},
  {"tails", "stop", case_tails_stop},
  {"tails", "pause", case_tails_pause},
  {"tails", "reconnect", case_tails_reconnect},
  {"tails", "destroy", case_tails_destroy},
  {"tails", "clear_isolated", case_tails_clear_isolated},
  {"lifecycle", "reject_cycle", case_lifecycle_reject_cycle},
  {"lifecycle", "duplicate_edge", case_lifecycle_duplicate_edge},
  {"lifecycle", "invalid_params", case_lifecycle_invalid_params},
  {"lifecycle", "ownership", case_lifecycle_ownership},
  {"lifecycle", "lock_not_nested", case_lifecycle_lock_not_nested},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
