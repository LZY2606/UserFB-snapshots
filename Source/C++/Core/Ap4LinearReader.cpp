/*****************************************************************
|
|    AP4 - Linear Sample Reader
|
|    Copyright 2002-2009 Axiomatic Systems, LLC
|
|
|    This file is part of Bento4/AP4 (MP4 Atom Processing Library).
|
|    Unless you have obtained Bento4 under a difference license,
|    this version of Bento4 is Bento4|GPL.
|    Bento4|GPL is free software; you can redistribute it and/or modify
|    it under the terms of the GNU General Public License as published by
|    the Free Software Foundation; either version 2, or (at your option)
|    any later version.
|
|    Bento4|GPL is distributed in the hope that it will be useful,
|    but WITHOUT ANY WARRANTY; without even the implied warranty of
|    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
|    GNU General Public License for more details.
|
|    You should have received a copy of the GNU General Public License
|    along with Bento4|GPL; see the file COPYING.  If not, write to the
|    Free Software Foundation, 59 Temple Place - Suite 330, Boston, MA
|    02111-1307, USA.
|
 ****************************************************************/

/*----------------------------------------------------------------------
|   includes
+---------------------------------------------------------------------*/
#include "Ap4LinearReader.h"
#include "Ap4Array.h"
#include "Ap4SampleTable.h"
#include "Ap4MovieFragment.h"
#include "Ap4FragmentSampleTable.h"
#include "Ap4AtomFactory.h"
#include "Ap4TfraAtom.h"
#include "Ap4ContainerAtom.h"
#include "Ap4TfhdAtom.h"
#include "Ap4TfdtAtom.h"
#include "Ap4TrunAtom.h"

/*----------------------------------------------------------------------
|   AP4_LinearReader::AP4_LinearReader
+---------------------------------------------------------------------*/
AP4_LinearReader::AP4_LinearReader(AP4_Movie&      movie, 
                                   AP4_ByteStream* fragment_stream) :
    m_Movie(movie),
    m_Fragment(NULL),
    m_FragmentStream(fragment_stream),
    m_CurrentFragmentPosition(0),
    m_NextFragmentPosition(0),
    m_BufferFullness(0),
    m_BufferFullnessPeak(0),
    m_Mfra(NULL)
{
    m_HasFragments = movie.HasFragments();
    if (fragment_stream) {
        fragment_stream->AddReference();
        fragment_stream->Tell(m_CurrentFragmentPosition);
        m_NextFragmentPosition = m_CurrentFragmentPosition;
    }
}

/*----------------------------------------------------------------------
|   AP4_LinearReader::~AP4_LinearReader
+---------------------------------------------------------------------*/
AP4_LinearReader::~AP4_LinearReader()
{
    for (unsigned int i=0; i<m_Trackers.ItemCount(); i++) {
        delete m_Trackers[i];
    }
    delete m_Fragment;
    delete m_Mfra;
    if (m_FragmentStream) m_FragmentStream->Release();
}

/*----------------------------------------------------------------------
|   AP4_LinearReader::EnableTrack
+---------------------------------------------------------------------*/
AP4_Result 
AP4_LinearReader::EnableTrack(AP4_UI32 track_id)
{
    // check if we don't already have this
    if (FindTracker(track_id)) return AP4_SUCCESS;

    // find the track in the movie
    AP4_Track* track = m_Movie.GetTrack(track_id);
    if (track == NULL) return AP4_ERROR_NO_SUCH_ITEM;
    
    // process this track
    return ProcessTrack(track);
}

/*----------------------------------------------------------------------
|   AP4_LinearReader::FlushQueue
+---------------------------------------------------------------------*/
void
AP4_LinearReader::FlushQueue(Tracker* tracker)
{
    // empty any queued samples
    for (AP4_List<SampleBuffer>::Item* item = tracker->m_Samples.FirstItem();
         item;
         item = item->GetNext()) {
        SampleBuffer* buffer = item->GetData();
        m_BufferFullness -= buffer->m_Data.GetDataSize();
        delete buffer;
    }
    tracker->m_Samples.Clear();
}

/*----------------------------------------------------------------------
|   AP4_LinearReader::FlushQueues
+---------------------------------------------------------------------*/
void
AP4_LinearReader::FlushQueues()
{
    for (unsigned int i=0; i<m_Trackers.ItemCount(); i++) {
        FlushQueue(m_Trackers[i]);
    }
}

/*----------------------------------------------------------------------
|   AP4_LinearReader::SetSampleIndex
+---------------------------------------------------------------------*/
AP4_Result 
AP4_LinearReader::SetSampleIndex(AP4_UI32 track_id, AP4_UI32 sample_index)
{
    Tracker* tracker = FindTracker(track_id);
    if (tracker == NULL) return AP4_ERROR_INVALID_PARAMETERS;
    assert(tracker->m_SampleTable);
    delete tracker->m_NextSample;
    tracker->m_NextSample = NULL;
    if (sample_index >= tracker->m_SampleTable->GetSampleCount()) {
        return AP4_ERROR_OUT_OF_RANGE;
    }
    tracker->m_Eos = false;
    tracker->m_NextSampleIndex = sample_index;
    
    // empty any queued samples
    for (AP4_List<SampleBuffer>::Item* item = tracker->m_Samples.FirstItem();
         item;
         item = item->GetNext()) {
        SampleBuffer* buffer = item->GetData();
        m_BufferFullness -= buffer->m_Data.GetDataSize();
        delete buffer;
    }
    tracker->m_Samples.Clear();
    
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_LinearReader::SeekTo
+---------------------------------------------------------------------*/
AP4_Result
AP4_LinearReader::SeekTo(AP4_UI32 time_ms, AP4_UI32* actual_time_ms)
{
    if (actual_time_ms) *actual_time_ms = time_ms; // default
    
    // we only support fragmented sources for now
    if (!m_HasFragments) return AP4_ERROR_NOT_SUPPORTED;
    
    // look for a fragment index
    if (m_Mfra == NULL) {
        if (m_FragmentStream) {
            // get the size of the stream (needed)
            AP4_LargeSize stream_size = 0;
            m_FragmentStream->GetSize(stream_size);

            if (stream_size > 12) {
                // remember where we are
                AP4_Position here;
                m_FragmentStream->Tell(here);
                
                // read the last 12 bytes
                unsigned char mfro[12];
                AP4_Result result = m_FragmentStream->Seek(stream_size-12);
                if (AP4_SUCCEEDED(result)) {
                    result = m_FragmentStream->Read(mfro, 12);
                }
                if (AP4_SUCCEEDED(result) && mfro[0] == 'm' && mfro[1] == 'f' && mfro[2] == 'r' && mfro[3] == 'o') {
                    AP4_UI32 mfra_size = AP4_BytesToUInt32BE(&mfro[8]);
                    if ((AP4_LargeSize)mfra_size < stream_size) {
                        result = m_FragmentStream->Seek(stream_size-mfra_size);
                        if (AP4_SUCCEEDED(result)) {
                            AP4_Atom* mfra = NULL;
                            AP4_LargeSize available = mfra_size;
                            AP4_DefaultAtomFactory atom_factory;
                            atom_factory.CreateAtomFromStream(*m_FragmentStream, available, mfra);
                            m_Mfra = AP4_DYNAMIC_CAST(AP4_ContainerAtom, mfra);
                        }
                    }
                }
                if (AP4_SUCCEEDED(result)) {
                    result = m_FragmentStream->Seek(here);
                }
            }
        }
    }
    
    // return now if we have not found an index
    if (m_Mfra == NULL) {
        return AP4_ERROR_NOT_SUPPORTED;
    }

    // get the size of the stream (needed to validate entry coordinates)
    AP4_LargeSize stream_size = 0;
    {
        AP4_Result result = m_FragmentStream->GetSize(stream_size);
        if (AP4_FAILED(result)) return result;
    }

    // resolved seek target for one track
    struct SeekTarget {
        SeekTarget() :
            m_Time(0),
            m_MoofOffset(0),
            m_TrafNumber(0),
            m_TrunNumber(0),
            m_SampleNumber(0),
            m_SampleIndex(0),
            m_SkippedDuration(0) {}
        AP4_UI64     m_Time;
        AP4_Position m_MoofOffset;
        unsigned int m_TrafNumber;
        unsigned int m_TrunNumber;
        unsigned int m_SampleNumber;
        AP4_Ordinal  m_SampleIndex;
        AP4_UI64     m_SkippedDuration;
    };

    // first pass: select and resolve an anchor entry for each enabled track,
    // independently, since each track has its own index, time base and anchors
    AP4_Array<SeekTarget> targets;
    AP4_Position          start_position = 0;
    AP4_UI64              earliest_time  = 0;
    for (unsigned int t=0; t<m_Trackers.ItemCount(); t++) {
        Tracker* tracker = m_Trackers[t];

        // find the tfra index for this track
        AP4_TfraAtom* tfra = NULL;
        for (AP4_List<AP4_Atom>::Item* item = m_Mfra->GetChildren().FirstItem();
                                       item;
                                       item = item->GetNext()) {
            if (item->GetData()->GetType() == AP4_ATOM_TYPE_TFRA) {
                AP4_TfraAtom* tfra_ = (AP4_TfraAtom*)item->GetData();
                if (tfra_->GetTrackId() == tracker->m_Track->GetId()) {
                    tfra = tfra_;
                    break;
                }
            }
        }
        if (tfra == NULL) {
            return AP4_ERROR_NOT_SUPPORTED;
        }
        AP4_Array<AP4_TfraAtom::Entry>& entries = tfra->GetEntries();

        // select the last entry with a time not later than the requested time
        // (when several entries have the same time, the last one wins)
        AP4_UI32 time_scale  = tracker->m_Track->GetMediaTimeScale();
        AP4_UI64 media_time  = ((AP4_UI64)time_ms*(AP4_UI64)time_scale)/1000;
        int entry = -1;
        for (int i=0; i<(int)entries.ItemCount(); i++) {
            if (entries[i].m_Time <= media_time) entry = i;
        }
        if (entry < 0) {
            // no random access point at or before the requested time
            return AP4_FAILURE;
        }

        // validate the entry coordinates (they are all 1-based)
        if (entries[entry].m_TrafNumber  == 0 ||
            entries[entry].m_TrunNumber  == 0 ||
            entries[entry].m_SampleNumber == 0) {
            return AP4_ERROR_OUT_OF_RANGE;
        }
        if (entries[entry].m_MoofOffset >= stream_size) {
            return AP4_ERROR_OUT_OF_RANGE;
        }

        // parse the moof the entry points to (this only reads the moof headers)
        AP4_Result result = m_FragmentStream->Seek((AP4_Position)entries[entry].m_MoofOffset);
        if (AP4_FAILED(result)) return result;
        AP4_Atom* atom = NULL;
        AP4_DefaultAtomFactory atom_factory;
        result = atom_factory.CreateAtomFromStream(*m_FragmentStream, atom);
        if (AP4_FAILED(result)) return result;
        if (atom->GetType() != AP4_ATOM_TYPE_MOOF) {
            delete atom;
            return AP4_ERROR_INVALID_FORMAT;
        }
        AP4_ContainerAtom* moof = AP4_DYNAMIC_CAST(AP4_ContainerAtom, atom);
        if (moof == NULL) {
            delete atom;
            return AP4_ERROR_INVALID_FORMAT;
        }

        // find the traf the entry points to, counting the trafs in the order
        // in which they appear in the moof
        AP4_ContainerAtom* traf = NULL;
        {
            unsigned int traf_number = 0;
            for (AP4_List<AP4_Atom>::Item* item = moof->GetChildren().FirstItem();
                                           item;
                                           item = item->GetNext()) {
                if (item->GetData()->GetType() == AP4_ATOM_TYPE_TRAF) {
                    ++traf_number;
                    if (traf_number == entries[entry].m_TrafNumber) {
                        traf = AP4_DYNAMIC_CAST(AP4_ContainerAtom, item->GetData());
                        break;
                    }
                }
            }
        }
        if (traf == NULL) {
            delete moof;
            return AP4_ERROR_OUT_OF_RANGE;
        }

        // the traf must belong to this track
        AP4_TfhdAtom* tfhd = AP4_DYNAMIC_CAST(AP4_TfhdAtom, traf->GetChild(AP4_ATOM_TYPE_TFHD));
        if (tfhd == NULL || tfhd->GetTrackId() != tracker->m_Track->GetId()) {
            delete moof;
            return AP4_ERROR_INVALID_FORMAT;
        }

        // find the trun the entry points to, counting the truns in the order
        // in which they appear in the traf, and resolve the sample number to
        // an index in the track's sample table for the fragment
        AP4_Ordinal sample_index = 0;
        bool        trun_found   = false;
        {
            unsigned int trun_number = 0;
            for (AP4_List<AP4_Atom>::Item* item = traf->GetChildren().FirstItem();
                                           item;
                                           item = item->GetNext()) {
                if (item->GetData()->GetType() == AP4_ATOM_TYPE_TRUN) {
                    AP4_TrunAtom* trun = AP4_DYNAMIC_CAST(AP4_TrunAtom, item->GetData());
                    if (trun == NULL) continue;
                    ++trun_number;
                    if (trun_number == entries[entry].m_TrunNumber) {
                        if (entries[entry].m_SampleNumber > trun->GetEntries().ItemCount()) {
                            delete moof;
                            return AP4_ERROR_OUT_OF_RANGE;
                        }
                        sample_index += entries[entry].m_SampleNumber-1;
                        trun_found = true;
                        break;
                    }
                    sample_index += trun->GetEntries().ItemCount();
                }
            }
        }
        if (!trun_found) {
            delete moof;
            return AP4_ERROR_OUT_OF_RANGE;
        }

        // compute the decode duration of the samples that will be skipped
        // before the anchor sample (they still belong to the decode time
        // of the fragment the anchor is in)
        AP4_UI64 skipped_duration = 0;
        {
            AP4_MovieFragment fragment(moof); // takes ownership of the moof
            AP4_FragmentSampleTable* sample_table = NULL;
            result = fragment.CreateSampleTable(&m_Movie,
                                                tracker->m_Track->GetId(),
                                                NULL,
                                                (AP4_Position)entries[entry].m_MoofOffset,
                                                0,
                                                0,
                                                sample_table);
            if (AP4_FAILED(result)) return result;
            if (sample_table == NULL || sample_index >= sample_table->GetSampleCount()) {
                delete sample_table;
                return AP4_ERROR_OUT_OF_RANGE;
            }
            for (AP4_Ordinal i=0; i<sample_index; i++) {
                AP4_Sample sample;
                sample_table->GetSample(i, sample);
                skipped_duration += sample.GetDuration();
            }
            delete sample_table;
        }

        // record the resolved target for this track
        SeekTarget target;
        target.m_Time            = entries[entry].m_Time;
        target.m_MoofOffset      = (AP4_Position)entries[entry].m_MoofOffset;
        target.m_TrafNumber      = entries[entry].m_TrafNumber;
        target.m_TrunNumber      = entries[entry].m_TrunNumber;
        target.m_SampleNumber    = entries[entry].m_SampleNumber;
        target.m_SampleIndex     = sample_index;
        target.m_SkippedDuration = skipped_duration;
        targets.Append(target);

        // keep track of the earliest anchor, both in file position and in time
        if (t == 0 || target.m_MoofOffset < start_position) {
            start_position = target.m_MoofOffset;
        }
        AP4_UI64 target_time_ms = (target.m_Time*1000)/time_scale;
        if (t == 0 || target_time_ms < earliest_time) {
            earliest_time = target_time_ms;
        }
    }

    // check that we found something
    if (targets.ItemCount() == 0) {
        return AP4_FAILURE;
    }

    // report the earliest of the selected times, converted to milliseconds
    if (actual_time_ms) {
        *actual_time_ms = (AP4_UI32)earliest_time;
    }

    // resume reading from the anchor that comes first in the file
    m_NextFragmentPosition = start_position;

    // flush any queued samples
    FlushQueues();

    // reset tracker states and arm the per-track anchors
    for (unsigned int i=0; i<m_Trackers.ItemCount(); i++) {
        if (m_Trackers[i]->m_SampleTableIsOwned) {
            delete m_Trackers[i]->m_SampleTable;
        }
        delete m_Trackers[i]->m_NextSample;
        m_Trackers[i]->m_SampleTable     = NULL;
        m_Trackers[i]->m_NextSample      = NULL;
        m_Trackers[i]->m_NextSampleIndex = 0;
        m_Trackers[i]->m_NextDts         = 0;
        m_Trackers[i]->m_Eos             = false;

        m_Trackers[i]->m_SeekPoint.m_Pending         = true;
        m_Trackers[i]->m_SeekPoint.m_Time            = targets[i].m_Time;
        m_Trackers[i]->m_SeekPoint.m_MoofOffset      = targets[i].m_MoofOffset;
        m_Trackers[i]->m_SeekPoint.m_TrafNumber      = targets[i].m_TrafNumber;
        m_Trackers[i]->m_SeekPoint.m_TrunNumber      = targets[i].m_TrunNumber;
        m_Trackers[i]->m_SeekPoint.m_SampleNumber    = targets[i].m_SampleNumber;
        m_Trackers[i]->m_SeekPoint.m_SampleIndex     = targets[i].m_SampleIndex;
        m_Trackers[i]->m_SeekPoint.m_SkippedDuration = targets[i].m_SkippedDuration;
    }

    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_LinearReader::ProcessTrack
+---------------------------------------------------------------------*/
AP4_Result
AP4_LinearReader::ProcessTrack(AP4_Track* track) 
{
    // create a new entry for the track
    Tracker* tracker = new Tracker(track);
    tracker->m_SampleTable = track->GetSampleTable();
    return m_Trackers.Append(tracker);
}

/*----------------------------------------------------------------------
|   AP4_LinearReader::ProcessMoof
+---------------------------------------------------------------------*/
AP4_Result
AP4_LinearReader::ProcessMoof(AP4_ContainerAtom* moof, 
                              AP4_Position       moof_offset, 
                              AP4_Position       mdat_payload_offset)
{
    AP4_Result result;
   
    // create a new fragment
    delete m_Fragment;
    m_Fragment = new AP4_MovieFragment(moof);
    
    // update the trackers
    AP4_Array<AP4_UI32> ids;
    m_Fragment->GetTrackIds(ids);
    for (unsigned int i=0; i<m_Trackers.ItemCount(); i++) {
        Tracker* tracker = m_Trackers[i];
        if (tracker->m_SampleTableIsOwned) {
            delete tracker->m_SampleTable;
        }
        tracker->m_SampleTable = NULL;
        tracker->m_NextSampleIndex = 0;

        // a tracker with a pending seek point must not deliver any sample
        // until the fragment that contains its anchor is reached
        bool before_anchor = tracker->m_SeekPoint.m_Pending &&
                             moof_offset != tracker->m_SeekPoint.m_MoofOffset;

        for (unsigned int j=0; j<ids.ItemCount(); j++) {
            if (ids[j] == tracker->m_Track->GetId()) {
                if (before_anchor) {
                    // this fragment is before the tracker's anchor: skip the
                    // samples, but keep their decode time so that the clock of
                    // the following fragments does not start early
                    AP4_FragmentSampleTable* sample_table = NULL;
                    result = m_Fragment->CreateSampleTable(&m_Movie,
                                                           ids[j],
                                                           NULL,
                                                           moof_offset,
                                                           mdat_payload_offset,
                                                           tracker->m_NextDts,
                                                           sample_table);
                    if (AP4_FAILED(result)) return result;
                    if (sample_table) {
                        tracker->m_NextDts += sample_table->GetDuration();
                        delete sample_table;
                    }
                    break;
                }
                AP4_FragmentSampleTable* sample_table = NULL;
                result = m_Fragment->CreateSampleTable(&m_Movie,
                                                       ids[j],
                                                       m_FragmentStream,
                                                       moof_offset,
                                                       mdat_payload_offset,
                                                       tracker->m_NextDts,
                                                       sample_table);
                if (AP4_FAILED(result)) return result;
                tracker->m_SampleTable = sample_table;
                tracker->m_SampleTableIsOwned = true;
                tracker->m_Eos = false;
                if (tracker->m_SeekPoint.m_Pending) {
                    // this fragment contains the tracker's anchor: start from
                    // the sample the index pointed to, and account for the
                    // decode time of the skipped prefix
                    tracker->m_NextSampleIndex = tracker->m_SeekPoint.m_SampleIndex;
                    AP4_ContainerAtom* traf = NULL;
                    if (AP4_SUCCEEDED(m_Fragment->GetTrafAtom(ids[j], traf)) && traf) {
                        AP4_TfdtAtom* tfdt = AP4_DYNAMIC_CAST(AP4_TfdtAtom, traf->GetChild(AP4_ATOM_TYPE_TFDT));
                        if (tfdt) {
                            tracker->m_NextDts = tfdt->GetBaseMediaDecodeTime() +
                                                 tracker->m_SeekPoint.m_SkippedDuration;
                        } else {
                            tracker->m_NextDts += tracker->m_SeekPoint.m_SkippedDuration;
                        }
                    }
                    tracker->m_SeekPoint.m_Pending = false;
                }
                break;
            }
        }
    }

    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_LinearReader::AdvanceFragment
+---------------------------------------------------------------------*/
AP4_Result
AP4_LinearReader::AdvanceFragment()
{
    AP4_Result result;
     
    // go the the start of the next fragment
    result = m_FragmentStream->Seek(m_NextFragmentPosition);
    if (AP4_FAILED(result)) return result;
    m_CurrentFragmentPosition = m_NextFragmentPosition;

    // read atoms until we find a moof
    assert(m_HasFragments);
    if (!m_FragmentStream) return AP4_ERROR_INVALID_STATE;
    AP4_DefaultAtomFactory atom_factory;
    do {
        AP4_Atom* atom = NULL;
        AP4_Position last_position = 0;
        m_FragmentStream->Tell(last_position);
        result = atom_factory.CreateAtomFromStream(*m_FragmentStream, atom);
        if (AP4_SUCCEEDED(result)) {
            if (atom->GetType() == AP4_ATOM_TYPE_MOOF) {
                AP4_ContainerAtom* moof = AP4_DYNAMIC_CAST(AP4_ContainerAtom, atom);
                if (moof) {
                    // remember where the moof started
                    m_CurrentFragmentPosition = last_position;

                    // remember where we are in the stream
                    AP4_Position position = 0;
                    m_FragmentStream->Tell(position);
        
                    // process the movie fragment
                    result = ProcessMoof(moof, position-atom->GetSize(), position+8);
                    if (AP4_FAILED(result)) return result;

                    // compute where the next fragment will be
                    AP4_UI32 size;
                    AP4_UI32 type;
                    m_FragmentStream->Tell(position);
                    result = m_FragmentStream->ReadUI32(size);
                    if (AP4_FAILED(result)) return AP4_SUCCESS; // can't read more
                    result = m_FragmentStream->ReadUI32(type);
                    if (AP4_FAILED(result)) return AP4_SUCCESS; // can't read more
                    if (size == 0) {
                        m_NextFragmentPosition = 0;
                    } else if (size == 1) {
                        AP4_UI64 size_64 = 0;
                        result = m_FragmentStream->ReadUI64(size_64);
                        if (AP4_FAILED(result)) return AP4_SUCCESS; // can't read more
                        m_NextFragmentPosition = position+size_64;
                    } else {
                        m_NextFragmentPosition = position+size;
                    }
                    return AP4_SUCCESS;
                } else {
                    delete atom;
                }
            } else {
                delete atom;
            }            
        }
    } while (AP4_SUCCEEDED(result));
        
    return AP4_ERROR_EOS;
}

/*----------------------------------------------------------------------
|   AP4_LinearReader::Advance
+---------------------------------------------------------------------*/
AP4_Result
AP4_LinearReader::Advance(bool read_data)
{

    AP4_UI64 min_offset = (AP4_UI64)(-1);
    Tracker* next_tracker = NULL;
    for (;;) {
        for (unsigned int i=0; i<m_Trackers.ItemCount(); i++) {
            Tracker* tracker = m_Trackers[i];
            if (tracker->m_Eos) continue;
            if (tracker->m_SampleTable == NULL) continue;
            
            // get the next sample unless we have it already
            if (tracker->m_NextSample == NULL) {
                if (tracker->m_NextSampleIndex >= tracker->m_SampleTable->GetSampleCount()) {
                    if (!m_HasFragments) tracker->m_Eos = true;
                    if (tracker->m_SampleTableIsOwned) {
                        delete tracker->m_SampleTable;
                        tracker->m_SampleTable = NULL;
                    }
                    continue;
                }
                tracker->m_NextSample = new AP4_Sample();
                AP4_Result result = tracker->m_SampleTable->GetSample(tracker->m_NextSampleIndex, *tracker->m_NextSample);
                if (AP4_FAILED(result)) {
                    tracker->m_Eos = true;
                    delete tracker->m_NextSample;
                    tracker->m_NextSample = NULL;
                    continue;
                }
                tracker->m_NextDts += tracker->m_NextSample->GetDuration();
            }
            assert(tracker->m_NextSample);
            
            AP4_UI64 offset = tracker->m_NextSample->GetOffset();
            if (offset < min_offset) {
                min_offset = offset;
                next_tracker = tracker;
            }
        }
        
        if (next_tracker) break;
        if (m_HasFragments) {
            AP4_Result result = AdvanceFragment();
            if (AP4_FAILED(result)) return result;
        } else {
            break;
        }
    }
 
    if (next_tracker) {
        // read the sample into a buffer
        assert(next_tracker->m_NextSample);
        SampleBuffer* buffer = new SampleBuffer(next_tracker->m_NextSample);
        AP4_Result result;
        if (read_data) {
            if (next_tracker->m_Reader) {
                result = next_tracker->m_Reader->ReadSampleData(*buffer->m_Sample, buffer->m_Data);
            } else {
                result = buffer->m_Sample->ReadData(buffer->m_Data);
            }
            if (AP4_FAILED(result)) {
                delete buffer;
                next_tracker->m_NextSample = NULL;
                return result;
            }

            // detach the sample from its source now that we've read its data
            buffer->m_Sample->Detach();
        }
        
        // add the buffer to the queue
        next_tracker->m_Samples.Add(buffer);
        m_BufferFullness += buffer->m_Data.GetDataSize();
        if (m_BufferFullness > m_BufferFullnessPeak) {
            m_BufferFullnessPeak = m_BufferFullness;
        }
        next_tracker->m_NextSample = NULL;
        next_tracker->m_NextSampleIndex++;
        return AP4_SUCCESS;
    } 
    
    return AP4_ERROR_EOS;   
}

/*----------------------------------------------------------------------
|   AP4_LinearReader::PopSample
+---------------------------------------------------------------------*/
bool
AP4_LinearReader::PopSample(Tracker*        tracker, 
                            AP4_Sample&     sample, 
                            AP4_DataBuffer* sample_data)
{
    SampleBuffer* head = NULL;
    if (AP4_SUCCEEDED(tracker->m_Samples.PopHead(head)) && head) {
        assert(head->m_Sample);
        sample = *head->m_Sample;
        if (sample_data) {
            sample_data->SetData(head->m_Data.GetData(), head->m_Data.GetDataSize());
        }
        assert(m_BufferFullness >= head->m_Data.GetDataSize());
        m_BufferFullness -= head->m_Data.GetDataSize();
        delete head;
        return true;
    }
    
    return false;
}

/*----------------------------------------------------------------------
|   AP4_LinearReader::ReadNextSample
+---------------------------------------------------------------------*/
AP4_Result 
AP4_LinearReader::ReadNextSample(AP4_UI32        track_id,
                                 AP4_Sample&     sample,
                                 AP4_DataBuffer& sample_data)
{
    if (m_Trackers.ItemCount() == 0) {
        return AP4_ERROR_NO_SUCH_ITEM;
    }
    
    // look for a sample from a specific track
    Tracker* tracker = FindTracker(track_id);
    if (tracker == NULL) return AP4_ERROR_INVALID_PARAMETERS;
    for(;;) {
        // pop a sample if we can
        if (PopSample(tracker, sample, &sample_data)) return AP4_SUCCESS;

        // don't continue if we've reached the end of that tracker
        if (tracker->m_Eos) return AP4_ERROR_EOS;

        AP4_Result result = Advance();
        if (AP4_FAILED(result)) return result;
    }
        
    // unreachable - return AP4_ERROR_EOS;
}

/*----------------------------------------------------------------------
|   AP4_LinearReader::ReadNextSample
+---------------------------------------------------------------------*/
AP4_Result 
AP4_LinearReader::ReadNextSample(AP4_Sample&     sample,
                                 AP4_DataBuffer* sample_data,
                                 AP4_UI32&       track_id)
{
    if (m_Trackers.ItemCount() == 0) {
        track_id = 0;
        return AP4_ERROR_NO_SUCH_ITEM;
    }
    
    // return the oldest buffered sample, if any
    AP4_UI64 min_offset = (AP4_UI64)(-1);
    Tracker* next_tracker = NULL;
    for (;;) {
        for (unsigned int i=0; i<m_Trackers.ItemCount(); i++) {
            Tracker* tracker = m_Trackers[i];
            if (tracker->m_Eos) continue;
            
            AP4_List<SampleBuffer>::Item* item = tracker->m_Samples.FirstItem();
            if (item) {
                AP4_UI64 offset = item->GetData()->m_Sample->GetOffset();
                if (offset < min_offset) {
                    min_offset = offset;
                    next_tracker = tracker;
                }
            }
        }
        
        // return the sample if we have found a tracker
        if (next_tracker) {
            PopSample(next_tracker, sample, sample_data);
            track_id = next_tracker->m_Track->GetId();
            return AP4_SUCCESS;
        }
        
        // nothing found, read one more sample
        AP4_Result result = Advance(sample_data != NULL);
        if (AP4_FAILED(result)) return result;
    }
    
    // unreachable - return AP4_ERROR_EOS;
}

/*----------------------------------------------------------------------
|   AP4_LinearReader::ReadNextSample
+---------------------------------------------------------------------*/
AP4_Result 
AP4_LinearReader::ReadNextSample(AP4_Sample&     sample,
                                 AP4_DataBuffer& sample_data,
                                 AP4_UI32&       track_id)
{
    return ReadNextSample(sample, &sample_data, track_id);
}

/*----------------------------------------------------------------------
|   AP4_LinearReader::GetNextSample
+---------------------------------------------------------------------*/
AP4_Result 
AP4_LinearReader::GetNextSample(AP4_Sample& sample, AP4_UI32& track_id)
{
    return ReadNextSample(sample, NULL, track_id);
}

/*----------------------------------------------------------------------
|   AP4_LinearReader::FindTracker
+---------------------------------------------------------------------*/
AP4_LinearReader::Tracker*
AP4_LinearReader::FindTracker(AP4_UI32 track_id)
{
    for (unsigned int i=0; i<m_Trackers.ItemCount(); i++) {
        if (m_Trackers[i]->m_Track->GetId() == track_id) return m_Trackers[i];
    }
    
    // not found
    return NULL;
}

/*----------------------------------------------------------------------
|   AP4_LinearReader::Tracker::~Tracker
+---------------------------------------------------------------------*/
AP4_LinearReader::Tracker::~Tracker()
{
    if (m_SampleTableIsOwned) delete m_SampleTable;
    m_Samples.DeleteReferences();
    delete m_NextSample;
    delete m_Reader;
}

/*----------------------------------------------------------------------
|   AP4_DecryptingSampleReader::ReadSampleData
+---------------------------------------------------------------------*/
AP4_Result 
AP4_DecryptingSampleReader::ReadSampleData(AP4_Sample&     sample, 
                                           AP4_DataBuffer& sample_data)
{
    AP4_Result result = sample.ReadData(m_DataBuffer);
    if (AP4_FAILED(result)) return result;

    return m_Decrypter->DecryptSampleData(m_DataBuffer, sample_data);
}
