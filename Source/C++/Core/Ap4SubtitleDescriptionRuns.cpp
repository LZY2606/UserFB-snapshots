/*****************************************************************
|
|    AP4 - Subtitle Sample Description Runs
|
|    Copyright 2002-2024 Axiomatic Systems, LLC
|
|
|    This file is part of Bento4/AP4 (MP4 Atom Processing Library).
|
|    Unless you have obtained Bento4 under a difference license,
|    this version of Bento4 is Bento4|GPL.
|    Bento4|GPL is free software; you can redistribute it and/or modify
|    it under the terms of the GNU General Public License as published by
|    the Free Software Foundation; either version 2 of the License, or
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
#include "Ap4SubtitleDescriptionRuns.h"
#include "Ap4Track.h"
#include "Ap4Sample.h"
#include "Ap4SampleDescription.h"
#include "Ap4Results.h"

/*----------------------------------------------------------------------
|   AP4_SubtitleDescriptionRuns::ValidateTrack
+---------------------------------------------------------------------*/
AP4_Result
AP4_SubtitleDescriptionRuns::ValidateTrack(AP4_Track& track)
{
    for (unsigned int i=0; i<track.GetSampleDescriptionCount(); i++) {
        AP4_SampleDescription* description = track.GetSampleDescription(i);
        if (description == NULL || description->GetFormat() != AP4_SAMPLE_FORMAT_STPP) {
            return AP4_ERROR_NOT_SUPPORTED;
        }
    }

    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_TrackSampleProvider
+---------------------------------------------------------------------*/
class AP4_TrackSampleProvider : public AP4_SubtitleRunSampleProvider {
public:
    AP4_TrackSampleProvider(AP4_Track& track) : m_Track(track) {}
    virtual AP4_Result GetSample(AP4_Ordinal index, AP4_Sample& sample) {
        return m_Track.GetSample(index, sample);
    }
private:
    AP4_Track& m_Track;
};

/*----------------------------------------------------------------------
|   AP4_SubtitleDescriptionRuns::ComputeRuns
+---------------------------------------------------------------------*/
AP4_Result
AP4_SubtitleDescriptionRuns::ComputeRuns(AP4_Track&                              track,
                                         AP4_Ordinal                             first_sample_index,
                                         AP4_Cardinal                            sample_count,
                                         AP4_Array<AP4_SubtitleDescriptionRun>& runs)
{
    AP4_TrackSampleProvider provider(track);
    return ComputeRuns(provider,
                       track.GetSampleDescriptionCount(),
                       first_sample_index,
                       sample_count,
                       runs);
}

/*----------------------------------------------------------------------
|   AP4_SubtitleDescriptionRuns::ComputeRuns
+---------------------------------------------------------------------*/
AP4_Result
AP4_SubtitleDescriptionRuns::ComputeRuns(AP4_SubtitleRunSampleProvider&         provider,
                                         AP4_Cardinal                            description_count,
                                         AP4_Ordinal                             first_sample_index,
                                         AP4_Cardinal                            sample_count,
                                         AP4_Array<AP4_SubtitleDescriptionRun>& runs)
{
    runs.Clear();
    if (sample_count == 0) return AP4_SUCCESS;

    for (AP4_Cardinal i=0; i<sample_count; i++) {
        AP4_Sample sample;
        AP4_Result result = provider.GetSample(first_sample_index+i, sample);
        if (AP4_FAILED(result)) return result;

        AP4_Ordinal description_index = sample.GetDescriptionIndex();
        if (description_index >= description_count) {
            return AP4_ERROR_OUT_OF_RANGE;
        }

        if (runs.ItemCount() && runs[runs.ItemCount()-1].m_DescriptionIndex == description_index) {
            // extend the current run
            runs[runs.ItemCount()-1].m_SampleCount++;
        } else {
            // start a new maximal run
            AP4_SubtitleDescriptionRun run;
            run.m_FirstSampleIndex  = first_sample_index+i;
            run.m_SampleCount       = 1;
            run.m_DescriptionIndex  = description_index;
            runs.Append(run);
        }
    }

    return AP4_SUCCESS;
}
