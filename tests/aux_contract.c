#include "task_support/check.h"
#include "src/cmixer.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_RATE 48000
#define MAX_FRAMES 700

struct seq {
  int pos;
  int rewinds;
  int reads;
  int destroys;
  int loop_length;
};

static int seq_left(int frame) {
  return ((frame * 73 + 17) % 503) - 251;
}

static int seq_right(int frame) {
  return -(((frame * 41 + 29) % 397) - 198);
}

static void seq_handler(cm_Event *e) {
  struct seq *s = (struct seq *) e->udata;
  int i;

  if (e->type == CM_EVENT_REWIND) {
    s->pos = 0;
    s->rewinds++;
  } else if (e->type == CM_EVENT_SAMPLES) {
    for (i = 0; i < e->length; i += 2) {
      int frame = s->pos++;
      if (s->loop_length > 0) {
        frame %= s->loop_length;
      }
      e->buffer[i] = (cm_Int16) seq_left(frame);
      e->buffer[i + 1] = (cm_Int16) seq_right(frame);
    }
    s->reads++;
  } else if (e->type == CM_EVENT_DESTROY) {
    s->destroys++;
  }
}

static cm_Source* make_seq_source(struct seq *state, int samplerate,
                                  int length) {
  cm_SourceInfo info;
  memset(&info, 0, sizeof(info));
  info.handler = seq_handler;
  info.udata = state;
  info.samplerate = samplerate;
  info.length = length;
  return cm_new_source(&info);
}

static void expected_rate_one(int frames, double *left, double *right) {
  int i;
  for (i = 0; i < frames; i++) {
    left[i] = seq_left(i);
    right[i] = seq_right(i);
  }
}

static int fx_lerp_reference(int a, int b, int p) {
  return a + (((b - a) * p) >> 12);
}

/* Native rate 24000 Hz at a 48000 Hz master rate gives fixed-point rate 0.5. */
static void expected_half_speed(int frames, double *left, double *right) {
  int k;
  for (k = 0; k < frames; k++) {
    int position = k * 2048;
    int frame = position >> 12;
    int p = position & 4095;
    left[k] = fx_lerp_reference(seq_left(frame), seq_left(frame + 1), p);
    right[k] = fx_lerp_reference(seq_right(frame), seq_right(frame + 1), p);
  }
}

/* Independent direct-form ring reference for y[n] = x[n-D] + f*y[n-D]. */
static void reference_delay(const double *xl, const double *xr, int frames,
                            int delay, double feedback,
                            double *yl, double *yr) {
  double *hxl = calloc((size_t) delay, sizeof(*hxl));
  double *hxr = calloc((size_t) delay, sizeof(*hxr));
  double *hyl = calloc((size_t) delay, sizeof(*hyl));
  double *hyr = calloc((size_t) delay, sizeof(*hyr));
  int n;

  if (!hxl || !hxr || !hyl || !hyr) {
    free(hxl); free(hxr); free(hyl); free(hyr);
    return;
  }

  for (n = 0; n < frames; n++) {
    int slot = n % delay;
    double xdl = n >= delay ? hxl[slot] : 0.0;
    double xdr = n >= delay ? hxr[slot] : 0.0;
    double ydl = n >= delay ? hyl[slot] : 0.0;
    double ydr = n >= delay ? hyr[slot] : 0.0;
    yl[n] = xdl + feedback * ydl;
    yr[n] = xdr + feedback * ydr;
    hxl[slot] = xl[n];
    hxr[slot] = xr[n];
    hyl[slot] = yl[n];
    hyr[slot] = yr[n];
  }

  free(hxl); free(hxr); free(hyl); free(hyr);
}

static int sample_matches(cm_Int16 actual, double expected) {
  return fabs((double) actual - expected) <= 1.000001;
}

static void put_u16(unsigned char *p, unsigned value) {
  p[0] = (unsigned char) value;
  p[1] = (unsigned char) (value >> 8);
}

static void put_u32(unsigned char *p, uint32_t value) {
  p[0] = (unsigned char) value;
  p[1] = (unsigned char) (value >> 8);
  p[2] = (unsigned char) (value >> 16);
  p[3] = (unsigned char) (value >> 24);
}

static unsigned char* make_stereo_wav(int frames, size_t *size_out) {
  int i;
  size_t size = 44 + (size_t) frames * 4;
  unsigned char *data = calloc(size, 1);
  if (!data) {
    return NULL;
  }

  memcpy(data, "RIFF", 4);
  put_u32(data + 4, (uint32_t) (36 + frames * 4));
  memcpy(data + 8, "WAVE", 4);
  memcpy(data + 12, "fmt ", 4);
  put_u32(data + 16, 16);
  put_u16(data + 20, 1);             /* PCM */
  put_u16(data + 22, 2);             /* stereo */
  put_u32(data + 24, TEST_RATE);
  put_u32(data + 28, TEST_RATE * 4);
  put_u16(data + 32, 4);
  put_u16(data + 34, 16);
  memcpy(data + 36, "data", 4);
  put_u32(data + 40, (uint32_t) (frames * 4));

  for (i = 0; i < frames; i++) {
    int16_t l = (int16_t) (900 + 37 * i);
    int16_t r = (int16_t) (-1200 - 53 * i);
    put_u16(data + 44 + i * 4, (uint16_t) l);
    put_u16(data + 46 + i * 4, (uint16_t) r);
  }

  *size_out = size;
  return data;
}

/*============================================================================
** routing
**============================================================================*/

static int case_routing_diamond_reference(void) {
  enum { N = 320 };
  struct seq state = {0};
  cm_Int16 out[N * 2];
  double xl[N], xr[N], wetl[N], wetr[N];
  int n;

  cm_init(TEST_RATE);

  cm_Source *src = make_seq_source(&state, TEST_RATE, 100000);
  cm_Bus *master = cm_master_bus();
  cm_Bus *mix = cm_new_bus();
  cm_Bus *plus = cm_new_bus();
  cm_Bus *minus = cm_new_bus();
  cm_Bus *delay = cm_new_delay_bus(7, 0.25);

  TASK_CHECK(src != NULL);
  TASK_CHECK(mix && plus && minus && delay);
  TASK_CHECK(cm_set_bus_gain(mix, 2.0) == CM_OK);
  TASK_CHECK(cm_set_source_bus(src, mix) == CM_OK);

  /* mix gain 2, dry send .5 => exactly one copy of source. */
  TASK_CHECK(cm_connect_bus(mix, master, 0.5) == CM_OK);
  /* Wet send .5 cancels the mix gain for the delay input. */
  TASK_CHECK(cm_connect_bus(mix, delay, 0.5) == CM_OK);
  TASK_CHECK(cm_connect_bus(delay, master, 0.25) == CM_OK);

  /* Large asymmetric branches sum to only 2*x.  Symmetric clipping would
  ** still cancel, but clipping either of these unequal branches would not. */
  TASK_CHECK(cm_connect_bus(mix, plus, 128.0) == CM_OK);
  TASK_CHECK(cm_connect_bus(mix, minus, -127.0) == CM_OK);
  TASK_CHECK(cm_connect_bus(plus, master, 1.0) == CM_OK);
  TASK_CHECK(cm_connect_bus(minus, master, 1.0) == CM_OK);

  cm_play(src);
  cm_process(out, N * 2);

  expected_rate_one(N, xl, xr);
  reference_delay(xl, xr, N, 7, 0.25, wetl, wetr);

  /* mix gain 2, dry .5 gives x; large branches give 256x - 254x = 2x. */
  TASK_CHECK(out[0] == 3 * seq_left(0));
  TASK_CHECK(out[1] == 3 * seq_right(0));
  TASK_CHECK(out[0] != out[1]);
  TASK_CHECK(state.rewinds == 1);
  TASK_CHECK(state.reads >= 2);

  for (n = 0; n < N; n++) {
    double expected_l = 3.0 * xl[n] + 0.25 * wetl[n];
    double expected_r = 3.0 * xr[n] + 0.25 * wetr[n];
    TASK_CHECK(sample_matches(out[n * 2], expected_l));
    TASK_CHECK(sample_matches(out[n * 2 + 1], expected_r));
  }

  cm_destroy_source(src);
  TASK_CHECK(cm_disconnect_bus(mix, master) == CM_OK);
  TASK_CHECK(cm_disconnect_bus(mix, delay) == CM_OK);
  TASK_CHECK(cm_disconnect_bus(delay, master) == CM_OK);
  TASK_CHECK(cm_disconnect_bus(mix, plus) == CM_OK);
  TASK_CHECK(cm_disconnect_bus(mix, minus) == CM_OK);
  TASK_CHECK(cm_disconnect_bus(plus, master) == CM_OK);
  TASK_CHECK(cm_disconnect_bus(minus, master) == CM_OK);
  TASK_CHECK(cm_destroy_bus(plus) == CM_OK);
  TASK_CHECK(cm_destroy_bus(minus) == CM_OK);
  TASK_CHECK(cm_destroy_bus(delay) == CM_OK);
  TASK_CHECK(cm_destroy_bus(mix) == CM_OK);
  return 0;
}

struct half_run {
  int rewinds;
  int reads;
  int state;
  double position;
};

static struct half_run run_half_speed_scenario(int chunked, cm_Int16 *out) {
  enum { N = 820 };
  static const int chunks[] = {1, 7, 64, 129, 211, 208, 200};
  struct seq state = {0};
  struct half_run info;
  cm_Source *src;
  cm_Bus *mix, *delay, *master;
  unsigned i;
  int offset = 0;

  cm_init(TEST_RATE);
  src = make_seq_source(&state, 24000, 100000);
  master = cm_master_bus();
  mix = cm_new_bus();
  delay = cm_new_delay_bus(11, 0.5);

  cm_set_source_bus(src, mix);
  cm_connect_bus(mix, master, 1.0);
  cm_connect_bus(mix, delay, 1.0);
  cm_connect_bus(delay, master, 0.5);
  cm_play(src);

  if (!chunked) {
    cm_process(out, N * 2);
  } else {
    for (i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
      cm_process(out + offset * 2, chunks[i] * 2);
      offset += chunks[i];
    }
  }

  info.rewinds = state.rewinds;
  info.reads = state.reads;
  info.state = cm_get_state(src);
  info.position = cm_get_position(src);

  cm_destroy_source(src);
  cm_disconnect_bus(mix, master);
  cm_disconnect_bus(mix, delay);
  cm_disconnect_bus(delay, master);
  cm_destroy_bus(delay);
  cm_destroy_bus(mix);
  return info;
}

static int case_routing_half_speed_and_chunks(void) {
  enum { N = 820 };
  cm_Int16 whole[N * 2];
  cm_Int16 pieces[N * 2];
  double xl[N], xr[N], wetl[N], wetr[N];
  struct half_run a, b;
  int i, n;

  a = run_half_speed_scenario(0, whole);
  b = run_half_speed_scenario(1, pieces);

  TASK_CHECK(a.state == CM_STATE_PLAYING);
  TASK_CHECK(b.state == CM_STATE_PLAYING);
  TASK_CHECK(a.rewinds == 1 && b.rewinds == 1);
  TASK_CHECK(a.reads >= 4 && b.reads >= 4);
  TASK_CHECK(a.reads == b.reads);
  TASK_CHECK(fabs(a.position - N / (double) TEST_RATE) < 1e-9);
  TASK_CHECK(fabs(b.position - N / (double) TEST_RATE) < 1e-9);

  for (i = 0; i < N * 2; i++) {
    TASK_CHECK(whole[i] == pieces[i]);
  }

  expected_half_speed(N, xl, xr);
  reference_delay(xl, xr, N, 11, 0.5, wetl, wetr);
  for (n = 0; n < N; n++) {
    TASK_CHECK(sample_matches(whole[n * 2], xl[n] + 0.5 * wetl[n]));
    TASK_CHECK(sample_matches(whole[n * 2 + 1],
                              xr[n] + 0.5 * wetr[n]));
  }
  return 0;
}

static int case_routing_pitch_control(void) {
  enum { N = 300 };
  struct seq state = {0};
  cm_Int16 out[N * 2];
  double xl[N], xr[N];
  cm_Source *src;
  cm_Bus *bus, *master;
  int n;

  cm_init(TEST_RATE);
  src = make_seq_source(&state, TEST_RATE, 100000);
  bus = cm_new_bus();
  master = cm_master_bus();
  TASK_CHECK(src && bus);
  cm_set_source_bus(src, bus);
  cm_connect_bus(bus, master, 1.0);
  cm_set_pitch(src, 0.5);

  cm_play(src);
  cm_process(out, N * 2);
  TASK_CHECK(fabs(cm_get_position(src) -
                  (N * 0.5) / (double) TEST_RATE) < 1e-9);

  expected_half_speed(N, xl, xr);
  for (n = 0; n < N; n++) {
    TASK_CHECK(out[n * 2] == (cm_Int16) xl[n]);
    TASK_CHECK(out[n * 2 + 1] == (cm_Int16) xr[n]);
  }

  cm_destroy_source(src);
  cm_disconnect_bus(bus, master);
  cm_destroy_bus(bus);
  return 0;
}

static int case_routing_two_delay_stages(void) {
  enum { N = 160 };
  struct seq state = {0};
  cm_Int16 out[N * 2];
  double xl[N], xr[N], w1l[N], w1r[N], w2l[N], w2r[N];
  cm_Source *src;
  cm_Bus *d1, *d2, *master;
  int n;

  cm_init(TEST_RATE);
  src = make_seq_source(&state, TEST_RATE, 100000);
  master = cm_master_bus();
  d1 = cm_new_delay_bus(3, 0.25);
  d2 = cm_new_delay_bus(5, 0.5);
  TASK_CHECK(src && d1 && d2);
  TASK_CHECK(cm_set_bus_gain(d1, 0.5) == CM_OK);
  TASK_CHECK(cm_set_source_bus(src, d1) == CM_OK);
  /* Bus gain .5 and edge gain 2 cancel downstream, while the delay's raw
  ** feedback state must remain unaffected by either gain. */
  TASK_CHECK(cm_connect_bus(d1, d2, 2.0) == CM_OK);
  TASK_CHECK(cm_connect_bus(d2, master, 1.0) == CM_OK);

  cm_play(src);
  cm_process(out, N * 2);

  expected_rate_one(N, xl, xr);
  reference_delay(xl, xr, N, 3, 0.25, w1l, w1r);
  reference_delay(w1l, w1r, N, 5, 0.5, w2l, w2r);

  for (n = 0; n < N; n++) {
    TASK_CHECK(sample_matches(out[n * 2], w2l[n]));
    TASK_CHECK(sample_matches(out[n * 2 + 1], w2r[n]));
  }

  cm_destroy_source(src);
  cm_disconnect_bus(d1, d2);
  cm_disconnect_bus(d2, master);
  cm_destroy_bus(d2);
  cm_destroy_bus(d1);
  return 0;
}

static int case_routing_generated_wav(void) {
  enum { FRAMES = 12, N = 20 };
  size_t wav_size = 0;
  unsigned char *wav = make_stereo_wav(FRAMES, &wav_size);
  cm_Int16 out[N * 2];
  cm_Source *src;
  cm_Bus *bus, *master;
  int n;

  TASK_CHECK(wav != NULL);
  cm_init(TEST_RATE);
  src = cm_new_source_from_mem(wav, (int) wav_size);
  TASK_CHECK(src != NULL);
  bus = cm_new_bus();
  master = cm_master_bus();
  TASK_CHECK(bus != NULL);
  TASK_CHECK(cm_set_source_bus(src, bus) == CM_OK);
  TASK_CHECK(cm_connect_bus(bus, master, 1.0) == CM_OK);

  cm_play(src);
  cm_process(out, N * 2);
  TASK_CHECK(cm_get_state(src) == CM_STATE_STOPPED);

  for (n = 0; n < N; n++) {
    int expected_l = n < FRAMES ? 900 + 37 * n : 0;
    int expected_r = n < FRAMES ? -1200 - 53 * n : 0;
    TASK_CHECK(out[n * 2] == expected_l);
    TASK_CHECK(out[n * 2 + 1] == expected_r);
  }

  cm_destroy_source(src);
  cm_disconnect_bus(bus, master);
  cm_destroy_bus(bus);
  free(wav);
  return 0;
}

/*============================================================================
** tails
**============================================================================*/

static int case_routing_loop_retained(void) {
  enum { LOOP = 17, N = 73 };
  struct seq state = {0};
  cm_Int16 out[N * 2];
  cm_Source *src;
  cm_Bus *bus, *master;
  int n;

  state.loop_length = LOOP;
  cm_init(TEST_RATE);
  src = make_seq_source(&state, TEST_RATE, LOOP);
  bus = cm_new_bus();
  master = cm_master_bus();
  TASK_CHECK(src && bus);
  TASK_CHECK(cm_set_source_bus(src, bus) == CM_OK);
  TASK_CHECK(cm_connect_bus(bus, master, 1.0) == CM_OK);
  cm_set_loop(src, 1);

  cm_play(src);
  cm_process(out, N * 2);
  TASK_CHECK(cm_get_state(src) == CM_STATE_PLAYING);

  for (n = 0; n < N; n++) {
    int frame = n % LOOP;
    TASK_CHECK(out[n * 2] == seq_left(frame));
    TASK_CHECK(out[n * 2 + 1] == seq_right(frame));
  }

  cm_destroy_source(src);
  cm_disconnect_bus(bus, master);
  cm_destroy_bus(bus);
  return 0;
}

/*============================================================================
** tails
**============================================================================*/

static int case_routing_gain_and_pan_preserved(void) {
  enum { A = 11, B = 13, N = A + B };
  struct seq state = {0};
  cm_Int16 out[N * 2];
  cm_Source *src;
  cm_Bus *bus, *master;
  int n;

  cm_init(TEST_RATE);
  src = make_seq_source(&state, TEST_RATE, 100000);
  bus = cm_new_bus();
  master = cm_master_bus();
  TASK_CHECK(src && bus);
  cm_set_source_bus(src, bus);
  cm_connect_bus(bus, master, 1.0);

  cm_set_gain(src, 0.5);
  cm_set_pan(src, -1.0);
  cm_play(src);
  cm_process(out, A * 2);

  cm_set_gain(src, 1.0);
  cm_set_pan(src, 1.0);
  cm_process(out + A * 2, B * 2);

  for (n = 0; n < A; n++) {
    TASK_CHECK(out[n * 2] == (cm_Int16) floor(0.5 * seq_left(n)));
    TASK_CHECK(out[n * 2 + 1] == 0);
  }
  for (n = 0; n < B; n++) {
    int frame = A + n;
    TASK_CHECK(out[(A + n) * 2] == 0);
    TASK_CHECK(out[(A + n) * 2 + 1] == seq_right(frame));
  }

  cm_destroy_source(src);
  cm_disconnect_bus(bus, master);
  cm_destroy_bus(bus);
  return 0;
}

static int case_routing_master_gain(void) {
  enum { N = 24 };
  struct seq state = {0};
  cm_Int16 out[N * 2];
  cm_Source *src;
  cm_Bus *bus, *master;
  int n;

  cm_init(TEST_RATE);
  src = make_seq_source(&state, TEST_RATE, 100000);
  bus = cm_new_bus();
  master = cm_master_bus();
  TASK_CHECK(src && bus);
  cm_set_source_bus(src, bus);
  cm_connect_bus(bus, master, 1.0);
  cm_set_master_gain(0.5);

  cm_play(src);
  cm_process(out, N * 2);
  for (n = 0; n < N; n++) {
    TASK_CHECK(out[n * 2] == (cm_Int16) floor(0.5 * seq_left(n)));
    TASK_CHECK(out[n * 2 + 1] == (cm_Int16) floor(0.5 * seq_right(n)));
  }

  cm_destroy_source(src);
  cm_disconnect_bus(bus, master);
  cm_destroy_bus(bus);
  return 0;
}

/*============================================================================
** tails
**============================================================================*/

static int case_tails_pause_keeps_decay(void) {
  enum { A = 9, TAIL = 40, N = A + TAIL };
  struct seq state = {0};
  cm_Int16 out[N * 2];
  double xl[N], xr[N], yl[N], yr[N];
  cm_Source *src;
  cm_Bus *delay, *master;
  double position;
  int n;

  cm_init(TEST_RATE);
  src = make_seq_source(&state, TEST_RATE, 100000);
  delay = cm_new_delay_bus(5, 0.5);
  master = cm_master_bus();
  TASK_CHECK(src && delay);
  cm_set_source_bus(src, delay);
  cm_connect_bus(delay, master, 1.0);

  cm_play(src);
  cm_process(out, A * 2);
  cm_pause(src);
  position = cm_get_position(src);
  TASK_CHECK(fabs(position - A / (double) TEST_RATE) < 1e-9);

  cm_process(out + A * 2, TAIL * 2);
  TASK_CHECK(fabs(cm_get_position(src) - position) < 1e-12);

  for (n = 0; n < N; n++) {
    xl[n] = n < A ? seq_left(n) : 0.0;
    xr[n] = n < A ? seq_right(n) : 0.0;
  }
  reference_delay(xl, xr, N, 5, 0.5, yl, yr);
  for (n = 0; n < N; n++) {
    TASK_CHECK(sample_matches(out[n * 2], yl[n]));
    TASK_CHECK(sample_matches(out[n * 2 + 1], yr[n]));
  }

  cm_destroy_source(src);
  cm_disconnect_bus(delay, master);
  cm_destroy_bus(delay);
  return 0;
}

static int case_tails_stop_and_destroy(void) {
  enum { A = 7, B = 12, C = 20, N = A + B + C };
  struct seq state = {0};
  cm_Int16 out[N * 2];
  double xl[N], xr[N], yl[N], yr[N];
  cm_Source *src;
  cm_Bus *delay, *master;
  int n;

  cm_init(TEST_RATE);
  src = make_seq_source(&state, TEST_RATE, 100000);
  delay = cm_new_delay_bus(6, 0.375);
  master = cm_master_bus();
  TASK_CHECK(src && delay);
  cm_set_source_bus(src, delay);
  cm_connect_bus(delay, master, 1.0);

  cm_play(src);
  cm_process(out, A * 2);
  cm_stop(src);
  cm_process(out + A * 2, B * 2);
  TASK_CHECK(cm_get_state(src) == CM_STATE_STOPPED);
  TASK_CHECK(state.rewinds >= 1);

  cm_destroy_source(src);
  cm_process(out + (A + B) * 2, C * 2);

  for (n = 0; n < N; n++) {
    xl[n] = n < A ? seq_left(n) : 0.0;
    xr[n] = n < A ? seq_right(n) : 0.0;
  }
  reference_delay(xl, xr, N, 6, 0.375, yl, yr);
  for (n = 0; n < N; n++) {
    TASK_CHECK(sample_matches(out[n * 2], yl[n]));
    TASK_CHECK(sample_matches(out[n * 2 + 1], yr[n]));
  }

  cm_disconnect_bus(delay, master);
  cm_destroy_bus(delay);
  return 0;
}

static int case_tails_natural_end(void) {
  enum { A = 13, TAIL = 32, N = A + TAIL };
  struct seq state = {0};
  cm_Int16 out[N * 2];
  double xl[N], xr[N], yl[N], yr[N];
  cm_Source *src;
  cm_Bus *delay, *master;
  int n;

  cm_init(TEST_RATE);
  src = make_seq_source(&state, TEST_RATE, A);
  delay = cm_new_delay_bus(4, 0.25);
  master = cm_master_bus();
  TASK_CHECK(src && delay);
  cm_set_source_bus(src, delay);
  cm_connect_bus(delay, master, 1.0);

  cm_play(src);
  cm_process(out, N * 2);
  TASK_CHECK(cm_get_state(src) == CM_STATE_STOPPED);

  for (n = 0; n < N; n++) {
    xl[n] = n < A ? seq_left(n) : 0.0;
    xr[n] = n < A ? seq_right(n) : 0.0;
  }
  reference_delay(xl, xr, N, 4, 0.25, yl, yr);
  for (n = 0; n < N; n++) {
    TASK_CHECK(sample_matches(out[n * 2], yl[n]));
    TASK_CHECK(sample_matches(out[n * 2 + 1], yr[n]));
  }

  cm_destroy_source(src);
  cm_disconnect_bus(delay, master);
  cm_destroy_bus(delay);
  return 0;
}

static int case_tails_output_disconnect_advances(void) {
  enum { A = 6, GAP = 10, AFTER = 28, N = A + GAP + AFTER };
  struct seq state = {0};
  cm_Int16 out[N * 2];
  double xl[N], xr[N], yl[N], yr[N];
  cm_Source *src;
  cm_Bus *delay, *master;
  int n;

  cm_init(TEST_RATE);
  src = make_seq_source(&state, TEST_RATE, 100000);
  delay = cm_new_delay_bus(4, 0.5);
  master = cm_master_bus();
  TASK_CHECK(src && delay);
  cm_set_source_bus(src, delay);
  cm_connect_bus(delay, master, 1.0);

  cm_play(src);
  cm_process(out, A * 2);
  cm_pause(src);
  TASK_CHECK(cm_disconnect_bus(delay, master) == CM_OK);
  cm_process(out + A * 2, GAP * 2);

  for (n = A; n < A + GAP; n++) {
    TASK_CHECK(out[n * 2] == 0);
    TASK_CHECK(out[n * 2 + 1] == 0);
  }

  TASK_CHECK(cm_connect_bus(delay, master, 1.0) == CM_OK);
  cm_process(out + (A + GAP) * 2, AFTER * 2);

  for (n = 0; n < N; n++) {
    xl[n] = n < A ? seq_left(n) : 0.0;
    xr[n] = n < A ? seq_right(n) : 0.0;
  }
  reference_delay(xl, xr, N, 4, 0.5, yl, yr);

  for (n = 0; n < A; n++) {
    TASK_CHECK(sample_matches(out[n * 2], yl[n]));
    TASK_CHECK(sample_matches(out[n * 2 + 1], yr[n]));
  }
  for (n = A + GAP; n < N; n++) {
    TASK_CHECK(sample_matches(out[n * 2], yl[n]));
    TASK_CHECK(sample_matches(out[n * 2 + 1], yr[n]));
  }

  cm_destroy_source(src);
  cm_disconnect_bus(delay, master);
  cm_destroy_bus(delay);
  return 0;
}

static int case_tails_input_disconnect_keeps_history(void) {
  enum { A = 8, TAIL = 30, N = A + TAIL };
  struct seq state = {0};
  cm_Int16 out[N * 2];
  double xl[N], xr[N], yl[N], yr[N];
  cm_Source *src;
  cm_Bus *mix, *delay, *master;
  int n;

  cm_init(TEST_RATE);
  src = make_seq_source(&state, TEST_RATE, 100000);
  mix = cm_new_bus();
  delay = cm_new_delay_bus(5, 0.5);
  master = cm_master_bus();
  TASK_CHECK(src && mix && delay);
  cm_set_source_bus(src, mix);
  cm_connect_bus(mix, delay, 1.0);
  cm_connect_bus(delay, master, 1.0);

  cm_play(src);
  cm_process(out, A * 2);
  TASK_CHECK(cm_disconnect_bus(mix, delay) == CM_OK);
  cm_process(out + A * 2, TAIL * 2);

  for (n = 0; n < N; n++) {
    xl[n] = n < A ? seq_left(n) : 0.0;
    xr[n] = n < A ? seq_right(n) : 0.0;
  }
  reference_delay(xl, xr, N, 5, 0.5, yl, yr);
  for (n = 0; n < N; n++) {
    TASK_CHECK(sample_matches(out[n * 2], yl[n]));
    TASK_CHECK(sample_matches(out[n * 2 + 1], yr[n]));
  }

  cm_destroy_source(src);
  cm_disconnect_bus(delay, master);
  cm_destroy_bus(delay);
  cm_destroy_bus(mix);
  return 0;
}

static int case_tails_clear_isolation(void) {
  enum { C = 10, N = 52, M = N - C };
  struct seq state = {0};
  cm_Int16 out[N * 2];
  double xl[N], xr[N];
  double y2l[N], y2r[N];
  double post_l[M], post_r[M], cleared_l[M], cleared_r[M];
  cm_Source *src;
  cm_Bus *split, *d1, *d2, *master;
  double position_before, position_after;
  int n, m;

  cm_init(TEST_RATE);
  src = make_seq_source(&state, TEST_RATE, 100000);
  split = cm_new_bus();
  d1 = cm_new_delay_bus(4, 0.5);
  d2 = cm_new_delay_bus(9, 0.25);
  master = cm_master_bus();
  TASK_CHECK(src && split && d1 && d2);
  cm_set_source_bus(src, split);
  cm_connect_bus(split, d1, 1.0);
  cm_connect_bus(split, d2, 1.0);
  cm_connect_bus(d1, master, 1.0);
  cm_connect_bus(d2, master, 1.0);

  cm_play(src);
  cm_process(out, C * 2);
  position_before = cm_get_position(src);
  TASK_CHECK(cm_clear_delay_history(d1) == CM_OK);
  TASK_CHECK(cm_clear_delay_history(cm_master_bus()) == CM_ERROR);
  position_after = cm_get_position(src);
  TASK_CHECK(position_before == position_after);
  cm_process(out + C * 2, (N - C) * 2);

  expected_rate_one(N, xl, xr);
  reference_delay(xl, xr, N, 9, 0.25, y2l, y2r);
  for (m = 0; m < M; m++) {
    post_l[m] = xl[C + m];
    post_r[m] = xr[C + m];
  }
  reference_delay(post_l, post_r, M, 4, 0.5, cleared_l, cleared_r);

  for (n = 0; n < N; n++) {
    double d1l = 0.0, d1r = 0.0;
    if (n < C) {
      /* Before the clear, use a full-history reference. */
      double full1l[MAX_FRAMES], full1r[MAX_FRAMES];
      /* This branch is only reached in the first C frames; recomputing in
      ** the loop is wasteful but keeps the independent formula local. */
      reference_delay(xl, xr, C, 4, 0.5, full1l, full1r);
      d1l = full1l[n];
      d1r = full1r[n];
    } else {
      d1l = cleared_l[n - C];
      d1r = cleared_r[n - C];
    }
    TASK_CHECK(sample_matches(out[n * 2], d1l + y2l[n]));
    TASK_CHECK(sample_matches(out[n * 2 + 1], d1r + y2r[n]));
  }

  cm_destroy_source(src);
  cm_disconnect_bus(split, d1);
  cm_disconnect_bus(split, d2);
  cm_disconnect_bus(d1, master);
  cm_disconnect_bus(d2, master);
  cm_destroy_bus(d1);
  cm_destroy_bus(d2);
  cm_destroy_bus(split);
  return 0;
}

/*============================================================================
** lifecycle
**============================================================================*/

static int case_lifecycle_invalid_parameters_atomic(void) {
  cm_Bus *a, *b, *d;
  double gain = 0.0;

  cm_init(TEST_RATE);

  TASK_CHECK(cm_new_delay_bus(0, 0.5) == NULL);
  TASK_CHECK(cm_new_delay_bus(-3, 0.5) == NULL);
  TASK_CHECK(cm_get_error() != NULL);
  TASK_CHECK(cm_new_delay_bus(4, -0.01) == NULL);
  TASK_CHECK(cm_new_delay_bus(4, 1.0) == NULL);
  TASK_CHECK(cm_new_delay_bus(4, INFINITY) == NULL);
  TASK_CHECK(cm_new_delay_bus(4, -INFINITY) == NULL);
  TASK_CHECK(cm_new_delay_bus(4, NAN) == NULL);

  a = cm_new_bus();
  b = cm_new_bus();
  d = cm_new_delay_bus(8, 0.25);
  TASK_CHECK(a && b && d);

  TASK_CHECK(cm_set_bus_gain(a, 3.5) == CM_OK);
  TASK_CHECK(cm_set_bus_gain(a, NAN) == CM_ERROR);
  TASK_CHECK(cm_set_bus_gain(a, INFINITY) == CM_ERROR);
  TASK_CHECK(cm_get_bus_gain(a, &gain) == CM_OK);
  TASK_CHECK(gain == 3.5);

  TASK_CHECK(cm_set_delay_feedback(d, 1.0) == CM_ERROR);
  TASK_CHECK(cm_set_delay_feedback(d, -0.2) == CM_ERROR);
  TASK_CHECK(cm_set_delay_feedback(d, NAN) == CM_ERROR);
  TASK_CHECK(cm_set_delay_feedback(d, 0.75) == CM_OK);

  TASK_CHECK(cm_connect_bus(a, b, 0.5) == CM_OK);
  TASK_CHECK(cm_connect_bus(a, b, NAN) == CM_ERROR);
  TASK_CHECK(cm_connect_bus(a, b, INFINITY) == CM_ERROR);
  TASK_CHECK(cm_get_edge_gain(a, b, &gain) == CM_OK);
  TASK_CHECK(gain == 0.5);

  TASK_CHECK(cm_connect_bus(NULL, b, 1.0) == CM_ERROR);
  TASK_CHECK(cm_connect_bus(a, NULL, 1.0) == CM_ERROR);

  TASK_CHECK(cm_disconnect_bus(a, b) == CM_OK);
  TASK_CHECK(cm_destroy_bus(d) == CM_OK);
  TASK_CHECK(cm_destroy_bus(b) == CM_OK);
  TASK_CHECK(cm_destroy_bus(a) == CM_OK);
  return 0;
}

static int case_lifecycle_cycle_keeps_original_graph(void) {
  enum { N = 48 };
  struct seq state = {0};
  cm_Int16 out[N * 2];
  double xl[N], xr[N];
  cm_Source *src;
  cm_Bus *a, *b, *c, *master;
  double gain;
  int n;

  cm_init(TEST_RATE);
  src = make_seq_source(&state, TEST_RATE, 100000);
  master = cm_master_bus();
  a = cm_new_bus();
  b = cm_new_bus();
  c = cm_new_bus();
  TASK_CHECK(src && a && b && c);

  TASK_CHECK(cm_set_source_bus(src, a) == CM_OK);
  TASK_CHECK(cm_connect_bus(a, master, 1.0) == CM_OK);
  TASK_CHECK(cm_connect_bus(a, b, 0.5) == CM_OK);
  TASK_CHECK(cm_connect_bus(b, c, 2.0) == CM_OK);
  TASK_CHECK(cm_connect_bus(c, master, 0.25) == CM_OK);

  TASK_CHECK(cm_connect_bus(b, a, 99.0) == CM_ERROR);
  TASK_CHECK(cm_connect_bus(c, a, 99.0) == CM_ERROR);
  TASK_CHECK(cm_connect_bus(a, a, 99.0) == CM_ERROR);
  TASK_CHECK(cm_connect_bus(master, a, 1.0) == CM_ERROR);

  TASK_CHECK(cm_get_edge_gain(b, a, &gain) == CM_ERROR);
  TASK_CHECK(cm_get_edge_gain(c, a, &gain) == CM_ERROR);
  TASK_CHECK(cm_get_edge_gain(a, b, &gain) == CM_OK);
  TASK_CHECK(gain == 0.5);
  TASK_CHECK(cm_get_edge_gain(b, c, &gain) == CM_OK);
  TASK_CHECK(gain == 2.0);

  cm_play(src);
  cm_process(out, N * 2);
  expected_rate_one(N, xl, xr);
  for (n = 0; n < N; n++) {
    TASK_CHECK(out[n * 2] == (cm_Int16) floor(1.25 * xl[n]));
    TASK_CHECK(out[n * 2 + 1] == (cm_Int16) floor(1.25 * xr[n]));
  }

  cm_destroy_source(src);
  cm_disconnect_bus(a, master);
  cm_disconnect_bus(a, b);
  cm_disconnect_bus(b, c);
  cm_disconnect_bus(c, master);
  cm_destroy_bus(c);
  cm_destroy_bus(b);
  cm_destroy_bus(a);
  return 0;
}

static int case_lifecycle_bus_ownership_and_default_route(void) {
  struct seq state = {0};
  cm_Int16 out[8];
  cm_Source *src, *other;
  cm_Bus *master, *aux, *a, *b, *safe;
  int i;

  cm_init(TEST_RATE);
  master = cm_master_bus();
  src = make_seq_source(&state, TEST_RATE, 100000);
  TASK_CHECK(src != NULL);
  TASK_CHECK(cm_get_source_bus(src) == master);
  TASK_CHECK(cm_destroy_bus(master) == CM_ERROR);

  aux = cm_new_bus();
  TASK_CHECK(aux != NULL);
  cm_play(src);
  cm_process(out, 8);
  for (i = 0; i < 4; i++) {
    TASK_CHECK(out[i * 2] == seq_left(i));
    TASK_CHECK(out[i * 2 + 1] == seq_right(i));
  }

  TASK_CHECK(cm_set_source_bus(src, aux) == CM_OK);
  TASK_CHECK(cm_get_source_bus(src) == aux);
  TASK_CHECK(cm_destroy_bus(aux) == CM_ERROR);
  cm_process(out, 8);
  for (i = 0; i < 8; i++) {
    TASK_CHECK(out[i] == 0);
  }

  TASK_CHECK(cm_use_master_bus(src) == CM_OK);
  TASK_CHECK(cm_get_source_bus(src) == master);
  cm_process(out, 8);
  for (i = 0; i < 4; i++) {
    TASK_CHECK(out[i * 2] == seq_left(8 + i));
    TASK_CHECK(out[i * 2 + 1] == seq_right(8 + i));
  }

  cm_destroy_source(src);
  TASK_CHECK(cm_destroy_bus(aux) == CM_OK);

  a = cm_new_bus();
  b = cm_new_bus();
  TASK_CHECK(a && b);
  TASK_CHECK(cm_connect_bus(a, b, 1.0) == CM_OK);
  TASK_CHECK(cm_destroy_bus(a) == CM_ERROR);
  TASK_CHECK(cm_destroy_bus(b) == CM_ERROR);
  TASK_CHECK(cm_disconnect_bus(a, b) == CM_OK);
  TASK_CHECK(cm_destroy_bus(a) == CM_OK);
  TASK_CHECK(cm_destroy_bus(b) == CM_OK);

  safe = cm_new_bus();
  other = make_seq_source(&(struct seq){0}, TEST_RATE, 100000);
  TASK_CHECK(safe && other);
  TASK_CHECK(cm_set_source_bus(other, safe) == CM_OK);
  TASK_CHECK(cm_destroy_bus(safe) == CM_ERROR);
  cm_destroy_source(other);
  TASK_CHECK(cm_destroy_bus(safe) == CM_OK);

  /* No dangling route: rendering after all auxiliary objects are gone works. */
  cm_process(out, 8);
  for (i = 0; i < 8; i++) {
    TASK_CHECK(out[i] == 0);
  }
  return 0;
}

const struct task_case task_cases[] = {
  {"routing", "diamond_reference", case_routing_diamond_reference},
  {"routing", "half_speed_and_chunks", case_routing_half_speed_and_chunks},
  {"routing", "pitch_control", case_routing_pitch_control},
  {"routing", "two_delay_stages", case_routing_two_delay_stages},
  {"routing", "generated_wav", case_routing_generated_wav},
  {"routing", "loop_retained", case_routing_loop_retained},
  {"routing", "gain_and_pan_preserved",
   case_routing_gain_and_pan_preserved},
  {"routing", "master_gain", case_routing_master_gain},
  {"tails", "pause_keeps_decay", case_tails_pause_keeps_decay},
  {"tails", "stop_and_destroy", case_tails_stop_and_destroy},
  {"tails", "natural_end", case_tails_natural_end},
  {"tails", "output_disconnect_advances",
   case_tails_output_disconnect_advances},
  {"tails", "input_disconnect_keeps_history",
   case_tails_input_disconnect_keeps_history},
  {"tails", "clear_isolation", case_tails_clear_isolation},
  {"lifecycle", "invalid_parameters_atomic",
   case_lifecycle_invalid_parameters_atomic},
  {"lifecycle", "cycle_keeps_original_graph",
   case_lifecycle_cycle_keeps_original_graph},
  {"lifecycle", "bus_ownership_and_default_route",
   case_lifecycle_bus_ownership_and_default_route}
};

const size_t task_case_count =
  sizeof(task_cases) / sizeof(task_cases[0]);
