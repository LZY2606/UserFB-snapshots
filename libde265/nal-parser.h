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

#ifndef DE265_NAL_PARSER_H
#define DE265_NAL_PARSER_H

#include "libde265/sps.h"
#include "libde265/pps.h"
#include "libde265/nal.h"
#include "libde265/util.h"
#include "libde265/de265.h"

#include <vector>
#include <memory>
#include <deque>


class NAL_unit {
 public:
  NAL_unit() = default;
  ~NAL_unit();

  nal_header header;

  de265_PTS  pts = 0;
  void*      user_data = nullptr;


  // --- rbsp data ---

  [[nodiscard]] bool resize(int new_size);
  [[nodiscard]] bool append(const unsigned char* data, int n);
  [[nodiscard]] bool set_data(const unsigned char* data, int n);

  int size() const { return data_size; }
  void set_size(int s) { data_size=s; }
  unsigned char* data() { return nal_data; }
  const unsigned char* data() const { return nal_data; }


  // --- skipped stuffing bytes ---

  uint32_t num_skipped_bytes_before(uint32_t byte_position, uint32_t headerLength) const;
  uint32_t num_skipped_bytes() const { return skipped_bytes.size(); }

  //void clear_skipped_bytes() { skipped_bytes.clear(); }

  /* Mark a byte as skipped. It is assumed that the byte is already removed
     from the input data. The NAL data is not modified.
  */
  void insert_skipped_byte(uint32_t pos);

  /* Remove all stuffing bytes from NAL data. The NAL data is modified and
     the removed bytes are marked as skipped bytes.
   */
  void remove_stuffing_bytes();

 private:
  unsigned char* nal_data = nullptr;
  int data_size = 0;
  int capacity = 0;

  std::vector<uint32_t> skipped_bytes; // up to position[x], there were 'x' skipped bytes
};


class NAL_Parser
{
 public:
  NAL_Parser();
  ~NAL_Parser();

  // Point the parser at the live security limits struct so that runtime
  // changes (via de265_get_security_limits()) take effect immediately.
  void set_security_limits(const de265_security_limits* limits) { m_security_limits = limits; }

  de265_error push_data(const unsigned char* data, int len,
                        de265_PTS pts, void* user_data = nullptr);

  de265_error push_NAL(const unsigned char* data, int len,
                       de265_PTS pts, void* user_data = nullptr);

  std::unique_ptr<NAL_unit> pop_from_NAL_queue();
  de265_error flush_data();
  void        mark_end_of_stream() { end_of_stream=true; }
  void        mark_end_of_frame() { end_of_frame=true; }
  void  remove_pending_input_data();

  // Enqueue a picture-boundary marker after all currently queued NAL units.
  // Markers stay in the same FIFO and therefore in the same relative order
  // as the NAL units; they do not participate in byte/NAL counting.
  void push_picture_boundary(uint64_t tag);

  // True when the next queue entry is a boundary marker rather than a NAL.
  bool head_is_boundary() const;

  // Remove and return the boundary marker at the head of the queue.
  uint64_t pop_boundary();

  int bytes_in_input_queue() const {
    int size = nBytes_in_NAL_queue;
    if (pending_input_NAL) { size += pending_input_NAL->size(); }
    return size;
  }

  int number_of_NAL_units_pending() const {
    int size = number_of_complete_NAL_units_pending();
    if (pending_input_NAL) { size++; }
    return size;
  }

  int number_of_complete_NAL_units_pending() const;

  // Release a NAL. Takes ownership by value, so a move transfers the object here
  // and leaves the caller holding nullptr; a redundant release therefore passes
  // nullptr and is a harmless no-op, which is what makes a double release
  // impossible to express.
  void free_NAL_unit(std::unique_ptr<NAL_unit> nal);


  int get_NAL_queue_length() const;
  bool is_end_of_stream() const { return end_of_stream; }
  bool is_end_of_frame() const { return end_of_frame; }

 private:
  // One entry of the ordered input FIFO. It is either a NAL unit or a
  // picture-boundary marker; mixing both in one queue guarantees that a
  // marker is consumed only after the NAL units of its picture.
  struct queue_entry {
    std::unique_ptr<NAL_unit> nal;     // non-null for a NAL entry
    bool is_boundary = false;          // true for a boundary marker
    uint64_t boundary_tag = 0;         // valid when is_boundary
  };

  // byte-stream level

  bool end_of_stream = false; // data in pending_input_data is end of stream
  bool end_of_frame = false;  // data in pending_input_data is end of frame
  int  input_push_state = 0;

  std::unique_ptr<NAL_unit> pending_input_NAL;

  const de265_security_limits* m_security_limits = nullptr;


  // NAL level

  // NAL units (stuffing bytes removed) and picture-boundary markers, kept in
  // the order in which they were pushed.
  std::deque<queue_entry> NAL_queue;
  int nBytes_in_NAL_queue = 0; // data bytes currently held by NAL entries

  void push_to_NAL_queue(std::unique_ptr<NAL_unit>);

  // Returns true if a NAL unit of the given size is within the configured
  // security limit (or if no limit is set).
  bool nal_size_within_limit(int64_t nal_size) const {
    return m_security_limits == nullptr ||
           m_security_limits->max_NAL_size_bytes == 0 ||
           nal_size <= m_security_limits->max_NAL_size_bytes;
  }


  [[nodiscard]] std::unique_ptr<NAL_unit> alloc_NAL_unit(int size);
};


#endif
