/* SoX Resampler Library      Copyright (c) 2007-18 robs@users.sourceforge.net
 * Licence for this file: LGPL v2.1                  See LICENCE for details. */

/* Streaming peak observer, see peak.h for the interface.  A frame unit holds
 * four phases: the integer sample x[k] and three windowed-sinc interpolants
 * at k + p/4.  A unit completes only once its 16 future frames exist, or when
 * the stream is explicitly finished (the missing tails being zero). */

#include "peak.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PEAK_PI 3.14159265358979323846

struct peak_entry {
  uint64_t frame;
  double magnitude;
  unsigned phase;
  unsigned channel;
};

struct soxr_peak_observer {
  unsigned num_channels;
  unsigned window;                 /* 0: observer disabled. */
  int finished;
  double norm;                     /* 1 / output full-scale gain. */

  uint64_t delivered;              /* Frames accepted since the last clear. */
  uint64_t completed;              /* Units fully observed. */

  size_t history_cap;              /* Ring capacity per channel, window + 32. */
  size_t history_len;
  size_t history_head;             /* Ring index of the oldest frame. */
  double * history;                /* Per-channel rings, cap samples each. */

  struct peak_entry * * deque;     /* One monotone ring deque per channel. */
  size_t * deque_head;
  size_t * deque_len;
  size_t * deque_cap;
};



static double peak_sinc(double t)
{
  double pi_t;
  if (t == 0.)
    return 1.;
  pi_t = PEAK_PI * t;
  return sin(pi_t) / pi_t;
}

/* The 3 x 32 normalised Hann-windowed sinc weights, shared by every unit. */
static double const * peak_weights(unsigned phase)
{
  static double weights[3][SOXR_PEAK_TAPS];
  static int initialised;
  unsigned p, j;

  if (!initialised) {
    for (p = 1; p <= 3; ++p) {
      double sum = 0.;
      for (j = 0; j < SOXR_PEAK_TAPS; ++j) {
        double t = (double)p / 4. - ((double)j - 15.);
        double w = peak_sinc(t) * (1. + cos(PEAK_PI * t / 16.)) / 2.;
        weights[p - 1][j] = w;
        sum += w;
      }
      for (j = 0; j < SOXR_PEAK_TAPS; ++j)
        weights[p - 1][j] /= sum;
    }
    initialised = 1;
  }
  return weights[phase - 1];
}



soxr_peak_observer_t * _soxr_peak_create(unsigned num_channels)
{
  soxr_peak_observer_t * p;
  if (!num_channels)
    return 0;
  p = (soxr_peak_observer_t *)calloc(1, sizeof(*p));
  if (p)
    p->num_channels = num_channels;
  return p;
}

void _soxr_peak_delete(soxr_peak_observer_t * p)
{
  unsigned c;
  if (!p)
    return;
  if (p->deque)
    for (c = 0; c < p->num_channels; ++c)
      free(p->deque[c]);
  free(p->deque);
  free(p->deque_head);
  free(p->deque_len);
  free(p->deque_cap);
  free(p->history);
  free(p);
}

unsigned _soxr_peak_get_window(soxr_peak_observer_t const * p)
{
  return p? p->window : 0;
}

uint64_t _soxr_peak_delivered(soxr_peak_observer_t const * p)
{
  return p? p->delivered : 0;
}

uint64_t _soxr_peak_completed(soxr_peak_observer_t const * p)
{
  return p? p->completed : 0;
}

/* (Re)allocate storage for the configured window and reset every counter. */
int _soxr_peak_set_storage(soxr_peak_observer_t * p, unsigned window,
    double output_gain)
{
  unsigned c;
  size_t cap = (size_t)window + 2 * SOXR_PEAK_GUARD;
  double * history;
  struct peak_entry * * deque;
  size_t * deque_head, * deque_len, * deque_cap;

  history = (double *)calloc(cap * p->num_channels, sizeof(double));
  deque = (struct peak_entry * *)calloc(p->num_channels, sizeof(*deque));
  deque_head = (size_t *)calloc(p->num_channels, sizeof(size_t));
  deque_len = (size_t *)calloc(p->num_channels, sizeof(size_t));
  deque_cap = (size_t *)calloc(p->num_channels, sizeof(size_t));
  if (!history || !deque || !deque_head || !deque_len || !deque_cap) {
    free(history); free(deque); free(deque_head);
    free(deque_len); free(deque_cap);
    return 0;
  }
  for (c = 0; c < p->num_channels; ++c) {
    deque[c] = (struct peak_entry *)calloc((size_t)window + 2, sizeof(**deque));
    if (!deque[c]) {
      unsigned u;
      for (u = 0; u < c; ++u)
        free(deque[u]);
      free(history); free(deque); free(deque_head);
      free(deque_len); free(deque_cap);
      return 0;
    }
    deque_cap[c] = (size_t)window + 2;
  }

  if (p->deque)
    for (c = 0; c < p->num_channels; ++c)
      free(p->deque[c]);
  free(p->deque);
  free(p->deque_head);
  free(p->deque_len);
  free(p->deque_cap);
  free(p->history);

  p->deque = deque;
  p->deque_head = deque_head;
  p->deque_len = deque_len;
  p->deque_cap = deque_cap;
  p->history = history;
  p->history_cap = cap;
  p->history_len = 0;
  p->history_head = 0;
  p->window = window;
  p->norm = output_gain != 0.? 1. / output_gain : 1.;
  p->finished = 0;
  p->delivered = 0;
  p->completed = 0;
  return 1;
}

void _soxr_peak_clear(soxr_peak_observer_t * p)
{
  unsigned c;
  if (!p || !p->window)
    return;
  if (p->deque)
    for (c = 0; c < p->num_channels; ++c) {
      p->deque_len[c] = 0;
      p->deque_head[c] = 0;
    }
  p->history_len = 0;
  p->history_head = 0;
  p->finished = 0;
  p->delivered = 0;
  p->completed = 0;
  memset(p->history, 0,
      p->history_cap * p->num_channels * sizeof(double));
}

/* Sample x[frame] of a channel in the history ring, zero outside the known
 * stream span (both the leading and the trailing silence). */
static double peak_sample(soxr_peak_observer_t const * p,
    unsigned channel, int64_t frame)
{
  size_t offset, idx;
  if (frame < 0 || (uint64_t)frame >= p->delivered)
    return 0.;
  offset = (uint64_t)frame - (p->delivered - p->history_len);
  if (offset >= p->history_len)
    return 0.;
  idx = (p->history_head + offset) % p->history_cap;
  return p->history[(size_t)channel * p->history_cap + idx];
}

/* Append one completed unit to a channel's monotone ring deque.  Equal
 * magnitudes keep the earlier position, so the tail is popped only when
 * strictly smaller; window eviction happens separately in peak_evict. */
static void peak_push(soxr_peak_observer_t * p, unsigned channel,
    uint64_t frame, double magnitude, unsigned phase)
{
  struct peak_entry * deque = p->deque[channel];
  size_t * len = &p->deque_len[channel];
  size_t cap = p->deque_cap[channel];

  while (*len && deque[(p->deque_head[channel] + *len - 1) % cap].magnitude
      < magnitude)
    --*len;
  if (*len < cap) {
    struct peak_entry * e = &deque[(p->deque_head[channel] + *len) % cap];
    e->frame = frame;
    e->magnitude = magnitude;
    e->phase = phase;
    e->channel = channel;
    ++*len;
  }
}

/* Drop entries older than the current window floor. */
static void peak_evict(soxr_peak_observer_t * p)
{
  unsigned c;
  uint64_t floor = p->completed > p->window?
      p->completed - p->window : 0;
  for (c = 0; c < p->num_channels; ++c) {
    struct peak_entry * deque = p->deque[c];
    size_t * len = &p->deque_len[c];
    while (*len && deque[p->deque_head[c]].frame < floor) {
      --*len;
      p->deque_head[c] = (p->deque_head[c] + 1) % p->deque_cap[c];
    }
  }
}

/* Observe the four phases of unit k for one channel and record the result. */
static void peak_observe_unit(soxr_peak_observer_t * p,
    unsigned channel, uint64_t k)
{
  double samples[SOXR_PEAK_TAPS];
  double best;
  unsigned best_phase = 0;
  unsigned phase;
  unsigned j;

  best = fabs(peak_sample(p, channel, (int64_t)k));
  for (phase = 1; phase <= 3; ++phase) {
    double const * weights = peak_weights(phase);
    double value = 0.;
    for (j = 0; j < SOXR_PEAK_TAPS; ++j)
      samples[j] = peak_sample(p, channel, (int64_t)k + (int64_t)j - 15);
    for (j = 0; j < SOXR_PEAK_TAPS; ++j)
      value += weights[j] * samples[j];
    if (fabs(value) > best) {
      best = fabs(value);
      best_phase = phase;
    }
  }
  peak_push(p, channel, k, best, best_phase);
}

static void peak_advance(soxr_peak_observer_t * p, int finish)
{
  /* Without an explicit end, unit k needs frames through k + 16 present. */
  while (!finish? p->delivered - p->completed >= SOXR_PEAK_GUARD + 1
                : p->completed < p->delivered) {
    unsigned c;
    uint64_t k = p->completed;
    for (c = 0; c < p->num_channels; ++c)
      peak_observe_unit(p, c, k);
    ++p->completed;
    peak_evict(p);
  }
}

static void peak_store_channel(soxr_peak_observer_t * p, unsigned channel,
    int is_double, void const * src, size_t n)
{
  size_t i;
  double * ring = p->history + (size_t)channel * p->history_cap;
  for (i = 0; i < n; ++i) {
    size_t idx = (p->history_head + p->history_len + i) % p->history_cap;
    ring[idx] = (is_double?
        ((double const *)src)[i] : (double)((float const *)src)[i]) * p->norm;
  }
}

void _soxr_peak_store_channel_f(soxr_peak_observer_t * p, unsigned channel,
    float const * src, size_t n)
{
  if (p && p->window && !p->finished)
    peak_store_channel(p, channel, 0, (void const *)src, n);
}

void _soxr_peak_store_channel_d(soxr_peak_observer_t * p, unsigned channel,
    double const * src, size_t n)
{
  if (p && p->window && !p->finished)
    peak_store_channel(p, channel, 1, (void const *)src, n);
}

void _soxr_peak_commit(soxr_peak_observer_t * p, size_t n)
{
  size_t overflow;
  if (!p || !p->window || p->finished || !n)
    return;
  overflow = p->history_len + n > p->history_cap?
      p->history_len + n - p->history_cap : 0;
  p->history_head = (p->history_head + overflow) % p->history_cap;
  p->history_len = p->history_len + n - overflow;
  p->delivered += n;
  peak_advance(p, 0);
}

void _soxr_peak_finish(soxr_peak_observer_t * p)
{
  if (!p || !p->window || p->finished)
    return;
  p->finished = 1;
  peak_advance(p, 1);
}

int _soxr_peak_query(soxr_peak_observer_t const * p, unsigned channel,
    double * magnitude, unsigned * phase, uint64_t * frame,
    unsigned * winner_channel)
{
  struct peak_entry const * best = 0;
  unsigned c, first, last;
  if (magnitude) *magnitude = 0.;
  if (phase) *phase = 0;
  if (frame) *frame = 0;
  if (winner_channel) *winner_channel = 0;
  if (!p || !p->window || !p->completed || channel > p->num_channels)
    return 0;
  first = channel < p->num_channels? channel : 0;
  last = channel < p->num_channels? channel : p->num_channels - 1;
  for (c = first; c <= last; ++c) {
    struct peak_entry const * e;
    if (!p->deque_len[c])
      continue;
    e = &p->deque[c][p->deque_head[c]]; /* Window maximum at the ring front. */
    if (!best || e->magnitude > best->magnitude ||
        (e->magnitude == best->magnitude &&
            (e->frame < best->frame ||
                (e->frame == best->frame &&
                    (e->phase < best->phase ||
                        (e->phase == best->phase &&
                            e->channel < best->channel))))))
      best = e;
  }
  if (!best)
    return 0;
  if (magnitude) *magnitude = best->magnitude;
  if (phase) *phase = best->phase;
  if (frame) *frame = best->frame;
  if (winner_channel) *winner_channel = best->channel;
  return 1;
}
