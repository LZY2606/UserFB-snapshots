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

#ifndef _AP4_SUBTITLE_DESCRIPTION_RUNS_H_
#define _AP4_SUBTITLE_DESCRIPTION_RUNS_H_

/*----------------------------------------------------------------------
|   includes
+---------------------------------------------------------------------*/
#include "Ap4Types.h"
#include "Ap4Array.h"

/*----------------------------------------------------------------------
|   class references
+---------------------------------------------------------------------*/
class AP4_Track;
class AP4_Sample;

/*----------------------------------------------------------------------
|   AP4_SubtitleRunSampleProvider
+---------------------------------------------------------------------*/
/**
 * Abstract source of the samples considered for run computation. This
 * lets callers feed samples from a regular track table or from a
 * fragmenter's cached sample array.
 */
class AP4_SubtitleRunSampleProvider {
public:
    virtual ~AP4_SubtitleRunSampleProvider() {}
    virtual AP4_Result GetSample(AP4_Ordinal index, AP4_Sample& sample) = 0;
};

/*----------------------------------------------------------------------
|   AP4_SubtitleDescriptionRun
+---------------------------------------------------------------------*/
/**
 * One maximal run of consecutive subtitle samples that share the same
 * sample description index. A run maps onto one 'traf' atom inside a
 * fragmented 'moof'.
 */
struct AP4_SubtitleDescriptionRun {
    AP4_SubtitleDescriptionRun() :
        m_FirstSampleIndex(0),
        m_SampleCount(0),
        m_DescriptionIndex(0) {}

    AP4_Ordinal  m_FirstSampleIndex;  // index of the first sample in the track
    AP4_Cardinal m_SampleCount;       // number of samples in the run
    AP4_Ordinal  m_DescriptionIndex;  // zero-based sample description index
};

/*----------------------------------------------------------------------
|   AP4_SubtitleDescriptionRuns
+---------------------------------------------------------------------*/
/**
 * Helpers used to split the subtitle samples of one fragment into maximal
 * runs of identical sample description index, so that each run can be
 * emitted as its own 'traf' atom. The TTML payloads themselves are never
 * inspected or re-encoded.
 */
class AP4_SubtitleDescriptionRuns {
public:
    /**
     * Verify that a subtitles track is eligible for description-run
     * fragmentation: all its sample descriptions must be 'stpp' entries.
     * Returns AP4_ERROR_NOT_SUPPORTED if a description is not 'stpp'.
     */
    static AP4_Result ValidateTrack(AP4_Track& track);

    /**
     * Split the samples in [first_sample_index, first_sample_index+
     * sample_count) into maximal runs of identical sample description
     * index, in sample order. Returns AP4_ERROR_OUT_OF_RANGE if a sample
     * references a description index that does not exist.
     */
    static AP4_Result ComputeRuns(AP4_Track&                              track,
                                  AP4_Ordinal                             first_sample_index,
                                  AP4_Cardinal                            sample_count,
                                  AP4_Array<AP4_SubtitleDescriptionRun>&  runs);

    /**
     * Same as above, but the samples are read through an explicit provider
     * (used when the samples have been cached out of fragmented tracks,
     * whose own sample tables are empty).
     */
    static AP4_Result ComputeRuns(AP4_SubtitleRunSampleProvider&          provider,
                                  AP4_Cardinal                            description_count,
                                  AP4_Ordinal                             first_sample_index,
                                  AP4_Cardinal                            sample_count,
                                  AP4_Array<AP4_SubtitleDescriptionRun>&  runs);
};

#endif // _AP4_SUBTITLE_DESCRIPTION_RUNS_H_
