////////////////////////////////////////////////////////////////////////////////
///
/// Source contribution tracing support for SoundTouch processing pipeline.
///
/// When tracing is enabled, every sample frame sitting in the processing
/// pipeline buffers is accompanied by a 'SourceRow' describing which original
/// input frames the sample linearly depends on, and with which weights. The
/// rows flow through the FIFO buffers together with the samples and are
/// combined by the processing operators (WSOLA overlap, interpolation,
/// anti-alias FIR filter) using the very same coefficients that are applied
/// to the sample data.
///
/// Author        : Copyright (c) Olli Parviainen
/// Author e-mail : oparviai 'at' iki.fi
/// SoundTouch WWW: http://www.surina.net/soundtouch
///
////////////////////////////////////////////////////////////////////////////////
//
// License :
//
//  SoundTouch audio processing library
//  Copyright (c) Olli Parviainen
//
//  This library is free software; you can redistribute it and/or
//  modify it under the terms of the GNU Lesser General Public
//  License as published by the Free Software Foundation; either
//  version 2.1 of the License, or (at your option) any later version.
//
//  This library is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//  Lesser General Public License for more details.
//
//  You should have received a copy of the GNU Lesser General Public
//  License along with this library; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
////////////////////////////////////////////////////////////////////////////////

#ifndef SourceTrace_H
#define SourceTrace_H

#include <deque>
#include <vector>

#include "SoundTouch.h"
#include "STTypes.h"

namespace soundtouch
{

/// Combines 'count' source rows into one output row: result is the sum of
/// each source row scaled by the corresponding coefficient. Terms with the
/// same frame number are merged (their weights are summed), exact zero
/// weights are dropped, and the result is strictly ascending by frame.
///
/// The coefficients must be the actual linear coefficients the audio
/// operator applies to the respective source samples.
SourceRow traceCombineRows(const SourceRow *const *rows, const double *coeffs, uint count);

/// FIFO queue of source contribution rows, mirroring a FIFOSampleBuffer one
/// row per sample frame. Only frames currently waiting in the pipeline are
/// held; rows are removed as the corresponding samples are consumed.
class SourceRowFifo
{
private:
    std::deque<SourceRow> rows;

public:
    /// Number of rows currently in the queue.
    uint size() const
    {
        return (uint)rows.size();
    }

    /// Total number of SourceWeight terms held by all rows in the queue.
    size_t terms() const;

    /// Access row at position 'pos' from the front of the queue.
    const SourceRow &operator[](uint pos) const
    {
        return rows[pos];
    }

    /// Appends one row to the end of the queue.
    void put(const SourceRow &row)
    {
        rows.push_back(row);
    }

    /// Appends one row to the end of the queue, moving its contents.
    void put(SourceRow &row)
    {
        rows.push_back(SourceRow());
        rows.back().swap(row);
    }

    /// Appends 'count' rows from the given array. If 'src' is NULL, appends
    /// empty (silent) rows.
    void put(const SourceRow *src, uint count);

    /// Appends 'count' rows copied from position 'pos' of another queue.
    void putFrom(const SourceRowFifo &src, uint pos, uint count);

    /// Moves all rows from 'other' to the end of this queue, leaving 'other'
    /// empty.
    void moveFrom(SourceRowFifo &other);

    /// Removes 'count' rows from the front of the queue.
    void receive(uint count);

    /// Removes 'count' rows from the front of the queue and moves them into
    /// 'out', replacing its previous contents.
    void receive(std::vector<SourceRow> &out, uint count);

    /// Keeps only the first 'count' rows, dropping the rest from the end.
    void truncate(uint count);

    /// Removes all rows.
    void clear()
    {
        rows.clear();
    }
};

}

#endif
