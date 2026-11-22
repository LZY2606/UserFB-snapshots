/*
 * H.265 video codec.
 * Copyright (c) 2013-2014 struktur AG, Dirk Farin <farin@struktur.de>
 *
 * This file is part of libde265.
 *
 * libde265 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libde265 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libde265.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LIBDE265_PICTURE_BOUNDARY_H
#define LIBDE265_PICTURE_BOUNDARY_H

#include <stdint.h>
#include <stddef.h>
#include <deque>

/* Tracks de265_push_picture_boundary() markers through the decoder.
 *
 * The NAL_Parser keeps the boundary markers in the same FIFO as the NAL
 * units, so a marker is consumed only after every NAL of its picture has
 * been dequeued. This class records the bookkeeping around those markers:
 *
 *  - Every VCL NAL that starts a new picture (first_slice_segment_in_pic)
 *    gets a sequential picture number via note_first_slice().
 *  - note_boundary() is called when a marker reaches the head of the input
 *    queue. It compares the number of pictures started up to that point with
 *    the number at the previous marker: a span in which no picture started
 *    (an "empty boundary", e.g. one carrying only parameter sets or no NAL
 *    at all) swallows its tag and never notifies. Otherwise the marker
 *    belongs to the picture that started in its span.
 *  - note_picture_finished() is called once, after all slices of the
 *    picture, deblocking/SAO and its suffix SEIs have been processed and
 *    the reference picture is in place. A tag is published only when both
 *    its marker has been consumed and its picture has finished, in either
 *    order.
 *
 * Notifications therefore come out in decoding order, exactly once per
 * non-empty boundary, independently of the POC-based display order.
 *
 * All methods run on the single owner thread of the decoder context (see
 * the thread-safety notes in de265.h), so no internal locking is needed.
 */
class picture_boundary_tracker
{
 public:
  // Assign the next sequential picture number to a picture that just
  // received its first slice. Returns the assigned number.
  unsigned note_first_slice();

  // Record consumption of a boundary marker carrying 'tag'. All NAL units
  // of the marker's span have been dequeued at this point.
  void note_boundary(uint64_t tag);

  // Record that the picture with the given number has been completely
  // decoded and inserted into the DPB/reorder machinery.
  void note_picture_finished(unsigned picture_number);

  // True when the marker of the picture with 'picture_number' has already
  // been consumed, i.e. no further NAL units can belong to that picture.
  bool is_picture_closed(unsigned picture_number) const;

  // True once any non-empty boundary has been seen, so the decoder can keep
  // boundary-driven pictures going without an end-of-frame marker.
  bool is_active() const { return m_active; }

  // Remove the oldest completed tag. Returns 1 and writes to 'tag' when a
  // notification was available; returns 0 and leaves '*tag' untouched
  // otherwise. A NULL output pointer is allowed and simply discards the
  // notification.
  int pop_completed(uint64_t* tag);

  size_t number_of_completed() const { return m_completed.size(); }

  // Drop all marker state and every notification not yet retrieved.
  void reset();

 private:
  struct pending_tag {
    unsigned picture_number;
    uint64_t tag;
  };

  // Move every tag whose picture has finished to the completion queue.
  void publish_due();

  unsigned m_pictures_started = 0;          // first slices seen so far
  unsigned m_pictures_at_last_boundary = 0; // first slices covered by the
                                            // previously consumed marker
  unsigned m_pictures_closed = 0;           // pictures whose marker was consumed
  unsigned m_pictures_finished = 0;         // pictures fully decoded
  bool m_active = false;

  std::deque<pending_tag> m_tags;       // markers consumed, ordered by picture
  std::deque<uint64_t>    m_completed;  // notifications waiting for the caller
};

#endif
