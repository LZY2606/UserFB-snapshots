////////////////////////////////////////////////////////////////////////////////
///
/// Source contribution tracing support for SoundTouch processing pipeline.
/// See 'SourceTrace.h' for description.
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

#include <algorithm>
#include <assert.h>

#include "SourceTrace.h"

using namespace soundtouch;


// Combines 'count' source rows into one output row.
SourceRow soundtouch::traceCombineRows(const SourceRow *const *srcRows, const double *coeffs, uint count)
{
    SourceRow result;
    size_t total = 0;
    uint i;

    // gather all contributing terms, scaled by their actual coefficients
    for (i = 0; i < count; i ++)
    {
        if (coeffs[i] != 0.0)
        {
            total += srcRows[i]->size();
        }
    }
    if (total == 0) return result;

    result.reserve(total);
    for (i = 0; i < count; i ++)
    {
        double scale = coeffs[i];
        const SourceRow &src = *srcRows[i];

        if (scale == 0.0) continue;     // exact zero coefficient contributes nothing
        for (size_t j = 0; j < src.size(); j ++)
        {
            SourceWeight term;
            term.frame = src[j].frame;
            term.weight = src[j].weight * scale;
            result.push_back(term);
        }
    }

    // order by frame number so that equal frames can be merged
    std::sort(result.begin(), result.end(),
        [](const SourceWeight &a, const SourceWeight &b) { return a.frame < b.frame; });

    // merge terms that share the same source frame, drop exact zeros
    size_t out = 0;
    for (size_t j = 0; j < result.size(); j ++)
    {
        if (out > 0 && result[out - 1].frame == result[j].frame)
        {
            result[out - 1].weight += result[j].weight;
        }
        else
        {
            result[out] = result[j];
            out ++;
        }
    }
    result.resize(out);

    // drop terms whose weights cancelled to exact zero
    result.erase(std::remove_if(result.begin(), result.end(),
        [](const SourceWeight &w) { return w.weight == 0.0; }), result.end());

    return result;
}


// Returns total number of SourceWeight terms held by all rows in the queue.
size_t SourceRowFifo::terms() const
{
    size_t count = 0;
    for (size_t i = 0; i < rows.size(); i ++)
    {
        count += rows[i].size();
    }
    return count;
}


// Appends 'count' rows from the given array.
void SourceRowFifo::put(const SourceRow *src, uint count)
{
    if (src == NULL)
    {
        rows.insert(rows.end(), count, SourceRow());
    }
    else
    {
        rows.insert(rows.end(), src, src + count);
    }
}


// Appends 'count' rows copied from position 'pos' of another queue.
void SourceRowFifo::putFrom(const SourceRowFifo &src, uint pos, uint count)
{
    assert(pos + count <= src.size());
    rows.insert(rows.end(), src.rows.begin() + pos, src.rows.begin() + pos + count);
}


// Moves all rows from 'other' to the end of this queue.
void SourceRowFifo::moveFrom(SourceRowFifo &other)
{
    if (rows.empty())
    {
        rows.swap(other.rows);
        return;
    }
    while (!other.rows.empty())
    {
        rows.push_back(SourceRow());
        rows.back().swap(other.rows.front());
        other.rows.pop_front();
    }
}


// Removes 'count' rows from the front of the queue.
void SourceRowFifo::receive(uint count)
{
    assert(count <= rows.size());
    rows.erase(rows.begin(), rows.begin() + count);
}


// Removes 'count' rows from the front of the queue and moves them into 'out'.
void SourceRowFifo::receive(std::vector<SourceRow> &out, uint count)
{
    assert(count <= rows.size());
    out.clear();
    out.reserve(count);
    for (uint i = 0; i < count; i ++)
    {
        out.push_back(SourceRow());
        out.back().swap(rows.front());
        rows.pop_front();
    }
}


// Keeps only the first 'count' rows, dropping the rest from the end.
void SourceRowFifo::truncate(uint count)
{
    if (count < rows.size())
    {
        rows.resize(count);
    }
}
