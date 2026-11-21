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
 * The Original Code is MP4v2.
 */

#ifndef MP4V2_IMPL_MP4HINTEDCOPY_H
#define MP4V2_IMPL_MP4HINTEDCOPY_H

namespace mp4v2 {
namespace impl {

class MP4File;

// Reorders the media samples of the hinted pair according to the media
// track edit list, building an independently playable media/hint track
// pair in dstFile. Throws Exception on malformed input; returns false on
// operational failures. On failure no tracks are left in dstFile and
// both output ids are set to MP4_INVALID_TRACK_ID.
bool CopyHintedTrack(
    MP4File*     srcFile,
    MP4TrackId   srcHintTrackId,
    MP4File*     dstFile,
    MP4TrackId*  pDstMediaTrackId,
    MP4TrackId*  pDstHintTrackId );

} // namespace mp4v2::impl
} // namespace mp4v2

#endif // MP4V2_IMPL_MP4HINTEDCOPY_H
