/*
 * H.265 video codec.
 * Copyright (c) 2026 struktur AG, Dirk Farin <farin@struktur.de>
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
 * You should have received a copy of the GNU GNU Lesser General Public License
 * along with libde265.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LIBDE265_FILTER_PIPELINE_H
#define LIBDE265_FILTER_PIPELINE_H

#include <stddef.h>

struct de265_image;
class image_unit;
class decoder_context;
class thread_task;


/* Bounded in-place deblocking/SAO pipeline.
 *
 * Returns true when the picture geometry is handled by the in-place pipeline.
 * All other geometries (4:2:2, 4:4:4, separate colour planes, unsupported bit
 * depths, ...) keep using the legacy full-image SAO copy path.
 */
bool inplace_filter_pipeline_supported(const de265_image* img);

/* Run the complete post-processing stage on the decoder's existing thread
 * pool and block until the last task has finished. The result is written into
 * the image's own planes; no second output image is allocated and no planes
 * are exchanged. On workspace allocation failure (possible only before the
 * first filtering write) returns false and leaves the image untouched, so the
 * caller can fall back to the legacy path.
 */
bool run_inplace_filter_pipeline(decoder_context* ctx, image_unit* imgunit);


/* --- Test-only observation points --------------------------------------
 *
 * These hooks are never installed by production code. A filter pipeline
 * observer is per decoder context: two decoder instances never share a
 * pipeline, its tasks or its workspace, and therefore never share hooks.
 * All callbacks are invoked from worker threads; synchronization is the
 * observer's responsibility.
 */
struct filter_pipeline_observer {
  virtual ~filter_pipeline_observer() = default;

  // A horizontal deblocking row task has passed every dependency wait and is
  // about to touch pixels. The callback is allowed to block on a condition
  // variable handshake.
  virtual void on_deblock_h_enter(int ctb_y) { (void)ctb_y; }

  // A SAO row task is about to run (after the neighbour-row swap).
  virtual void on_sao_enter(int ctb_y) { (void)ctb_y; }

  // A SAO row task has finished and its slot has been released.
  virtual void on_sao_finish(int ctb_y) { (void)ctb_y; }

  // A bounded workspace slot was assigned to / recycled from a CTB row.
  // Slot ids are in [0, W+3); seeing the same id on different rows proves
  // slot reuse without per-row pixel allocation.
  virtual void on_slot_acquire(int ctb_y, int slot_id)
  { (void)ctb_y; (void)slot_id; }
  virtual void on_slot_release(int ctb_y, int slot_id)
  { (void)ctb_y; (void)slot_id; }

  // One pixel plane buffer was allocated / freed, with its size in bytes.
  virtual void on_workspace_alloc(size_t bytes) { (void)bytes; }
  virtual void on_workspace_free(size_t bytes) { (void)bytes; }
};

void filter_pipeline_set_observer(decoder_context* ctx,
                                  filter_pipeline_observer* observer);

#endif
