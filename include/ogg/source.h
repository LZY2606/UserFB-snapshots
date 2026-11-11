/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE Ogg CONTAINER SOURCE CODE.              *
 * USE, DISTRIBUTION AND REDISTRIBUTION OF THIS LIBRARY SOURCE IS   *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE OggVorbis SOURCE CODE IS (C) COPYRIGHT 1994-2018             *
 * by the Xiph.Org Foundation http://www.xiph.org/                  *
 *                                                                  *
 ********************************************************************

 function: decode Ogg pages into packets while tracking the byte
           ranges of the original input each packet was built from

 ********************************************************************/
#ifndef _OGG_SOURCE_H
#define _OGG_SOURCE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <ogg/os_types.h>
#include <ogg/ogg.h>

/* ogg_source_span describes one contiguous byte range of the original
   input that contributed payload to a decoded packet.  offset is the
   absolute byte position of the range within the original input (the
   same coordinate space as the offsets handed to
   ogg_source_stream_pagein); bytes is the length of the range.  Only
   packet payload bytes are covered: page headers, lacing tables and
   the payload of dropped partial packets never appear in a span. */
typedef struct {
  ogg_int64_t offset;
  long bytes;
} ogg_source_span;

/* ogg_source_info is filled by ogg_source_stream_packetout() and
   ogg_source_stream_packetpeek() to describe where the returned
   packet came from.  spans points to a read-only array of count
   ogg_source_span entries, in the order the bytes appear in the
   packet; concatenating the ranges in array order reproduces the
   packet payload byte for byte.  Spans are merged when they are
   adjacent within a single input page, but never across page
   boundaries.  A zero-length packet reports count 0.

   The spans array is owned by the ogg_source_stream and remains
   valid only until the next call to any ogg_source_stream_* function
   on the same stream.  Copy the data if it is needed longer; do not
   free it. */
typedef struct {
  const ogg_source_span *spans;
  size_t count;
} ogg_source_info;

/* ogg_source_stream is an opaque decode stream equivalent to
   ogg_stream_state, extended with input-origin tracking. */
typedef struct ogg_source_stream ogg_source_stream;

/* Create a source-tracking decode stream for the given serialno.
   Returns NULL on allocation failure. */
extern ogg_source_stream *ogg_source_stream_new(int serialno);

/* Destroy a stream and release all decode and tracking state.
   NULL is accepted. */
extern void ogg_source_stream_free(ogg_source_stream *ss);

/* Reset a stream to its initial state and select a new serialno,
   releasing all buffered packet and source-tracking state.
   Returns 0 on success, -1 on a corrupt stream. */
extern int ogg_source_stream_reset(ogg_source_stream *ss, int serialno);

/* Submit a page taken from the sync layer.  offset is the absolute
   byte position of the page's capture pattern within the original
   input.  A negative offset, or a page whose header+body extent would
   overflow a signed 64-bit offset, is rejected with -1 before any
   stream state is modified.  Pages with a non-matching serial number
   or a nonzero version are rejected like ogg_stream_pagein() and
   contribute no source information.  Otherwise behaves exactly like
   ogg_stream_pagein() and returns 0 on success, -1 on error. */
extern int ogg_source_stream_pagein(ogg_source_stream *ss, ogg_page *og,
                                    ogg_int64_t offset);

/* Retrieve the next decoded packet, like ogg_stream_packetout().
   Either op or info may be NULL; the consumption rules of
   ogg_stream_packetout() apply unchanged.  When info is non-NULL and
   a packet is returned, *info describes the input ranges the packet
   payload was assembled from.  When the return value is 0 (need more
   data) or -1 (hole in the stream; the marker is consumed, as with
   ogg_stream_packetout()), *info is cleared to no source so that
   stale ranges from a previous packet are never mistaken as current. */
extern int ogg_source_stream_packetout(ogg_source_stream *ss,
                                       ogg_packet *op,
                                       ogg_source_info *info);

/* Like ogg_stream_packetpeek(): a complete packet is reported without
   being consumed, while a hole marker is still consumed and reported
   as -1.  info follows the same rules as in
   ogg_source_stream_packetout(). */
extern int ogg_source_stream_packetpeek(ogg_source_stream *ss,
                                        ogg_packet *op,
                                        ogg_source_info *info);

#ifdef __cplusplus
}
#endif

#endif  /* _OGG_SOURCE_H */
