/****************************************************************************
** libmatroska : parse Matroska files, see http://www.matroska.org/
**
** <file/class description>
**
** Copyright (C) 2002-2010 Steve Lhomme.  All rights reserved.
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
  \version \$Id: KaxCues.cpp 1265 2007-01-14 17:20:35Z mosu $
  \author Steve Lhomme     <robux4 @ users.sf.net>
*/
#include <algorithm>
#include <cassert>
#include <map>

#include "matroska/KaxCues.h"
#include "matroska/KaxCuesData.h"
#include "matroska/KaxContexts.h"
#include "ebml/EbmlStream.h"
#include "matroska/KaxCluster.h"
#include "matroska/KaxSegment.h"
#include "matroska/KaxDefines.h"
#include "matroska/KaxSemantic.h"

// sub elements
namespace libmatroska {

KaxCues::~KaxCues()
{
  assert(myTempReferences.empty()); // otherwise that means you have added references and forgot to set the position
}
/* deprecated and wrong
bool KaxCues::AddBlockGroup(const KaxBlockGroup & BlockRef)
{
  // Do not add the element if it's already present.
  std::vector<const KaxBlockBlob *>::iterator ListIdx;
  KaxBlockBlob *BlockReference = new KaxBlockBlob(BLOCK_BLOB_NO_SIMPLE);
  BlockReference->SetBlockGroup(*const_cast<KaxBlockGroup*>(&BlockRef));

  for (ListIdx = myTempReferences.begin(); ListIdx != myTempReferences.end(); ListIdx++)
    if (&(KaxBlockGroup&)*ListIdx == &BlockRef) {
      delete BlockReference;
      return true;
    }

  myTempReferences.push_back(BlockReference);
  return true;
}
*/
bool KaxCues::AddBlockBlob(const KaxBlockBlob & BlockReference)
{
  bCueingStarted = true;

  // Do not add the element if it's already present.
  const auto& pr = myTempReferences;
  const bool present = std::find(pr.begin(), pr.end(), &BlockReference) != pr.end();
  if (present) {
    return true;
  }

  myTempReferences.push_back(&BlockReference);
  if (bCodecStateTracking) {
    TrackedEntry Entry;
    Entry.Blob = &BlockReference;
    mTrackedEntries.push_back(Entry);
  }
  return true;
}

bool KaxCues::SetCodecStateTracking(bool enabled)
{
  if (enabled == bCodecStateTracking)
    return true;

  if (bCueingStarted)
    return false; // too late: blocks were already registered or clusters already rendered

  bCodecStateTracking = enabled;
  return true;
}

void KaxCues::PositionSet(const KaxBlockBlob & BlockReference)
{
  bCueingStarted = true;

  // look for the element in the temporary references
  const auto& pr = myTempReferences;
  auto it = std::find(pr.begin(), pr.end(), &BlockReference);

  if (it != pr.end()) {
    // found, now add the element to the entry list
    auto & NewPoint = AddNewChild<KaxCuePoint>(*this);
    NewPoint.PositionSet(BlockReference, GlobalTimecodeScale());
    myTempReferences.erase(it);
  }
}

void KaxCues::PositionSet(const KaxBlockGroup & BlockRef)
{
  bCueingStarted = true;

  // look for the element in the temporary references
  auto it = std::find_if(myTempReferences.begin(), myTempReferences.end(),
    [&](const KaxBlockBlob *myTempReference)
      { const KaxInternalBlock &refTmp = *myTempReference;
        return refTmp.GlobalTimecode() == BlockRef.GlobalTimecode()
            && refTmp.TrackNum() == BlockRef.TrackNumber(); });

  if(it != myTempReferences.end()) {
    // found, now add the element to the entry list
    auto & NewPoint = AddNewChild<KaxCuePoint>(*this);
    NewPoint.PositionSet(**it, GlobalTimecodeScale());
    myTempReferences.erase(it);
  }
}

/*!
  \warning Assume that the list has been sorted (Sort())
*/
const KaxCuePoint * KaxCues::GetTimecodePoint(uint64 aTimecode) const
{
  const uint64 TimecodeToLocate = aTimecode / GlobalTimecodeScale();
  const KaxCuePoint * aPointPrev = nullptr;
  uint64 aPrevTime = 0;
  uint64 aNextTime = EBML_PRETTYLONGINT(0xFFFFFFFFFFFF);

  EBML_MASTER_CONST_ITERATOR Itr;
  for (Itr = begin(); Itr != end(); ++Itr) {
    if (EbmlId(*(*Itr)) == EBML_ID(KaxCuePoint)) {
      auto tmp = static_cast<const KaxCuePoint *>(*Itr);
      // check the tile
      auto aTime = static_cast<const KaxCueTime *>(tmp->FindFirstElt(EBML_INFO(KaxCueTime)));
      if (aTime != nullptr) {
        auto _Time = static_cast<uint64>(*aTime);
        if (_Time > aPrevTime && _Time < TimecodeToLocate) {
          aPrevTime = _Time;
          aPointPrev = tmp;
        }
        if (_Time < aNextTime && _Time > TimecodeToLocate) {
          aNextTime= _Time;
        }
      }
    }
  }

  return aPointPrev;
}

uint64 KaxCues::GetTimecodePosition(uint64 aTimecode) const
{
  const KaxCuePoint * const aPoint = GetTimecodePoint(aTimecode);
  if (aPoint == nullptr)
    return 0;

  const KaxCueTrackPositions * const aTrack = aPoint->GetSeekPosition();
  if (aTrack == nullptr)
    return 0;

  return aTrack->ClusterPosition();
}

const KaxCueTrackPositions * KaxCues::GetDecodePosition(uint64 timecode, uint64 track) const
{
  const KaxCueTrackPositions * Best = nullptr;
  uint64 BestTime = 0;
  uint64 BestCluster = 0;
  uint64 BestBlock = 0;

  for (const auto Child : *this) {
    if (EbmlId(*Child) != EBML_ID(KaxCuePoint))
      continue;

    const auto & Point = *static_cast<const KaxCuePoint *>(Child);
    const auto Time = static_cast<const KaxCueTime *>(Point.FindFirstElt(EBML_INFO(KaxCueTime)));
    if (Time == nullptr)
      continue;

    const uint64 PointTime = static_cast<uint64>(*Time) * GlobalTimecodeScale();
    if (PointTime > timecode)
      continue;

    auto Positions = static_cast<const KaxCueTrackPositions *>(Point.FindFirstElt(EBML_INFO(KaxCueTrackPositions)));
    while (Positions != nullptr) {
      if (Positions->TrackNumber() == track) {
        const uint64 ClusterPos = Positions->ClusterPosition();
        uint64 BlockNum = 0;
        const auto BlockElt = static_cast<const KaxCueBlockNumber *>(Positions->FindFirstElt(EBML_INFO(KaxCueBlockNumber)));
        if (BlockElt != nullptr)
          BlockNum = static_cast<uint64>(*BlockElt);

        // latest point not after the requested timecode; for identical
        // timecodes the physically earlier block wins
        if (Best == nullptr || PointTime > BestTime
            || (PointTime == BestTime && (ClusterPos < BestCluster
                || (ClusterPos == BestCluster && BlockNum < BestBlock)))) {
          Best = Positions;
          BestTime = PointTime;
          BestCluster = ClusterPos;
          BestBlock = BlockNum;
        }
      }
      Positions = static_cast<const KaxCueTrackPositions *>(Point.FindNextElt(*Positions));
    }
  }

  return Best;
}

size_t KaxCues::MatchTrackedEntry(const KaxBlockGroup *Group, const KaxSimpleBlock *Simple,
                                  uint64 Track, uint64 GlobalTimecode,
                                  const std::vector<bool> &Matched) const
{
  size_t Fallback = NoTrackedEntry;

  for (size_t i = 0; i < mTrackedEntries.size(); ++i) {
    if (Matched[i])
      continue;

    const KaxBlockBlob & Blob = *mTrackedEntries[i].Blob;
    if (!Blob.HasBlock())
      continue; // registered but not filled yet, it cannot have been rendered

    if (Group != nullptr && !Blob.IsSimpleBlock()
        && &static_cast<const KaxBlockGroup &>(Blob) == Group)
      return i;

    if (Simple != nullptr && Blob.IsSimpleBlock()
        && &static_cast<KaxSimpleBlock &>(const_cast<KaxBlockBlob &>(Blob)) == Simple)
      return i;

    // legacy matching by track and timecode
    if (Fallback == NoTrackedEntry) {
      const KaxInternalBlock & Block = Blob;
      if (Block.TrackNum() == Track && Block.GlobalTimecode() == GlobalTimecode)
        Fallback = i;
    }
  }

  return Fallback;
}

/*!
  \note called by KaxCluster::Render() only after the Cluster has been
        completely written, so a failed render never reaches this point and
        the previously committed state is preserved
*/
void KaxCues::CommitCluster(const KaxCluster & Cluster)
{
  bCueingStarted = true;

  TrackedCluster Record;
  Record.Cluster = &Cluster;
  Record.ClusterPosition = Cluster.GetPosition();

  uint32 BlockNumber = 0;
  std::vector<bool> Matched(mTrackedEntries.size(), false);

  for (const auto Child : Cluster) {
    const bool bIsGroup = EbmlId(*Child) == EBML_ID(KaxBlockGroup);
    const bool bIsSimple = EbmlId(*Child) == EBML_ID(KaxSimpleBlock);
    if (!bIsGroup && !bIsSimple)
      continue;

    TrackedBlockEvent Event;
    Event.ClusterPosition = Record.ClusterPosition;
    Event.BlockNumber = ++BlockNumber; // laced groups count as a single block
    Event.Segment = Cluster.GetParentSegment();
    Event.CodecState = nullptr;

    const KaxBlockGroup * Group = nullptr;
    const KaxSimpleBlock * Simple = nullptr;
    if (bIsGroup) {
      Group = static_cast<const KaxBlockGroup *>(Child);
      Event.Track = Group->TrackNumber();
      Event.GlobalTimecode = Group->GlobalTimecode();
      Event.CodecState = static_cast<const KaxCodecState *>(Group->FindFirstElt(EBML_INFO(KaxCodecState)));
    } else {
      Simple = static_cast<const KaxSimpleBlock *>(Child);
      Event.Track = Simple->TrackNum();
      Event.GlobalTimecode = Simple->GlobalTimecode();
    }

    Event.EntryIndex = MatchTrackedEntry(Group, Simple, Event.Track, Event.GlobalTimecode, Matched);
    if (Event.EntryIndex != NoTrackedEntry)
      Matched[Event.EntryIndex] = true;

    Record.Events.push_back(Event);
  }

  // a re-rendered cluster replaces its previous commit, positions may have moved
  for (auto It = mTrackedClusters.begin(); It != mTrackedClusters.end(); ++It) {
    if (It->Cluster == &Cluster) {
      mTrackedClusters.erase(It);
      break;
    }
  }
  mTrackedClusters.push_back(std::move(Record));

  RegenerateTrackedCues();
}

void KaxCues::RegenerateTrackedCues()
{
  // the codec state is inherited following the physical order of the blocks
  // in the file, not the order of the cue timecodes
  std::vector<const TrackedCluster *> Ordered;
  Ordered.reserve(mTrackedClusters.size());
  for (const auto & Record : mTrackedClusters)
    Ordered.push_back(&Record);
  std::stable_sort(Ordered.begin(), Ordered.end(),
    [](const TrackedCluster * a, const TrackedCluster * b)
      { return a->ClusterPosition < b->ClusterPosition; });

  struct StateAnchor {
    const KaxCodecState * State;
    const KaxSegment * Segment;
  };
  std::map<uint64, StateAnchor> StateByTrack;

  for (const auto Record : Ordered) {
    for (const auto & Event : Record->Events) {
      // a codec state is effective from its own BlockGroup onwards
      if (Event.CodecState != nullptr)
        StateByTrack[Event.Track] = StateAnchor{Event.CodecState, Event.Segment};

      if (Event.EntryIndex == NoTrackedEntry)
        continue;

      TrackedEntry & Entry = mTrackedEntries[Event.EntryIndex];

      // the registration is consumed once matched, like PositionSet() does,
      // but the tracked entry stays so later re-renders keep updating the cue
      const auto RefIt = std::find(myTempReferences.begin(), myTempReferences.end(), Entry.Blob);
      if (RefIt != myTempReferences.end())
        myTempReferences.erase(RefIt);

      if (Entry.Point == nullptr) {
        Entry.Point = &AddNewChild<KaxCuePoint>(*this);
        Entry.Positions = &AddNewChild<KaxCueTrackPositions>(*Entry.Point);
      }

      auto & NewTime = GetChild<KaxCueTime>(*Entry.Point);
      *static_cast<EbmlUInteger*>(&NewTime) = Event.GlobalTimecode / GlobalTimecodeScale();

      auto & TheTrack = GetChild<KaxCueTrack>(*Entry.Positions);
      *static_cast<EbmlUInteger*>(&TheTrack) = Event.Track;

      auto & TheClustPos = GetChild<KaxCueClusterPosition>(*Entry.Positions);
      *static_cast<EbmlUInteger*>(&TheClustPos) = Event.ClusterPosition;

      auto & TheBlockNum = GetChild<KaxCueBlockNumber>(*Entry.Positions);
      *static_cast<EbmlUInteger*>(&TheBlockNum) = Event.BlockNumber;

      uint64 StatePosition = 0;
      const auto StateIt = StateByTrack.find(Event.Track);
      if (StateIt != StateByTrack.end() && StateIt->second.State != nullptr && StateIt->second.Segment != nullptr)
        StatePosition = StateIt->second.Segment->GetRelativePosition(*StateIt->second.State);

      if (StatePosition != 0) {
        auto & CueState = GetChild<KaxCueCodecState>(*Entry.Positions);
        *static_cast<EbmlUInteger*>(&CueState) = StatePosition;
      } else {
        // no codec state seen for this track: 0 is the format default, so
        // the element is kept out of the file
        for (size_t i = 0; i < Entry.Positions->ListSize(); ++i) {
          if (EbmlId(*(*Entry.Positions)[i]) == EBML_ID(KaxCueCodecState)) {
            delete (*Entry.Positions)[i];
            Entry.Positions->Remove(i);
            break;
          }
        }
      }

      Entry.Point->SetValueIsSet();
    }
  }
}

} // namespace libmatroska
