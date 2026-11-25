/*
 * Copyright © 2026 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */
#if !defined(NESTEGG_EVENTS_7c2e9a41_1b6d_4f0a_9c33_2e8d5b7a1101)
#define NESTEGG_EVENTS_7c2e9a41_1b6d_4f0a_9c33_2e8d5b7a1101

#include <stddef.h>
#include <stdint.h>

#include "nestegg.h"

#if defined(__cplusplus)
extern "C" {
#endif

/** @file
    Fragment-first, pull based event reading API.

    nestegg_read_event() delivers each laced frame as fixed size DATA
    fragments while the block body is being read.  Once the whole
    Block/SimpleBlock and its surrounding BlockGroup have been parsed, a
    single COMMIT event carries the final packet metadata. */

#define NESTEGG_EVENT_DATA   1 /**< A body fragment (possibly zero length). */
#define NESTEGG_EVENT_COMMIT 2 /**< End of block/group with final metadata. */

/** A single fragment or commit event.

    Pointers exposed through this structure are borrowed by the library and
    remain valid only until the next nestegg_read_event(), seek or
    nestegg_destroy() call on the owning context.  Consumers must copy any
    DATA bytes they need to retain immediately. */
typedef struct nestegg_event {
  int type;                       /**< #NESTEGG_EVENT_DATA or #NESTEGG_EVENT_COMMIT. */
  uint64_t packet_id;              /**< Monotonic id shared by one block. */
  uint64_t timestamp;             /**< Absolute timestamp in nanoseconds. */
  uint64_t duration;              /**< Committed BlockDuration in nanoseconds. */
  unsigned int track;             /**< Zero based track index. */
  unsigned int frame_index;       /**< Zero based lace frame index. */
  unsigned int frame_count;       /**< Total number of lace frames. */
  const unsigned char * data;     /**< Borrowed fragment bytes; NULL on COMMIT. */
  size_t length;                  /**< Fragment length; zero for COMMIT/empty frames. */
  int first;                      /**< Non-zero on the frame's first fragment. */
  int last;                       /**< Non-zero on the frame's last fragment. */
  int keyframe;                   /**< One of the NESTEGG_PACKET_HAS_KEYFRAME_* values. */
  int has_duration;               /**< Non-zero if BlockDuration was present. */
  int has_discard_padding;        /**< Non-zero if DiscardPadding was present. */
  int has_reference;              /**< Non-zero if ReferenceBlock was present. */
  int64_t discard_padding;        /**< Signed DiscardPadding value in nanoseconds. */
  int64_t reference;              /**< Raw signed ReferenceBlock value. */
} nestegg_event;

/** Read the next fragment or commit @a event.

    @param context Parser context returned by nestegg_init().
    @param event   Storage for a borrowed pointer to the next event.
    @retval  1 An event was returned through @a event.
    @retval  0 Normal end of stream; @a event is set to NULL.
    @retval -1 A parse or IO error occurred; @a event is set to NULL. */
int nestegg_read_event(nestegg * context, const nestegg_event ** event);

#if defined(__cplusplus)
}
#endif

#endif /* NESTEGG_EVENTS_7c2e9a41_1b6d_4f0a_9c33_2e8d5b7a1101 */
