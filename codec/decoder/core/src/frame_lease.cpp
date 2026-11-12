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
 * \file    frame_lease.cpp
 *
 * \brief   Reference-counted leases over decoder picture storage.
 *
 * Ownership model
 * ---------------
 * Two object kinds cooperate:
 *  - TagWelsDecodedFrame is the public handle. It has a local reference count
 *    (WelsRetain/Release) and stays valid until the matching number of
 *    releases drops that count to zero. Releasing a different handle of the
 *    same picture can never invalidate a handle that is still referenced.
 *  - SWelsSharedLease is one per leased picture and is shared by every handle
 *    of that picture. It owns the picture once the pool is torn down and frees
 *    the storage only when its last handle goes away.
 *
 * Each live handle holds exactly one pin on the picture's pool slot
 * (SPicture::iLeaseRefCount). PrefetchPic() refuses pinned slots, so leased
 * storage is never recycled as a reconstruction target; Retain only extends an
 * existing handle and therefore does not add another pin.
 *
 * On pool teardown (DPB eviction, resolution switch, Uninitialize,
 * WelsDestroyDecoder) every still-live shared lease is detached: it takes
 * ownership of the SPicture and its registry back-link is cleared. The final
 * release of its last handle then frees the picture with
 * FreeDetachedPicture(), which only uses the stateless process-global aligned
 * free and therefore works after the decoder context and its CMemoryAlign
 * object are gone. No handle ever calls back into the decoder.
 *
 * All lease state is serialized by one process-global lock that is never
 * destroyed, so Retain/Release stay valid after WelsDestroyDecoder().
 *
 *************************************************************************************
 */
#include "frame_lease.h"

#include "memory_align.h"
#include "pic_queue.h"
#include "WelsThreadLib.h"

namespace WelsDec {

// One shared lease per leased picture; referenced by every public handle of
// that picture. Defined before the handle so both can be allocated here.
struct SWelsSharedLease {
  SWelsSharedLease*           pNext;         // intrusive registry list link
  CDecodedFrameRegistry*      pRegistry;     // non-NULL while linked
  int32_t                     iHandleCount;  // live public handles
  PPicture                    pPic;          // pinned slot, or owned after detach
  bool                        bOwnedByLease; // pool gone; last handle frees pPic
  SWelsDecodedFrameInfo       sInfo;         // snapshot of the delivered output
};

// Process-global lock for every registry's lease list and every handle
// refcount. It intentionally lives for the whole process so handles can be
// retained or released after their decoder has been destroyed.
WELS_MUTEX* GlobalLeaseLock() {
  static WELS_MUTEX s_csLease;
  static const bool s_bInitialized = (WelsMutexInit (&s_csLease) == 0);
  (void)s_bInitialized;
  return &s_csLease;
}

} // namespace WelsDec

// Public handle, allocated/freed only inside this module. The shared lease
// outlives all of its handles, so pShared is valid for the handle's lifetime.
struct TagWelsDecodedFrame {
  WelsDec::SWelsSharedLease* pShared;
  int32_t                    iLocalRefCount;
};

namespace WelsDec {

CDecodedFrameRegistry::CDecodedFrameRegistry()
  : m_pLeaseList (NULL) {
}

CDecodedFrameRegistry::~CDecodedFrameRegistry() {
  // Pool teardown detaches every lease before the registry dies. Detach
  // anything still linked defensively; afterwards each lease is self-contained
  // and its handles keep working without this registry.
  WelsMutexLock (GlobalLeaseLock());
  SWelsSharedLease* pLease = m_pLeaseList;
  while (pLease != NULL) {
    SWelsSharedLease* pNext = pLease->pNext;
    pLease->bOwnedByLease = true;
    pLease->pRegistry = NULL;
    pLease = pNext;
  }
  m_pLeaseList = NULL;
  WelsMutexUnlock (GlobalLeaseLock());
}

static SWelsSharedLease* FindLeaseLocked (SWelsSharedLease* pLeaseList, PPicture pPic) {
  SWelsSharedLease* pLease = pLeaseList;
  while (pLease != NULL) {
    if (pLease->pPic == pPic)
      return pLease;
    pLease = pLease->pNext;
  }
  return NULL;
}

::TagWelsDecodedFrame* CDecodedFrameRegistry::Acquire (PPicture pPic, const SWelsDecodedFrameInfo& rsInfo) {
  if (pPic == NULL)
    return NULL;

  WelsMutexLock (GlobalLeaseLock());
  SWelsSharedLease* pLease = FindLeaseLocked (m_pLeaseList, pPic);
  if (pLease == NULL) {
    pLease = static_cast<SWelsSharedLease*> (WelsMallocz (sizeof (SWelsSharedLease), "SWelsSharedLease"));
    if (pLease == NULL) {
      WelsMutexUnlock (GlobalLeaseLock());
      return NULL;
    }
    pLease->pNext = m_pLeaseList;
    pLease->pRegistry = this;
    pLease->iHandleCount = 0;
    pLease->pPic = pPic;
    pLease->bOwnedByLease = false;
    pLease->sInfo = rsInfo;
    m_pLeaseList = pLease;
  }
  ::TagWelsDecodedFrame* pFrame =
    static_cast< ::TagWelsDecodedFrame*> (WelsMallocz (sizeof (::TagWelsDecodedFrame), "SWelsDecodedFrame"));
  if (pFrame == NULL) {
    // Remove a freshly created, unreferenced lease before bailing out.
    if (pLease->iHandleCount == 0) {
      SWelsSharedLease** ppLink = &m_pLeaseList;
      while (*ppLink != NULL && *ppLink != pLease)
        ppLink = & (*ppLink)->pNext;
      if (*ppLink == pLease)
        *ppLink = pLease->pNext;
      WelsFree (pLease, "SWelsSharedLease");
    }
    WelsMutexUnlock (GlobalLeaseLock());
    return NULL;
  }
  pFrame->pShared = pLease;
  pFrame->iLocalRefCount = 1;
  ++pLease->iHandleCount;
  // One pool pin per live handle; keeps PrefetchPic() away from this slot.
  LeasePoolPicture (pPic);
  WelsMutexUnlock (GlobalLeaseLock());
  return pFrame;
}

void CDecodedFrameRegistry::DetachLeasedPicture (PPicture pPic) {
  if (pPic == NULL)
    return;
  WelsMutexLock (GlobalLeaseLock());
  SWelsSharedLease** ppLink = &m_pLeaseList;
  while (*ppLink != NULL) {
    SWelsSharedLease* pLease = *ppLink;
    if (pLease->pPic == pPic) {
      // The pool no longer frees this slot; the lease owns it until its last
      // handle is released, and must not touch this registry afterwards.
      pLease->bOwnedByLease = true;
      pLease->pRegistry = NULL;
      *ppLink = pLease->pNext;
      break;
    }
    ppLink = &pLease->pNext;
  }
  WelsMutexUnlock (GlobalLeaseLock());
}

void CDecodedFrameRegistry::DetachLeasedPictures (PPicBuff pPicBuff) {
  if (pPicBuff == NULL)
    return;
  for (int32_t i = 0; i < pPicBuff->iCapacity; ++i) {
    PPicture pPic = pPicBuff->ppPic[i];
    if (pPic != NULL && pPic->iLeaseRefCount > 0)
      DetachLeasedPicture (pPic);
  }
}

int CDecodedFrameRegistry::Retain (::TagWelsDecodedFrame* pFrame) {
  if (pFrame == NULL || pFrame->pShared == NULL)
    return 1;
  WelsMutexLock (GlobalLeaseLock());
  ++pFrame->iLocalRefCount;
  WelsMutexUnlock (GlobalLeaseLock());
  return 0;
}

void CDecodedFrameRegistry::Release (::TagWelsDecodedFrame* pFrame) {
  if (pFrame == NULL || pFrame->pShared == NULL)
    return;

  WelsMutexLock (GlobalLeaseLock());
  if (pFrame->iLocalRefCount > 0)
    --pFrame->iLocalRefCount;
  if (pFrame->iLocalRefCount > 0) {
    // Still referenced: the handle, its lease and the pool pin all stay.
    WelsMutexUnlock (GlobalLeaseLock());
    return;
  }

  SWelsSharedLease* pLease = pFrame->pShared;
  PPicture pPicToFree = NULL;
  if (pLease->iHandleCount > 0)
    --pLease->iHandleCount;
  UnleasePoolPicture (pLease->pPic); // one pin per dying handle

  if (pLease->iHandleCount == 0) {
    if (pLease->pRegistry != NULL) {
      // Pool still alive: unlink; the pool recycles the slot normally.
      CDecodedFrameRegistry* pRegistry = pLease->pRegistry;
      SWelsSharedLease** ppLink = &pRegistry->m_pLeaseList;
      while (*ppLink != NULL && *ppLink != pLease)
        ppLink = & (*ppLink)->pNext;
      if (*ppLink == pLease)
        *ppLink = pLease->pNext;
      pLease->pRegistry = NULL;
    } else if (pLease->bOwnedByLease) {
      // The pool is gone; this was the last handle for the picture.
      pPicToFree = pLease->pPic;
    }
  }
  WelsMutexUnlock (GlobalLeaseLock());

  WelsFree (pFrame, "SWelsDecodedFrame");
  if (pLease->iHandleCount == 0)
    WelsFree (pLease, "SWelsSharedLease");
  // Free outside the lock: it only touches process-global allocation state.
  if (pPicToFree != NULL)
    FreeDetachedPicture (pPicToFree);
}

int CDecodedFrameRegistry::GetInfo (const ::TagWelsDecodedFrame* pFrame, SWelsDecodedFrameInfo* pInfo) {
  if (pFrame == NULL || pFrame->pShared == NULL || pInfo == NULL)
    return 1;
  WelsMutexLock (GlobalLeaseLock());
  *pInfo = pFrame->pShared->sInfo;
  WelsMutexUnlock (GlobalLeaseLock());
  return 0;
}

void LeasePoolPicture (PPicture pPic) {
  if (pPic != NULL)
    ++pPic->iLeaseRefCount;
}

void UnleasePoolPicture (PPicture pPic) {
  if (pPic != NULL && pPic->iLeaseRefCount > 0)
    --pPic->iLeaseRefCount;
}

} // namespace WelsDec
