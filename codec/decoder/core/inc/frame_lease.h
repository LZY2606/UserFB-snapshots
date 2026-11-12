/*!
 * \copy
 *     Copyright (c)  2026, Cisco Systems
 *     All rights reserved.
 *
 *     Redistribution and use in source and binary forms, with or without
 *     modification, are permitted provided that the following conditions
 *     are met:
 *
 *        * Redistributions of source code must retain the above copyright
 *          notice, this list of conditions and the following disclaimer.
 *
 *        * Redistributions in binary form must reproduce the above copyright
 *          notice, this list of conditions and the following disclaimer in
 *          the documentation and/or other materials provided with the
 *          distribution.
 *
 *     THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *     "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *     LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *     FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *     COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *     INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *     BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *     LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *     CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *     LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *     ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *     POSSIBILITY OF SUCH DAMAGE.
 *
 *
 * \file    frame_lease.h
 *
 * \brief   Ownership bridge for decoded pictures that callers lease through
 *          WelsAcquireDecodedFrame(). frame_lease.cpp documents the ownership
 *          relationship between the picture pool, the decoder context, the
 *          aligned allocator and the public handles.
 *
 *************************************************************************************
 */
#ifndef WELS_FRAME_LEASE_H__
#define WELS_FRAME_LEASE_H__

#include "codec_def.h"
#include "typedefs.h"
#include "pic_queue.h"

struct TagWelsDecodedFrame;

namespace WelsDec {

// One-per-picture shared lease, defined in frame_lease.cpp.
struct SWelsSharedLease;

/*
 * One registry belongs to a decoder instance. It tracks the pictures of the
 * current picture pool that carry external leases and takes ownership of
 * their storage when the pool is torn down.
 *
 *  - The picture pool (SPicBuff) only recycles slots whose lease count
 *    (SPicture::iLeaseRefCount) is zero, so a leased slot is never selected
 *    as a reconstruction target and never freed out from under a handle.
 *  - On DPB eviction / resolution change / Uninitialize / decoder destruction
 *    the pool hands every still-leased SPicture to the registry instead of
 *    freeing it. A detached lease fully owns the SPicture until its last
 *    external reference is released.
 *  - Each lease carries its own mutex and handle refcount; registry list
 *    bookkeeping uses a process-global lock that outlives every decoder.
 *    Detached storage is freed through the stateless process-global aligned
 *    allocator. Retain/Release therefore need no decoder context and remain
 *    legal after WelsDestroyDecoder(); the last release never calls back
 *    into the decoder.
 */
class CDecodedFrameRegistry {
 public:
  CDecodedFrameRegistry();
  ~CDecodedFrameRegistry();

  // Lease a pool picture and return a new independent public handle.
  // The info snapshot is copied verbatim into every WelsGetDecodedFrameInfo()
  // answer for this picture. Returns NULL on allocation failure.
  ::TagWelsDecodedFrame* Acquire (PPicture pPic, const SWelsDecodedFrameInfo& rsInfo);

  // Pool teardown hooks: detach one picture (DecreasePicBuff drop path) or
  // every still-leased picture of a whole buffer (DestroyPicBuff path).
  void DetachLeasedPicture (PPicture pPic);
  void DetachLeasedPictures (PPicBuff pPicBuff);

  // Public C entry points. Only lease-internal/global locking is taken, so
  // these are safe on threads other than the decoder's calling thread.
  static int Retain (::TagWelsDecodedFrame* pFrame);
  static void Release (::TagWelsDecodedFrame* pFrame);
  static int GetInfo (const ::TagWelsDecodedFrame* pFrame, SWelsDecodedFrameInfo* pInfo);

 private:
  SWelsSharedLease* m_pLeaseList; // intrusive per-picture lease list head
};

// Bump / drop the external-lease portion of a pool picture's refcount.
// A leased picture stays unrecyclable for PrefetchPic() until every lease is
// released, independently of the decoder's own output-buffer refcount.
void LeasePoolPicture (PPicture pPic);
void UnleasePoolPicture (PPicture pPic);

} // namespace WelsDec

#endif // WELS_FRAME_LEASE_H__
