/*
 * Copyright © 2026 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */
#if !defined(NESTEGG_EVENTS_6f0b1d4e_2a71_4c8f_9c55_0e7d3a2b8c11)
#define NESTEGG_EVENTS_6f0b1d4e_2a71_4c8f_9c55_0e7d3a2b8c11

#include <stddef.h>
#include <stdint.h>

#include <nestegg/nestegg.h>

#if defined(__cplusplus)
extern "C" {
#endif

/** Event type for a fragment of compressed frame data. */
#define NESTEGG_EVENT_DATA   1
/** Event type for the final metadata of a block. */
#define NESTEGG_EVENT_COMMIT 2

/** A library-owned event returned by #nestegg_read_event.
 *
 * DATA events carry a fragment of a frame.  Fragments never span frames and
 * are at most 1024 bytes each.  A frame with no payload produces a single
 * DATA event with #length zero and both #first and #last set.
 *
 * COMMIT events carry the final metadata of a block once the whole block
 * (and, for a BlockGroup, its trailing metadata) has been read.  #data is
 * NULL and #length is zero.  The #packet_id of the block is complete only
 * once its COMMIT has been received.
 */
typedef struct nestegg_event {
  int type;                    /**< One of #NESTEGG_EVENT_DATA or
                                    #NESTEGG_EVENT_COMMIT. */
  uint64_t packet_id;          /**< Block identity, starting at 1. */
  uint64_t timestamp;          /**< Block timestamp in nanoseconds. */
  uint64_t duration;           /**< Block duration in nanoseconds. */
  unsigned int track;          /**< Zero-based track index. */
  unsigned int frame_index;    /**< Frame index within the block. */
  unsigned int frame_count;    /**< Total frame count of the block. */
  const unsigned char * data;  /**< Borrowed fragment data, or NULL for a
                                    COMMIT. */
  size_t length;               /**< Fragment length in bytes. */
  int first;                   /**< DATA is the first fragment of a frame. */
  int last;                    /**< DATA is the last fragment of a frame. */
  int keyframe;                /**< One of the
                                    NESTEGG_PACKET_HAS_KEYFRAME values. */
  int has_duration;            /**< BlockDuration was present. */
  int has_discard_padding;     /**< DiscardPadding was present. */
  int has_reference;           /**< ReferenceBlock was present. */
  int64_t discard_padding;     /**< DiscardPadding in nanoseconds. */
  int64_t reference;           /**< ReferenceBlock in timecode ticks. */
} nestegg_event;

/** Read the next fragment or commit event from the stream.
 *
 * The returned pointer is owned by @a context and remains valid until the
 * next call to #nestegg_read_event, a successful seek, or
 * #nestegg_destroy.  The data pointer inside a DATA event remains valid for
 * the same period.
 *
 * @param context Stream context initialized by #nestegg_init.
 * @param event   Storage for the returned event.  Set to NULL whenever this
 *                function does not return 1.
 * @retval  1 An event was returned.
 * @retval  0 End of stream.
 * @retval -1 Error.
 */
int nestegg_read_event(nestegg * context, const nestegg_event ** event);

#if defined(__cplusplus)
}
#endif

#endif /* NESTEGG_EVENTS_6f0b1d4e_2a71_4c8f_9c55_0e7d3a2b8c11 */
