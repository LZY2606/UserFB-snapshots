/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*------------------------------------------------------------------------------

    Table of contents

     1. Include headers
     2. Module defines
     3. Local function prototypes
     4. Functions
          h264bsdRecoveryBegin
          h264bsdRecoveryParseSei
          h264bsdRecoveryPictureDone
          h264bsdRecoveryGetState

------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "h264bsd_recovery.h"
#include "h264bsd_util.h"
#include "h264bsd_vlc.h"
#include "h264bsd_nal_unit.h"

/*------------------------------------------------------------------------------
    2. Module defines
------------------------------------------------------------------------------*/

#define SEI_PAYLOAD_RECOVERY_POINT 6u

/*------------------------------------------------------------------------------
    3. Local function prototypes
------------------------------------------------------------------------------*/

static u32 ParseRecoveryPoint(strmData_t *pStrm, u32 payloadSize,
    u32 *pFrameCnt, u32 *pExactMatch, u32 *pChangingSliceGroup);

/*------------------------------------------------------------------------------

    Function: h264bsdRecoveryBegin

        Functional description:
            Arm a recovery session. Requires activated parameter sets and a
            finished picture. All pictures currently queued for display lose
            their output eligibility; reference pictures, SPS and PPS are
            kept untouched.

        Inputs:
            pStorage    pointer to storage data structure

        Returns:
            RECOVERY_BEGIN_OK
            RECOVERY_BEGIN_NO_HEADERS   no active SPS/PPS yet
            RECOVERY_BEGIN_PICTURE_BUSY picture decoding is half way

------------------------------------------------------------------------------*/

u32 h264bsdRecoveryBegin(storage_t *pStorage)
{

/* Variables */

/* Code */

    ASSERT(pStorage);

    if (pStorage->activeSps == NULL || pStorage->activePps == NULL)
        return(RECOVERY_BEGIN_NO_HEADERS);

    /* a slice of the current picture has been decoded but the picture is
     * not finished yet */
    if (pStorage->picStarted)
        return(RECOVERY_BEGIN_PICTURE_BUSY);

    pStorage->recoveryState            = H264BSD_RECOVERY_WAIT_MARKER;
    pStorage->recoveryPendingMarker    = HANTRO_FALSE;
    pStorage->recoveryBound            = HANTRO_FALSE;
    pStorage->recoveryFrameCnt         = 0;
    pStorage->recoveryBoundFrameNum    = 0;
    pStorage->recoveryTargetFrameNum   = 0;

    /* revoke every pending output and close the display gate. Pictures keep
     * their reference status, prediction is not affected */
    h264bsdDpbRecoveryBegin(pStorage->dpb);

    return(RECOVERY_BEGIN_OK);

}

/*------------------------------------------------------------------------------

    Function: ParseRecoveryPoint

        Functional description:
            Parse one recovery_point SEI payload. The payload occupies
            payloadSize bytes; bits left after the four syntax elements are
            payload boundary alignment bits (one '1' followed by '0's).

        Returns:
            HANTRO_OK       payload parsed
            HANTRO_NOK      corrupted or truncated payload

------------------------------------------------------------------------------*/

static u32 ParseRecoveryPoint(strmData_t *pStrm, u32 payloadSize,
    u32 *pFrameCnt, u32 *pExactMatch, u32 *pChangingSliceGroup)
{

/* Variables */

    u32 tmp, value;
    u32 bitsAtStart;
    u32 consumedBits;
    u32 alignBits;
    u32 brokenLink;

/* Code */

    ASSERT(pStrm);
    ASSERT(payloadSize);

    bitsAtStart = pStrm->strmBuffReadBits;

    tmp = h264bsdDecodeExpGolombUnsigned(pStrm, &value);
    if (tmp != HANTRO_OK)
        return(HANTRO_NOK);
    *pFrameCnt = value;

    tmp = h264bsdGetBits(pStrm, 1);
    if (tmp == END_OF_STREAM)
        return(HANTRO_NOK);
    *pExactMatch = tmp;

    /* broken_link_flag: both values are accepted, the bit only has to be
     * consumed */
    tmp = h264bsdGetBits(pStrm, 1);
    if (tmp == END_OF_STREAM)
        return(HANTRO_NOK);
    brokenLink = tmp;

    tmp = h264bsdGetBits(pStrm, 2);
    if (tmp == END_OF_STREAM)
        return(HANTRO_NOK);
    *pChangingSliceGroup = tmp;

    consumedBits = pStrm->strmBuffReadBits - bitsAtStart;

    /* syntax elements run past the declared payload boundary */
    if (consumedBits > payloadSize * 8)
        return(HANTRO_NOK);

    /* remainder of the payload is the payload bit alignment */
    alignBits = 0;
    while (consumedBits + alignBits < payloadSize * 8)
    {
        tmp = h264bsdGetBits(pStrm, 1);
        if (tmp == END_OF_STREAM)
            return(HANTRO_NOK);
        if (alignBits == 0)
        {
            if (tmp != 1)
                return(HANTRO_NOK);
        }
        else if (tmp != 0)
        {
            return(HANTRO_NOK);
        }
        alignBits++;
    }

    (void)brokenLink;

    return(HANTRO_OK);

}

/*------------------------------------------------------------------------------

    Function: h264bsdRecoveryParseSei

        Functional description:
            Parse a complete prefix SEI NAL unit. payloadType/payloadSize
            extension bytes are honored, unknown payload types are skipped
            over their declared boundary. Accepted recovery_point markers
            are collected locally and only the last valid one is committed
            once the whole NAL unit is sound; a corrupted or truncated NAL
            returns an error without touching the existing waiting state.
            Markers are ignored unless a recovery session is active and not
            yet completed.

        Inputs:
            pStorage    pointer to storage data structure
            pStrm       stream positioned at the first SEI payload byte

        Returns:
            HANTRO_OK       NAL unit parsed
            HANTRO_NOK      corrupted or truncated NAL unit

------------------------------------------------------------------------------*/

u32 h264bsdRecoveryParseSei(storage_t *pStorage, strmData_t *pStrm)
{

/* Variables */

    u32 payloadType, payloadSize, tmp, i;
    u32 frameCnt, exactMatch, changingSliceGroup;
    u32 markerAccepted = HANTRO_FALSE;
    u32 markerFrameCnt = 0;
    u32 maxFrameNum;
    u32 sessionActive;

/* Code */

    ASSERT(pStorage);
    ASSERT(pStrm);
    ASSERT(pStorage->activeSps != NULL);

    maxFrameNum = pStorage->activeSps->maxFrameNum;
    sessionActive =
        (pStorage->recoveryState == H264BSD_RECOVERY_WAIT_MARKER ||
         pStorage->recoveryState == H264BSD_RECOVERY_WAIT_TARGET) ?
        HANTRO_TRUE : HANTRO_FALSE;

    while (h264bsdMoreRbspData(pStrm) == HANTRO_TRUE)
    {
        payloadType = 0;
        tmp = h264bsdGetBits(pStrm, 8);
        if (tmp == END_OF_STREAM)
            return(HANTRO_NOK);
        while (tmp == 0xFF)
        {
            payloadType += 255;
            tmp = h264bsdGetBits(pStrm, 8);
            if (tmp == END_OF_STREAM)
                return(HANTRO_NOK);
        }
        payloadType += tmp;

        payloadSize = 0;
        tmp = h264bsdGetBits(pStrm, 8);
        if (tmp == END_OF_STREAM)
            return(HANTRO_NOK);
        while (tmp == 0xFF)
        {
            payloadSize += 255;
            tmp = h264bsdGetBits(pStrm, 8);
            if (tmp == END_OF_STREAM)
                return(HANTRO_NOK);
        }
        payloadSize += tmp;

        if (payloadType == SEI_PAYLOAD_RECOVERY_POINT)
        {
            if (ParseRecoveryPoint(pStrm, payloadSize, &frameCnt,
                    &exactMatch, &changingSliceGroup) != HANTRO_OK)
                return(HANTRO_NOK);

            /* only exact match without slice group changes is supported;
             * recovery_frame_cnt outside [0, MaxFrameNum-1] is an
             * unsupported marker and gets ignored */
            if (sessionActive && exactMatch == 1 &&
                changingSliceGroup == 0 && frameCnt < maxFrameNum)
            {
                markerAccepted = HANTRO_TRUE;
                markerFrameCnt = frameCnt;
            }
        }
        else
        {
            /* skip unknown payload over its declared boundary */
            for (i = 0; i < payloadSize; i++)
            {
                if (h264bsdGetBits(pStrm, 8) == END_OF_STREAM)
                    return(HANTRO_NOK);
            }
        }
    }

    if (h264bsdRbspTrailingBits(pStrm) != HANTRO_OK)
        return(HANTRO_NOK);

    /* commit the last accepted marker of this NAL as one atomic step; it
     * rebinds the next primary coded picture. A marker arriving while a
     * previous marker is already bound replaces it and the next primary
     * picture is bound again */
    if (markerAccepted)
    {
        pStorage->recoveryPendingMarker  = HANTRO_TRUE;
        pStorage->recoveryBound          = HANTRO_FALSE;
        pStorage->recoveryFrameCnt       = markerFrameCnt;
        pStorage->recoveryState          = H264BSD_RECOVERY_WAIT_TARGET;
    }

    return(HANTRO_OK);

}

/*------------------------------------------------------------------------------

    Function: h264bsdRecoveryPictureDone

        Functional description:
            Drive the recovery state machine for one completely reconstructed
            primary coded picture. Called once per picture, so additional
            slices and redundant slices of the same picture can never restart
            the binding. The picture has already been reconstructed,
            deblocked and remains fully usable as a reference; only display
            eligibility is decided here.

        Inputs:
            pStorage    pointer to storage data structure

        Returns:
            0                           gate already released or no action
            RECOVERY_RELEASE_AFTER_MARK IDR picture: the gate must stay
                                        closed while the DPB is flushed and
                                        be released only after the IDR
                                        picture has been inserted

------------------------------------------------------------------------------*/

u32 h264bsdRecoveryPictureDone(storage_t *pStorage)
{

/* Variables */

    u32 frameNum;
    u32 maxFrameNum;
    u32 isReference;
    u32 isIdr;

/* Code */

    ASSERT(pStorage);

    if (pStorage->recoveryState != H264BSD_RECOVERY_WAIT_MARKER &&
        pStorage->recoveryState != H264BSD_RECOVERY_WAIT_TARGET)
        return(0);

    if (pStorage->activeSps == NULL)
        return(0);

    frameNum    = pStorage->sliceHeader->frameNum;
    maxFrameNum = pStorage->activeSps->maxFrameNum;
    isReference = pStorage->prevNalUnit->nalRefIdc != 0 ?
                  HANTRO_TRUE : HANTRO_FALSE;
    isIdr       = IS_IDR_NAL_UNIT(pStorage->prevNalUnit) ?
                  HANTRO_TRUE : HANTRO_FALSE;

    /* an IDR picture completes the recovery immediately and revokes any
     * marker still waiting. The DPB flush during reference marking must
     * stay gated, so the gate is released by the caller afterwards */
    if (isIdr)
    {
        pStorage->recoveryPendingMarker = HANTRO_FALSE;
        pStorage->recoveryBound         = HANTRO_FALSE;
        pStorage->recoveryState         = H264BSD_RECOVERY_DONE;
        return(RECOVERY_RELEASE_AFTER_MARK);
    }

    /* bind a pending marker to the first primary coded picture after it */
    if (pStorage->recoveryPendingMarker && !pStorage->recoveryBound)
    {
        pStorage->recoveryBound             = HANTRO_TRUE;
        pStorage->recoveryPendingMarker     = HANTRO_FALSE;
        pStorage->recoveryBoundFrameNum     = frameNum;
        pStorage->recoveryTargetFrameNum    =
            (frameNum + pStorage->recoveryFrameCnt) % maxFrameNum;
        pStorage->recoveryState             = H264BSD_RECOVERY_WAIT_TARGET;
    }

    if (pStorage->recoveryBound &&
        frameNum == pStorage->recoveryTargetFrameNum)
    {
        /* a non-reference picture carrying the target number early must not
         * complete the recovery; the explicitly bound picture itself
         * (recovery_frame_cnt == 0) always qualifies */
        if (isReference || frameNum == pStorage->recoveryBoundFrameNum)
        {
            pStorage->recoveryState         = H264BSD_RECOVERY_DONE;
            /* release before DPB insertion so that the target picture itself
             * gets output eligibility */
            h264bsdDpbRecoveryRelease(pStorage->dpb);
        }
    }

    return(0);

}

/*------------------------------------------------------------------------------

    Function: h264bsdRecoveryGetState

        Functional description:
            Return the current recovery state, one of
            H264BSD_RECOVERY_{NORMAL,WAIT_MARKER,WAIT_TARGET,DONE}.

------------------------------------------------------------------------------*/

u32 h264bsdRecoveryGetState(storage_t *pStorage)
{

    ASSERT(pStorage);

    return(pStorage->recoveryState);

}
