/*
** Copyright (c) 2017 rxi
**
** This library is free software; you can redistribute it and/or modify it
** under the terms of the MIT license. See `cmixer.c` for details.
**/

#ifndef CMIXER_H
#define CMIXER_H

#define CM_VERSION "0.1.1"

typedef short           cm_Int16;
typedef int             cm_Int32;
typedef long long       cm_Int64;
typedef unsigned char   cm_UInt8;
typedef unsigned short  cm_UInt16;
typedef unsigned        cm_UInt32;

typedef struct cm_Source cm_Source;
typedef struct cm_Bus cm_Bus;

typedef struct {
  int type;
  void *udata;
  const char *msg;
  cm_Int16 *buffer;
  int length;
} cm_Event;

typedef void (*cm_EventHandler)(cm_Event *e);

typedef struct {
  cm_EventHandler handler;
  void *udata;
  int samplerate;
  int length;
} cm_SourceInfo;


enum {
  CM_STATE_STOPPED,
  CM_STATE_PLAYING,
  CM_STATE_PAUSED
};

enum {
  CM_EVENT_LOCK,
  CM_EVENT_UNLOCK,
  CM_EVENT_DESTROY,
  CM_EVENT_SAMPLES,
  CM_EVENT_REWIND
};


const char* cm_get_error(void);
void cm_init(int samplerate);
void cm_set_lock(cm_EventHandler lock);
void cm_set_master_gain(double gain);
void cm_process(cm_Int16 *dst, int len);

cm_Source* cm_new_source(const cm_SourceInfo *info);
cm_Source* cm_new_source_from_file(const char *filename);
cm_Source* cm_new_source_from_mem(void *data, int size);
void cm_destroy_source(cm_Source *src);
double cm_get_length(cm_Source *src);
double cm_get_position(cm_Source *src);
int cm_get_state(cm_Source *src);
void cm_set_gain(cm_Source *src, double gain);
void cm_set_pan(cm_Source *src, double pan);
void cm_set_pitch(cm_Source *src, double pitch);
void cm_set_loop(cm_Source *src, int loop);
void cm_play(cm_Source *src);
void cm_pause(cm_Source *src);
void cm_stop(cm_Source *src);

/* Auxiliary buses.

** The master output is a permanent summing bus. A source contributes its
** post-resampling, post-gain and post-pan stereo signal to a bus; buffers are
** summed, never clipped, between buses. A delay bus applies the effect
**     y[n] = x[n - D] + feedback * y[n - D]
** per channel, where D is the delay length in frames. Bus and edge gains are
** applied to the bus output and never written back into the delay history.
** The routing graph must stay acyclic; the master output cannot be an edge
** source. All bus functions must be called between cm_process() calls from a
** single thread. */
cm_Bus* cm_new_bus(double gain);
cm_Bus* cm_new_delay_bus(int delay, double feedback);
int cm_destroy_bus(cm_Bus *bus);
void cm_set_bus_gain(cm_Bus *bus, double gain);
cm_Bus* cm_get_master_bus(void);
cm_Bus* cm_get_source_bus(cm_Source *src);
void cm_set_source_bus(cm_Source *src, cm_Bus *bus);
void cm_set_source_master(cm_Source *src);
int cm_connect_bus(cm_Bus *from, cm_Bus *to, double gain);
int cm_set_edge_gain(cm_Bus *from, cm_Bus *to, double gain);
int cm_disconnect_bus(cm_Bus *from, cm_Bus *to);
void cm_clear_bus_history(cm_Bus *bus);

#endif
