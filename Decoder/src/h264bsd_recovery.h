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

    Recovery point (SEI payload type 6) driven output gating. The module owns
    the recovery state machine of one decoder instance and the parsing of
    prefix recovery_point SEI messages. Decoding, reconstruction, deblocking
    and DPB/reference handling are never skipped; only the display eligibility
    of completed pictures is controlled.

------------------------------------------------------------------------------*/

#ifndef H264BSD_RECOVERY_H
#define H264BSD_RECOVERY_H

#include "basetype.h"
#include "h264bsd_storage.h"
#include "h264bsd_stream.h"

/* recovery states, values are part of the public H264SwDecGetRecoveryState
 * contract and must stay 0..3 in this order */
enum {
    H264BSD_RECOVERY_NORMAL       = 0,
    H264BSD_RECOVERY_WAIT_MARKER  = 1,
    H264BSD_RECOVERY_WAIT_TARGET  = 2,
    H264BSD_RECOVERY_DONE         = 3
};

/* h264bsdRecoveryBegin return values */
enum {
    RECOVERY_BEGIN_OK            = 0,
    RECOVERY_BEGIN_NO_HEADERS    = 1,
    RECOVERY_BEGIN_PICTURE_BUSY  = 2
};

/* h264bsdRecoveryPictureDone return value: release the output gate only
 * after the current picture has been inserted into the DPB (IDR case) */
#define RECOVERY_RELEASE_AFTER_MARK 1u

/* arm a recovery session, see H264SwDecBeginRecovery for the semantics */
u32 h264bsdRecoveryBegin(storage_t *pStorage);

/* parse one complete prefix SEI NAL unit, the stream position points to the
 * first SEI payload byte (NAL header already consumed). Returns HANTRO_OK or
 * HANTRO_NOK for a corrupted/truncated NAL; on failure no state is changed */
u32 h264bsdRecoveryParseSei(storage_t *pStorage, strmData_t *pStrm);

/* update the recovery state machine when a primary coded picture has been
 * completely reconstructed, called just before reference picture marking and
 * DPB insertion of that picture. Returns 0 normally, or
 * RECOVERY_RELEASE_AFTER_MARK when the gate must be released after the DPB
 * insertion of the current (IDR) picture */
u32 h264bsdRecoveryPictureDone(storage_t *pStorage);

/* current recovery state */
u32 h264bsdRecoveryGetState(storage_t *pStorage);

#endif /* H264BSD_RECOVERY_H */
