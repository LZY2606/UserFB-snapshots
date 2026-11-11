/*

 Copyright 2016 Christian Hoene, Symonics GmbH

 */

#ifndef MYSOFA_H_INCLUDED
#define MYSOFA_H_INCLUDED
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "mysofa_export.h"

#define MYSOFA_DEFAULT_NEIGH_STEP_ANGLE 0.5f
#define MYSOFA_DEFAULT_NEIGH_STEP_RADIUS 0.01f

/** debugging output */
#ifdef VDEBUG
#include <stdio.h>
#define mylog(...)                                                             \
  {                                                                            \
    fprintf(stderr, "%s:%d: ", __FILE__, __LINE__);                            \
    fprintf(stderr, __VA_ARGS__);                                              \
  }
#else
#define mylog(...)
#endif

/** attributes */
struct MYSOFA_ATTRIBUTE {
  struct MYSOFA_ATTRIBUTE *next;
  char *name;
  char *value;
};

struct MYSOFA_ARRAY {
  float *values;
  unsigned int elements;
  struct MYSOFA_ATTRIBUTE *attributes;
};

/** additional variable */
struct MYSOFA_VARIABLE {
  struct MYSOFA_VARIABLE *next;
  char *name;
  struct MYSOFA_ARRAY *value;
};

/*
 * The HRTF structure data types
 */
struct MYSOFA_HRTF {

  /* Dimensions defined in AES69
 M Number of measurements; must be integer greater than zero.
 R Number of receivers; must be integer greater than zero.
 E Number of emitters; must be integer greater than zero.
 N Number of data samples describing one measurement; must be integer greater
 than zero. S Number of characters in a string; must be integer greater than
 zero. I 1 Singleton dimension, defines a scalar value. C 3 Coordinate
 triplet, always three; the coordinate type defines the meaning of this
 dimension.
 */
  unsigned I, C, R, E, N, M;

  struct MYSOFA_ARRAY ListenerPosition;

  struct MYSOFA_ARRAY ReceiverPosition;

  struct MYSOFA_ARRAY SourcePosition;

  struct MYSOFA_ARRAY EmitterPosition;

  struct MYSOFA_ARRAY ListenerUp;

  struct MYSOFA_ARRAY ListenerView;

  /** array of filter coefficients. Sizes are filters*filter_length. */
  struct MYSOFA_ARRAY DataIR;

  /** the sampling rate used in this structure */
  struct MYSOFA_ARRAY DataSamplingRate;

  /** array of min-phase delays. Sizes are filters */
  struct MYSOFA_ARRAY DataDelay;

  /** general file attributes */
  struct MYSOFA_ATTRIBUTE *attributes;

  /** additional variables that might be present in a SOFA file */
  struct MYSOFA_VARIABLE *variables;
};

/* structure for lookup HRTF filters */
struct MYSOFA_LOOKUP {
  void *kdtree;
  float radius_min, radius_max;
  float theta_min, theta_max;
  float phi_min, phi_max;
};

struct MYSOFA_NEIGHBORHOOD {
  int elements;
  int *index;
};

/*
 * Opaque context for spherical-triangle interpolation.  It owns a private
 * snapshot of the measurement positions, FIR filters and delays taken during
 * mysofa_spherical_init(); the caller's MYSOFA_HRTF may be modified,
 * resampled or freed afterwards without affecting later queries.  Queries do
 * not allocate memory or modify the context, so one context may be queried
 * concurrently from several threads as long as each thread uses its own
 * output buffers.
 */
struct MYSOFA_SPHERICAL;
typedef struct MYSOFA_SPHERICAL MYSOFA_SPHERICAL;
typedef struct MYSOFA_HRTF MYSOFA_HRTF;

/*
 * Build a spherical-interpolation context from a cartesian HRTF.
 *
 * Requirements: SourcePosition must have Type="cartesian" (use
 * mysofa_tocartesian() first for spherical files), C=3, R=2, M>=4, N>0,
 * array sizes must match the dimensions, and all coordinates, FIR values and
 * delays must be finite.  All measurement radii must agree with the radius of
 * the first measurement to within a relative error of 2e-5, must be non-zero,
 * and normalized directions must be separated by more than 1e-6.  DataDelay
 * may contain either R=2 global values or M*R per-measurement values.
 *
 * On success a context is returned and *err is MYSOFA_OK.  On failure NULL
 * is returned and *err is set to a library error code.  The input HRTF is
 * only read, never modified.
 */
MYSOFA_EXPORT MYSOFA_SPHERICAL *mysofa_spherical_init(const MYSOFA_HRTF *hrtf,
                                                      int *err);

/*
 * Interpolate one direction.  `direction` is normalized internally; only its
 * direction matters.  `left` and `right` receive N coefficients each, and
 * `indices`/`weights` describe the three measurement points used.
 *
 * Delays are returned in the same units as the input DataDelay values.  No
 * propagation distance is added and no extra gain normalization is performed.
 * A global two-value DataDelay is returned unchanged; a per-measurement delay
 * is blended with the same barycentric weights as the FIR filters.
 *
 * Returns MYSOFA_OK on success.  A zero-length or non-finite direction,
 * a NULL pointer, or any other invalid argument returns an error code without
 * writing to the output buffers.
 */
MYSOFA_EXPORT int mysofa_spherical_getfilter(const MYSOFA_SPHERICAL *spherical,
                                            const float direction[3],
                                            float *left, float *right,
                                            float delays[2],
                                            unsigned indices[3],
                                            float weights[3]);

MYSOFA_EXPORT void mysofa_spherical_free(MYSOFA_SPHERICAL *spherical);

enum {
  MYSOFA_OK = 0,
  MYSOFA_INTERNAL_ERROR = -1,
  MYSOFA_INVALID_FORMAT = 10000,
  MYSOFA_UNSUPPORTED_FORMAT,
  MYSOFA_NO_MEMORY,
  MYSOFA_READ_ERROR,
  MYSOFA_INVALID_ATTRIBUTES,
  MYSOFA_INVALID_DIMENSIONS,
  MYSOFA_INVALID_DIMENSION_LIST,
  MYSOFA_INVALID_COORDINATE_TYPE,
  MYSOFA_ONLY_EMITTER_WITH_ECI_SUPPORTED,
  MYSOFA_ONLY_DELAYS_WITH_IR_OR_MR_SUPPORTED,
  MYSOFA_ONLY_THE_SAME_SAMPLING_RATE_SUPPORTED,
  MYSOFA_RECEIVERS_WITH_RCI_SUPPORTED,
  MYSOFA_RECEIVERS_WITH_CARTESIAN_SUPPORTED,
  MYSOFA_INVALID_RECEIVER_POSITIONS,
  MYSOFA_ONLY_SOURCES_WITH_MC_SUPPORTED
};

MYSOFA_EXPORT struct MYSOFA_HRTF *mysofa_load(const char *filename, int *err);
MYSOFA_EXPORT struct MYSOFA_HRTF *mysofa_load_data(const char *data, size_t size, int *err);

MYSOFA_EXPORT int mysofa_check(struct MYSOFA_HRTF *hrtf);
MYSOFA_EXPORT char *mysofa_getAttribute(struct MYSOFA_ATTRIBUTE *attr, char *name);
MYSOFA_EXPORT void mysofa_tospherical(struct MYSOFA_HRTF *hrtf);
MYSOFA_EXPORT void mysofa_tocartesian(struct MYSOFA_HRTF *hrtf);
MYSOFA_EXPORT void mysofa_free(struct MYSOFA_HRTF *hrtf);

MYSOFA_EXPORT struct MYSOFA_LOOKUP *mysofa_lookup_init(struct MYSOFA_HRTF *hrtf);
MYSOFA_EXPORT int mysofa_lookup(struct MYSOFA_LOOKUP *lookup, float *coordinate);
MYSOFA_EXPORT void mysofa_lookup_free(struct MYSOFA_LOOKUP *lookup);

MYSOFA_EXPORT struct MYSOFA_NEIGHBORHOOD *
mysofa_neighborhood_init(struct MYSOFA_HRTF *hrtf,
                         struct MYSOFA_LOOKUP *lookup);
MYSOFA_EXPORT struct MYSOFA_NEIGHBORHOOD *mysofa_neighborhood_init_withstepdefine(
    struct MYSOFA_HRTF *hrtf, struct MYSOFA_LOOKUP *lookup,
    float neighbor_angle_step, float neighbor_radius_step);
MYSOFA_EXPORT int *mysofa_neighborhood(struct MYSOFA_NEIGHBORHOOD *neighborhood, int pos);
MYSOFA_EXPORT void mysofa_neighborhood_free(struct MYSOFA_NEIGHBORHOOD *neighborhood);

MYSOFA_EXPORT float *mysofa_interpolate(struct MYSOFA_HRTF *hrtf, float *cordinate,
                          int nearest, int *neighborhood, float *fir,
                          float *delays);

MYSOFA_EXPORT int mysofa_resample(struct MYSOFA_HRTF *hrtf, float samplerate);
MYSOFA_EXPORT float mysofa_loudness(struct MYSOFA_HRTF *hrtf);
MYSOFA_EXPORT int mysofa_minphase(struct MYSOFA_HRTF *hrtf, float threshold);

MYSOFA_EXPORT struct MYSOFA_EASY *mysofa_cache_lookup(const char *filename, float samplerate);
MYSOFA_EXPORT struct MYSOFA_EASY *mysofa_cache_store(struct MYSOFA_EASY *,
                                       const char *filename, float samplerate);
MYSOFA_EXPORT void mysofa_cache_release(struct MYSOFA_EASY *);
MYSOFA_EXPORT void mysofa_cache_release_all(void);

MYSOFA_EXPORT void mysofa_c2s(float values[3]);
MYSOFA_EXPORT void mysofa_s2c(float values[3]);

struct MYSOFA_EASY {
  struct MYSOFA_HRTF *hrtf;
  struct MYSOFA_LOOKUP *lookup;
  struct MYSOFA_NEIGHBORHOOD *neighborhood;
  float *fir;
};

MYSOFA_EXPORT struct MYSOFA_EASY *mysofa_open(const char *filename, float samplerate,
                                int *filterlength, int *err);
MYSOFA_EXPORT struct MYSOFA_EASY *mysofa_open_no_norm(const char *filename, float samplerate,
                                        int *filterlength, int *err);
MYSOFA_EXPORT struct MYSOFA_EASY *mysofa_open_advanced(const char *filename, float samplerate,
                                         int *filterlength, int *err, bool norm,
                                         float neighbor_angle_step,
                                         float neighbor_radius_step);
MYSOFA_EXPORT struct MYSOFA_EASY *mysofa_open_data(const char *data, long size,
                                     float samplerate, int *filterlength,
                                     int *err);
MYSOFA_EXPORT struct MYSOFA_EASY *mysofa_open_data_no_norm(const char *data, long size,
                                             float samplerate,
                                             int *filterlength, int *err);
MYSOFA_EXPORT struct MYSOFA_EASY *mysofa_open_data_advanced(
    const char *data, long size, float samplerate, int *filterlength, int *err,
    bool norm, float neighbor_angle_step, float neighbor_radius_step);
MYSOFA_EXPORT struct MYSOFA_EASY *mysofa_open_cached(const char *filename, float samplerate,
                                       int *filterlength, int *err);
MYSOFA_EXPORT void mysofa_getfilter_short(struct MYSOFA_EASY *easy, float x, float y, float z,
                            short *IRleft, short *IRright, int *delayLeft,
                            int *delayRight);
MYSOFA_EXPORT void mysofa_getfilter_float(struct MYSOFA_EASY *easy, float x, float y, float z,
                            float *IRleft, float *IRright, float *delayLeft,
                            float *delayRight);
MYSOFA_EXPORT void mysofa_getfilter_float_nointerp(struct MYSOFA_EASY *easy, float x, float y,
                                     float z, float *IRleft, float *IRright,
                                     float *delayLeft, float *delayRight);
MYSOFA_EXPORT void mysofa_close(struct MYSOFA_EASY *easy);
MYSOFA_EXPORT void mysofa_close_cached(struct MYSOFA_EASY *easy);

MYSOFA_EXPORT void mysofa_getversion(int *major, int *minor, int *patch);

#ifdef __cplusplus
}
#endif
#endif
