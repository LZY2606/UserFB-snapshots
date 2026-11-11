/*
 * Small demonstration of the spherical-triangulation interpolation path.
 *
 * The measurement set is an octahedron with a synthetic, sparse binaural FIR
 * and asymmetric per-measurement delays.  Queries walk through a sequence of
 * continuous directions, including directions on the positive and negative
 * x axis, near the poles, and across the azimuth seam.
 */

#include "mysofa.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILTER_LEN 8
#define MEASUREMENTS 6
#define PI 3.14159265358979323846

static struct MYSOFA_ATTRIBUTE type_attr = {NULL, (char *)"Type",
                                            (char *)"cartesian"};

int main(void) {
  float pos[MEASUREMENTS * 3] = {1, 0, 0, -1, 0, 0, 0, 1,
                                 0, 0, -1, 0, 0, 0, 1, 0,
                                 0, -1};
  float ir[MEASUREMENTS * 2 * FILTER_LEN];
  float delay[MEASUREMENTS * 2];
  struct MYSOFA_HRTF hrtf;
  MYSOFA_SPHERICAL *spherical;
  unsigned m, d;
  unsigned directions = 0;
  int err = 0;

  memset(&hrtf, 0, sizeof(hrtf));
  memset(ir, 0, sizeof(ir));
  for (m = 0; m < MEASUREMENTS; m++) {
    /* one non-zero tap per ear, different for every measurement */
    ir[m * 2 * FILTER_LEN + (m % FILTER_LEN)] = (float)(m + 1) * 1.5f;
    ir[m * 2 * FILTER_LEN + FILTER_LEN + ((m + 3) % FILTER_LEN)] =
        (float)(m + 1) * 0.75f;
    delay[m * 2 + 0] = (float)m * 0.31f + 0.5f;
    delay[m * 2 + 1] = (float)m * 0.43f + 0.75f;
  }

  hrtf.I = 1;
  hrtf.C = 3;
  hrtf.R = 2;
  hrtf.E = 1;
  hrtf.N = FILTER_LEN;
  hrtf.M = MEASUREMENTS;
  hrtf.SourcePosition.values = pos;
  hrtf.SourcePosition.elements = MEASUREMENTS * 3;
  hrtf.SourcePosition.attributes = &type_attr;
  hrtf.DataIR.values = ir;
  hrtf.DataIR.elements = MEASUREMENTS * 2 * FILTER_LEN;
  hrtf.DataDelay.values = delay;
  hrtf.DataDelay.elements = MEASUREMENTS * 2;

  spherical = mysofa_spherical_init(&hrtf, &err);
  if (!spherical) {
    fprintf(stderr, "mysofa_spherical_init failed: %d\n", err);
    return 1;
  }

  for (d = 0; d < 12; d++) {
    double az = -PI + 1.5 * PI * (double)(d % 4) / 3.0;
    double el = -0.55 + 1.1 * (double)(d / 4) / 2.0;
    double dx = cos(el) * cos(az);
    double dy = cos(el) * sin(az);
    double dz = sin(el);
    /* add a small radius variation to show that only the direction matters */
    double scale = 1.0 + 0.001 * (double)d;
    float dir[3];
    float left[FILTER_LEN], right[FILTER_LEN], delays[2], weights[3];
    unsigned indices[3];
    int ret;
    double len = sqrt(dx * dx + dy * dy + dz * dz);

    dir[0] = (float)(dx * scale / len);
    dir[1] = (float)(dy * scale / len);
    dir[2] = (float)(dz * scale / len);

    ret = mysofa_spherical_getfilter(spherical, dir, left, right, delays,
                                     indices, weights);
    if (ret != MYSOFA_OK) {
      fprintf(stderr, "query %u failed: %d\n", d, ret);
      mysofa_spherical_free(spherical);
      return 2;
    }
    printf("direction=(%.6f,%.6f,%.6f) indices=%u,%u,%u "
           "weights=%.6f,%.6f,%.6f left[0]=%.6f right[0]=%.6f "
           "delay=%.6f,%.6f\n",
           dir[0], dir[1], dir[2], indices[0], indices[1], indices[2],
           weights[0], weights[1], weights[2], left[0], right[0], delays[0],
           delays[1]);
    directions++;
  }

  /* exact measurement directions reproduce that measurement */
  for (m = 0; m < MEASUREMENTS; m++) {
    float dir[3];
    float left[FILTER_LEN], right[FILTER_LEN], delays[2], weights[3];
    unsigned indices[3];
    int ret;
    dir[0] = pos[m * 3 + 0];
    dir[1] = pos[m * 3 + 1];
    dir[2] = pos[m * 3 + 2];
    ret = mysofa_spherical_getfilter(spherical, dir, left, right, delays,
                                     indices, weights);
    if (ret != MYSOFA_OK) {
      fprintf(stderr, "vertex query %u failed: %d\n", m, ret);
      mysofa_spherical_free(spherical);
      return 3;
    }
    printf("direction=(%.6f,%.6f,%.6f) indices=%u,%u,%u "
           "weights=%.6f,%.6f,%.6f left[0]=%.6f right[0]=%.6f "
           "delay=%.6f,%.6f\n",
           dir[0], dir[1], dir[2], indices[0], indices[1], indices[2],
           weights[0], weights[1], weights[2], left[0], right[0], delays[0],
           delays[1]);
    directions++;
  }

  mysofa_spherical_free(spherical);
  printf("SPHERICAL_WALK_OK directions=%u\n", directions);
  return 0;
}
