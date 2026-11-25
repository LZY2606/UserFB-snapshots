/*
 * The contents of this file are subject to the Mozilla Public
 * License Version 1.1 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of
 * the License at http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code is MPEG4IP.
 *
 * The Initial Developer of the Original Code is Cisco Systems Inc.
 * Portions created by Cisco Systems Inc. are
 * Copyright (C) Cisco Systems Inc. 2001.  All Rights Reserved.
 */

#ifndef MP4V2_IMPL_HINTED_TRACK_H
#define MP4V2_IMPL_HINTED_TRACK_H

namespace mp4v2 { namespace impl {

// Internal implementation behind the public MP4CopyHintedTrack() API.
// Copies the RTP hint track srcHintTrackId and its single referenced media
// track from srcFile into dstFile, driven by the media track's edit list.
// On success *pMediaTrackId and *pHintTrackId receive the new track ids;
// on failure both are left as MP4_INVALID_TRACK_ID and any partially
// created destination tracks are deleted.
bool CopyHintedTrack(
    MP4FileHandle srcFile,
    MP4TrackId    srcHintTrackId,
    MP4FileHandle dstFile,
    MP4TrackId*   pMediaTrackId,
    MP4TrackId*   pHintTrackId );

}} // namespace mp4v2::impl

#endif // MP4V2_IMPL_HINTED_TRACK_H
