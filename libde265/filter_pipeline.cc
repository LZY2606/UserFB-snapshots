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
 * You should have received a copy of the GNU Lesser General Public License
 * along with libde265.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "filter_pipeline.h"

#include "decctx.h"
#include "deblock.h"
#include "sao.h"
#include "image.h"
#include "threads.h"
#include "util.h"

#include <algorithm>
#include <new>
#include <vector>


/* Bounded in-place deblocking/SAO pipeline.
 *
 * Task graph per CTB row r (R rows in total):
 *
 *   V(r)   vertical deblocking, ready immediately (whole picture is PREFILTER)
 *   H(r)   horizontal deblocking, ready after V(r-1), V(r), V(r+1)
 *   S(r)   pre-SAO window snapshot, ready after H(r-1)*, H(r) and H(r+1)*
 *   A(r)   in-place SAO, ready after S(r-1)*, S(r) and S(r+1)*
 *
 *   (*) the neighbour-row variant does not exist for the border rows.
 *
 * Rationale for the edges:
 * - H(r) writes up to 4 luma rows into row r-1 and reads V results of the
 *   rows above/below, mirroring the legacy thread_task_deblock_CTBRow waits.
 * - S(r) saves image rows [r*Hc-1, (r+1)*Hc+1). Both edge rows are foreign
 *   write targets: H(r-1) reaches into the top edge (chroma q0 one row down,
 *   luma up to two), H(r+1) into the bottom edge. S(r) therefore waits for
 *   all three horizontal passes, exactly like the legacy SAO row task.
 * - In-place SAO needs both neighbour windows to be safe. A(r) writes row
 *   r*Hc, which is the bottom edge row saved by S(r-1), so A(r) waits for
 *   S(r-1) as well as S(r); and A(r-1) overwrites the top edge of S(r),
 *   which is covered by A(r-1) waiting for S(r).
 *
 * Every unit is enqueued exactly once, only after it is runnable, so workers
 * never block on dependencies, never busy-poll and never re-queue units.
 * The decoder thread acts as coordinator and sleeps on a condition variable.
 * Pixel workspace is a fixed pool of W+4 row windows, acquired before S(r) is
 * published and recycled only after A(r) has finished reading it.
 */

namespace {

enum UnitKind { UNIT_V, UNIT_H, UNIT_SNAP, UNIT_SAO };

struct Unit {
  UnitKind kind;
  int      row;
};

struct Slot {
  std::vector<uint8_t*> plane;   // 3 workspace planes, row 0 == image row origin[r][c]
  bool   inUse = false;
  int    row = -1;               // CTB row this slot was snapped for
};

class InplaceFilterPipeline {
public:
  InplaceFilterPipeline(decoder_context* ctx_, image_unit* imgunit_, int workers_);
  ~InplaceFilterPipeline();

  // Allocate the bounded pixel workspace. Call before any task is published.
  bool allocate();

  // Run the whole pipeline on the existing thread pool and block until done.
  void run();

  void worker_execute(const Unit& u);

private:
  void run_deblock_unit(const Unit& u);
  void run_snapshot(int row);
  void run_sao(int row);

  // Coordinator-side scheduling, mutex held.
  void publish_locked();
  bool take_ready_locked(Unit& out);

  Slot* acquire_slot_locked(int row);
  void  release_slot_locked(Slot* slot);

  decoder_context* mCtx;
  image_unit*      mImgunit;
  de265_image*     mImg;
  const seq_parameter_set* mSps;
  int              mWorkers;
  int              mRows;
  bool             mDoDeblock;
  bool             mDoSAO;

  std::vector<uint8_t> mVDone;
  std::vector<uint8_t> mHDone;
  std::vector<uint8_t> mSnapDone;
  std::vector<uint8_t> mScheduled;   // unit was enqueued at least once

  std::vector<Unit>  mReady;
  int                mFinished = 0;
  int                mTotalUnits = 0;

  std::mutex              mMutex;
  std::condition_variable mCond;

  // Workspace geometry, per plane (3 entries).
  int      mUnitRows[3];    // Hc: sample rows of one CTB row
  ptrdiff_t mStride[3];     // Sc: sample stride
  int      mBytesPerSample[3]; // Bc
  size_t   mPlaneBytes[3];  // Sc * (Hc+2) * Bc

  std::vector<Slot> mSlots;

  // Row -> assigned slot index, valid between snapshot and SAO recycle.
  std::vector<int> mSlotOfRow;
};


class PipelineUnitTask : public thread_task {
public:
  PipelineUnitTask(InplaceFilterPipeline* p, const Unit& u) : mPipeline(p), mUnit(u) {}

  void work() override { mPipeline->worker_execute(mUnit); }

  std::string name() const override {
    const char* k = "v";
    switch (mUnit.kind) {
    case UNIT_V:    k = "deblk-v"; break;
    case UNIT_H:    k = "deblk-h"; break;
    case UNIT_SNAP: k = "snap";    break;
    case UNIT_SAO:  k = "sao";     break;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%s-%d", k, mUnit.row);
    return buf;
  }

private:
  InplaceFilterPipeline* mPipeline;
  Unit                   mUnit;
};

} // namespace


InplaceFilterPipeline::InplaceFilterPipeline(decoder_context* ctx_,
                                             image_unit* imgunit_,
                                             int workers_)
  : mCtx(ctx_), mImgunit(imgunit_), mImg(imgunit_->img),
    mSps(&imgunit_->img->get_sps()), mWorkers(workers_),
    mRows(mSps->PicHeightInCtbsY),
    mDoDeblock(!mCtx->param_disable_deblocking),
    mDoSAO(!mCtx->param_disable_sao && mSps->sample_adaptive_offset_enabled_flag)
{
  const int planes = (mSps->ChromaArrayType == CHROMA_MONO) ? 1 : 3;

  for (int c=0;c<3;c++) {
    const int subH = (c==0) ? 1 : mSps->SubHeightC;
    mUnitRows[c] = (c < planes) ? (mSps->CtbSizeY / subH) : 0;
    mStride[c]   = mImg->get_image_stride(c);
    mBytesPerSample[c] = mImg->get_bytes_per_pixel(c);
    if (c < planes) {
      mPlaneBytes[c] = (size_t)mStride[c] * (mUnitRows[c] + 2) * mBytesPerSample[c];
    } else {
      mPlaneBytes[c] = 0;
    }
  }

  mVDone.assign(mRows, 0);
  mHDone.assign(mRows, 0);
  mSnapDone.assign(mRows, 0);
  mScheduled.assign((size_t)mRows * 4, 0);
  mSlotOfRow.assign(mRows, -1);

  mSlots.resize((size_t)mWorkers + 4);
}


InplaceFilterPipeline::~InplaceFilterPipeline()
{
  for (Slot& s : mSlots) {
    for (int c=0;c<(int)s.plane.size();c++) {
      if (s.plane[c]) {
        delete[] s.plane[c];
        s.plane[c] = nullptr;
        if (mCtx->m_filter_pipeline_observer) {
          mCtx->m_filter_pipeline_observer->on_workspace_free(mPlaneBytes[c]);
        }
      }
    }
  }
}


bool InplaceFilterPipeline::allocate()
{
  const int planes = (mSps->ChromaArrayType == CHROMA_MONO) ? 1 : 3;

  // All pixel workspace is acquired before the first filtering write. On any
  // failure the image is still untouched and the caller takes the legacy path.
  try {
    for (Slot& slot : mSlots) {
      slot.plane.assign(3, nullptr);
      for (int c=0;c<planes;c++) {
        slot.plane[c] = new uint8_t[mPlaneBytes[c]];
        if (mCtx->m_filter_pipeline_observer) {
          mCtx->m_filter_pipeline_observer->on_workspace_alloc(mPlaneBytes[c]);
        }
      }
      slot.inUse = false;
    }
  }
  catch (const std::bad_alloc&) {
    for (Slot& slot : mSlots) {
      for (int c=0;c<3;c++) {
        if (slot.plane[c]) {
          delete[] slot.plane[c];
          slot.plane[c] = nullptr;
          if (mCtx->m_filter_pipeline_observer) {
            mCtx->m_filter_pipeline_observer->on_workspace_free(mPlaneBytes[c]);
          }
        }
      }
    }
    return false;
  }

  return true;
}


Slot* InplaceFilterPipeline::acquire_slot_locked(int row)
{
  for (Slot& s : mSlots) {
    if (!s.inUse) {
      s.inUse = true;
      s.row = row;
      const int id = (int)(&s - &mSlots[0]);
      mSlotOfRow[row] = id;
      if (mCtx->m_filter_pipeline_observer) {
        mCtx->m_filter_pipeline_observer->on_slot_acquire(row, id);
      }
      return &s;
    }
  }
  return nullptr;
}


void InplaceFilterPipeline::release_slot_locked(Slot* slot)
{
  const int id = (int)(slot - &mSlots[0]);
  if (mCtx->m_filter_pipeline_observer) {
    mCtx->m_filter_pipeline_observer->on_slot_release(slot->row, id);
  }
  mSlotOfRow[slot->row] = -1;
  slot->inUse = false;
  slot->row = -1;
}


bool InplaceFilterPipeline::take_ready_locked(Unit& out)
{
  if (mReady.empty()) {
    return false;
  }
  out = mReady.back();
  mReady.pop_back();
  return true;
}


// Move every not-yet-scheduled unit whose dependencies are satisfied into the
// ready queue. Snapshot units additionally require a free workspace slot so
// that a worker never has to wait for pixel memory.
void InplaceFilterPipeline::publish_locked()
{
  auto scheduled = [&](UnitKind k, int r) -> uint8_t& {
    return mScheduled[(size_t)r * 4 + (int)k];
  };

  for (int r=0;r<mRows;r++) {
    if (mDoDeblock) {
      if (!scheduled(UNIT_V, r)) {
        scheduled(UNIT_V, r) = 1;
        mReady.push_back(Unit{UNIT_V, r});
      }

      if (!scheduled(UNIT_H, r) && mVDone[r] &&
          (r==0        || mVDone[r-1]) &&
          (r==mRows-1  || mVDone[r+1])) {
        scheduled(UNIT_H, r) = 1;
        mReady.push_back(Unit{UNIT_H, r});
      }
    }

    if (mDoSAO) {
      // The snapshot window covers [r*Hc-1, (r+1)*Hc+1). The top edge row is
      // also a write target of the previous row's horizontal pass (chroma
      // q0 reaches one row down, luma up to two rows down), so like the
      // legacy SAO task we wait for the horizontal pass of rows r-1, r and
      // r+1, not just the two local ones.
      if (!scheduled(UNIT_SNAP, r) &&
          (!mDoDeblock ||
           (mHDone[r] &&
            (r==0       || mHDone[r-1]) &&
            (r==mRows-1 || mHDone[r+1]))) &&
          acquire_slot_locked(r) != nullptr) {
        scheduled(UNIT_SNAP, r) = 1;
        mReady.push_back(Unit{UNIT_SNAP, r});
      }

      // A(r) overwrites the first block row r*Hc, which is the bottom edge
      // saved by S(r-1), so all three neighbouring windows must exist first.
      if (!scheduled(UNIT_SAO, r) && mSnapDone[r] &&
          (r==0       || mSnapDone[r-1]) &&
          (r==mRows-1 || mSnapDone[r+1])) {
        scheduled(UNIT_SAO, r) = 1;
        mReady.push_back(Unit{UNIT_SAO, r});
      }
    }
  }
}


void InplaceFilterPipeline::run()
{
  mTotalUnits = (mDoDeblock ? mRows * 2 : 0) + (mDoSAO ? mRows * 2 : 0);
  mImg->thread_start(mTotalUnits);

  std::unique_lock<std::mutex> lock(mMutex);
  while (mFinished < mTotalUnits) {
    publish_locked();

    std::vector<Unit> toEnqueue;
    Unit u;
    while (take_ready_locked(u)) {
      toEnqueue.push_back(u);
    }

    if (toEnqueue.empty()) {
      // No runnable unit and work is still in flight: wait for a worker to
      // finish a unit and publish new progress. The coordinator never polls.
      mCond.wait(lock);
      continue;
    }

    lock.unlock();
    for (const Unit& unit : toEnqueue) {
      PipelineUnitTask* task = new PipelineUnitTask(this, unit);
      mImgunit->tasks.push_back(task);
      mCtx->thread_pool_.add_task(task);
    }
    lock.lock();
  }
}


void InplaceFilterPipeline::worker_execute(const Unit& u)
{
  mImg->thread_run(nullptr);

  switch (u.kind) {
  case UNIT_V:
  case UNIT_H:
    run_deblock_unit(u);
    break;
  case UNIT_SNAP:
    run_snapshot(u.row);
    break;
  case UNIT_SAO:
    run_sao(u.row);
    break;
  }

  // Finish the image's thread accounting FIRST. This is the worker's last
  // access to mImg, and it is the same gate the legacy wait_for_completion()
  // relies on. Only afterwards may we tell the coordinator that the unit is
  // done: otherwise the coordinator could observe total completion, return
  // from run() and let the DPB free the image while this worker is still
  // inside thread_finishes() locking mImg->mutex (use-after-free).
  mImg->thread_finishes(nullptr);

  {
    std::unique_lock<std::mutex> lock(mMutex);
    mFinished++;

    switch (u.kind) {
    case UNIT_V:    mVDone[u.row] = 1; break;
    case UNIT_H:    mHDone[u.row] = 1; break;
    case UNIT_SNAP: mSnapDone[u.row] = 1; break;
    case UNIT_SAO:  break;
    }

    mCond.notify_all();
  }
}


void InplaceFilterPipeline::run_deblock_unit(const Unit& u)
{
  const bool vertical = (u.kind == UNIT_V);
  const int r = u.row;

  if (!vertical && mCtx->m_filter_pipeline_observer) {
    mCtx->m_filter_pipeline_observer->on_deblock_h_enter(r);
  }

  const int ctbSize  = mSps->CtbSizeY;
  const int deblkSize = ctbSize / 4;
  const int xEnd = mImg->get_deblk_width();

  int first = r * deblkSize;
  int last  = (r+1) * deblkSize;
  if (last > mImg->get_deblk_height()) {
    last = mImg->get_deblk_height();
  }

  bool deblocking_enabled;
  if (vertical) {
    deblocking_enabled = derive_edgeFlags_CTBRow(mImg, (uint16_t)r);
    mImg->set_CtbDeblockFlag(0, r, deblocking_enabled);
  }
  else {
    deblocking_enabled = mImg->get_CtbDeblockFlag(0, r);
  }

  if (deblocking_enabled) {
    derive_boundaryStrength(mImg, vertical, first, last, 0, xEnd);
    edge_filtering_luma(mImg, vertical, first, last, 0, xEnd);

    if (mSps->ChromaArrayType != CHROMA_MONO) {
      edge_filtering_chroma(mImg, vertical, first, last, 0, xEnd);
    }
  }

  if (!vertical) {
    const int ctbWidth = mSps->PicWidthInCtbsY;
    for (int x=0;x<ctbWidth;x++) {
      mImg->ctb_progress[x + r*ctbWidth].set_progress(CTB_PROGRESS_DEBLK_H);
    }
  }
  else {
    const int ctbWidth = mSps->PicWidthInCtbsY;
    for (int x=0;x<ctbWidth;x++) {
      mImg->ctb_progress[x + r*ctbWidth].set_progress(CTB_PROGRESS_DEBLK_V);
    }
  }
}


void InplaceFilterPipeline::run_snapshot(int row)
{
  // Supported geometries always carry all three planes (4:2:0).
  Slot* slot;
  {
    std::unique_lock<std::mutex> lock(mMutex);
    slot = &mSlots[mSlotOfRow[row]];
  }

  for (int c=0;c<3;c++) {
    const int hc = mUnitRows[c];
    const int planeH = mImg->get_height(c);
    const int origin = row * hc - 1;                 // image row at workspace row 0
    const int firstSrc = std::max(0, origin);
    const int endSrc   = std::min(planeH, (row+1)*hc + 1);

    const size_t bps = (size_t)mBytesPerSample[c];
    const size_t lineBytes = (size_t)mStride[c] * bps;
    uint8_t* dst = slot->plane[c];
    const uint8_t* src = mImg->get_image_plane(c);

    for (int y=firstSrc;y<endSrc;y++) {
      memcpy(dst + (size_t)(y - origin) * lineBytes,
             src + (size_t)y * lineBytes,
             lineBytes);
    }
  }
}


void InplaceFilterPipeline::run_sao(int row)
{
  if (mCtx->m_filter_pipeline_observer) {
    mCtx->m_filter_pipeline_observer->on_sao_enter(row);
  }

  const int ctbSize = mSps->CtbSizeY;
  const int ctbWidth = mSps->PicWidthInCtbsY;

  int slotIdx;
  {
    std::unique_lock<std::mutex> lock(mMutex);
    slotIdx = mSlotOfRow[row];
  }
  Slot& slot = mSlots[slotIdx];

  for (int xCtb=0; xCtb<ctbWidth; xCtb++) {
    const slice_segment_header* shdr = mImg->get_SliceHeaderCtb(xCtb, row);
    if (shdr == nullptr) {
      break;
    }

    if (shdr->slice_sao_luma_flag) {
      const int origin = row * mUnitRows[0] - 1;
      apply_sao_window(mImg, xCtb, row, shdr, 0, ctbSize, ctbSize,
                       slot.plane[0], mStride[0],
                       mImg->get_image_plane(0), mImg->get_image_stride(0),
                       origin);
    }

    if (shdr->slice_sao_chroma_flag) {
      const int nSW = ctbSize / mSps->SubWidthC;
      const int nSH = ctbSize / mSps->SubHeightC;

      for (int c=1;c<=2;c++) {
        const int origin = row * mUnitRows[c] - 1;
        apply_sao_window(mImg, xCtb, row, shdr, c, nSW, nSH,
                         slot.plane[c], mStride[c],
                         mImg->get_image_plane(c), mImg->get_image_stride(c),
                         origin);
      }
    }
  }

  for (int x=0;x<ctbWidth;x++) {
    mImg->ctb_progress[x + row*ctbWidth].set_progress(CTB_PROGRESS_SAO);
  }

  {
    std::unique_lock<std::mutex> lock(mMutex);
    release_slot_locked(&slot);
    if (mCtx->m_filter_pipeline_observer) {
      mCtx->m_filter_pipeline_observer->on_sao_finish(row);
    }
    mCond.notify_all();
  }
}


bool inplace_filter_pipeline_supported(const de265_image* img)
{
  if (!img->has_sps()) {
    return false;
  }

  const seq_parameter_set& sps = img->get_sps();

  if (sps.ChromaArrayType != CHROMA_420) {
    return false;
  }
  if (sps.separate_colour_plane_flag) {
    return false;
  }
  if (sps.BitDepth_Y != 8 && sps.BitDepth_Y != 10 && sps.BitDepth_Y != 12) {
    return false;
  }
  if (sps.BitDepth_C != 8 && sps.BitDepth_C != 10 && sps.BitDepth_C != 12) {
    return false;
  }
  if (sps.pic_width_in_luma_samples <= 0 ||
      sps.pic_height_in_luma_samples <= 0 ||
      (sps.pic_width_in_luma_samples & 1) ||
      (sps.pic_height_in_luma_samples & 1)) {
    return false;
  }
  if (sps.PicHeightInCtbsY <= 0 || sps.PicWidthInCtbsY <= 0) {
    return false;
  }

  return true;
}


bool run_inplace_filter_pipeline(decoder_context* ctx, image_unit* imgunit)
{
  if (!inplace_filter_pipeline_supported(imgunit->img)) {
    return false;
  }

  InplaceFilterPipeline pipeline(ctx, imgunit, ctx->get_num_worker_threads());

  // SAO disabled (globally or in the SPS): no pixel workspace is needed.
  const bool needWorkspace =
      !ctx->param_disable_sao &&
      imgunit->img->get_sps().sample_adaptive_offset_enabled_flag;

  if (needWorkspace) {
    if (!pipeline.allocate()) {
      ctx->add_warning(DE265_WARNING_CANNOT_APPLY_SAO_OUT_OF_MEMORY, false);
      return false;
    }
  }

  pipeline.run();
  return true;
}


void filter_pipeline_set_observer(decoder_context* ctx,
                                  filter_pipeline_observer* observer)
{
  ctx->m_filter_pipeline_observer = observer;
}
