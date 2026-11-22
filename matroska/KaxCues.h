/****************************************************************************
** libmatroska : parse Matroska files, see http://www.matroska.org/
**
** <file/class description>
**
** Copyright (C) 2002-2010 Steve Lhomme.  All rights reserved.
**
** This file is part of libmatroska.
**
** This library is free software; you can redistribute it and/or
** modify it under the terms of the GNU Lesser General Public
** License as published by the Free Software Foundation; either
** version 2.1 of the License, or (at your option) any later version.
**
** This library is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
** Lesser General Public License for more details.
**
** You should have received a copy of the GNU Lesser General Public
** License along with this library; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
**
** See http://www.gnu.org/licenses/lgpl-2.1.html for LGPL licensing information.**
** Contact license@matroska.org if any conditions of this licensing are
** not clear to you.
**
**********************************************************************/

/*!
  \file
  \version \$Id: KaxCues.h,v 1.7 2004/04/14 23:26:17 robux4 Exp $
  \author Steve Lhomme     <robux4 @ users.sf.net>
*/
#ifndef LIBMATROSKA_CUES_H
#define LIBMATROSKA_CUES_H

#include <cstddef>
#include <vector>

#include "matroska/KaxTypes.h"
#include "ebml/EbmlMaster.h"
#include "matroska/KaxBlock.h"

using namespace libebml;

namespace libmatroska {

class KaxCuePoint;
class KaxCueTrackPositions;
class KaxCluster;
class KaxCodecState;
class KaxSegment;

DECLARE_MKX_MASTER(KaxCues)
  public:
    ~KaxCues() override;

    //bool AddBlockGroup(const KaxBlockGroup & BlockReference); // deprecated
    bool AddBlockBlob(const KaxBlockBlob & BlockReference);

    /*!
      \brief Indicate that the position for this Block is set
    */
    void PositionSet(const KaxBlockGroup & BlockReference);
    void PositionSet(const KaxBlockBlob & BlockReference);

    /*!
      \brief override to sort by timecode/track
    */
    filepos_t Render(IOCallback & output, bool bSaveDefault = false) {
      Sort();
      return EbmlMaster::Render(output, bSaveDefault);
    }

    uint64 GetTimecodePosition(uint64 aTimecode) const;
    const KaxCuePoint * GetTimecodePoint(uint64 aTimecode) const;

    /*!
      \brief Find the track positions to decode from for the given track at the
             given timecode (expressed in nanoseconds).
      \note Only the CuePoint elements currently held by this element are used.
            The selected point is the latest one not after the requested
            timecode; points with the same timecode are ordered by their
            physical position in the file. Returns nullptr when the track has
            no cue point at or before the requested timecode.
    */
    const KaxCueTrackPositions * GetDecodePosition(uint64 timecode, uint64 track) const;

    /*!
      \brief Enable or disable tracking of the KaxCodecState elements so that
             each generated cue entry references the codec state in effect for
             its block, even when the state changed in a BlockGroup that is
             not itself referenced by a cue.
      \note This can only be changed before the first AddBlockBlob() call and
            before any KaxCluster::Render() feeding this element. When called
            too late the previous mode is kept and false is returned.
    */
    bool SetCodecStateTracking(bool enabled);
    bool CodecStateTracking() const { return bCodecStateTracking; }

    /*!
      \brief Commit the blocks of a successfully rendered Cluster and refresh
             the tracked cue entries (internal use, codec state tracking only)
    */
    void CommitCluster(const KaxCluster & Cluster);

    void SetGlobalTimecodeScale(uint64 aGlobalTimecodeScale) {
      mGlobalTimecodeScale = aGlobalTimecodeScale;
      bGlobalTimecodeScaleIsSet = true;
    }
    uint64 GlobalTimecodeScale() const {
      assert(bGlobalTimecodeScaleIsSet);
      return mGlobalTimecodeScale;
    }

  protected:
    struct TrackedBlockEvent {
      uint64 Track;
      uint64 GlobalTimecode;
      uint64 ClusterPosition;
      uint32 BlockNumber;
      const KaxCodecState *CodecState;
      const KaxSegment *Segment;
      size_t EntryIndex;
    };

    struct TrackedCluster {
      const KaxCluster *Cluster;
      uint64 ClusterPosition;
      std::vector<TrackedBlockEvent> Events;
    };

    struct TrackedEntry {
      const KaxBlockBlob *Blob;
      KaxCuePoint *Point{nullptr};
      KaxCueTrackPositions *Positions{nullptr};
    };

    static constexpr size_t NoTrackedEntry = ~static_cast<size_t>(0);

    size_t MatchTrackedEntry(const KaxBlockGroup *Group, const KaxSimpleBlock *Simple,
                             uint64 Track, uint64 GlobalTimecode,
                             const std::vector<bool> &Matched) const;
    void RegenerateTrackedCues();

    std::vector<const KaxBlockBlob *> myTempReferences;
    std::vector<TrackedEntry> mTrackedEntries;
    std::vector<TrackedCluster> mTrackedClusters;
    bool   bGlobalTimecodeScaleIsSet;
    uint64 mGlobalTimecodeScale;
    bool   bCodecStateTracking{false};
    bool   bCueingStarted{false};
};

} // namespace libmatroska

#endif // LIBMATROSKA_CUES_H
