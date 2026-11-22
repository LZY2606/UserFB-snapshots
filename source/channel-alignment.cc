// ----------------------------------------------------------------------------
//
//  Copyright (C) 2006-2020 Fons Adriaensen <fons@linuxaudio.org>
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// ----------------------------------------------------------------------------


#include <math.h>
#include "zita-resampler/channel-alignment.h"


ChannelAlignment::ChannelAlignment (void) :
    _offsets (0),
    _max_shift (0),
    _nchan (0)
{
}


ChannelAlignment::~ChannelAlignment (void)
{
    release ();
}


int ChannelAlignment::prepare (unsigned int max_shift, unsigned int nchan)
{
    double *offsets;

    if (!max_shift || (max_shift > 32) || !nchan) return 1;
    if (_offsets && (_max_shift == max_shift) && (_nchan == nchan))
    {
        for (unsigned int c = 0; c < nchan; c++) _offsets [c] = 0;
        return 0;
    }
    // Allocation is allowed here only, never in process () or set_offsets ().
    offsets = new double [nchan];
    if (!offsets) return 1;
    release ();
    _offsets = offsets;
    _max_shift = max_shift;
    _nchan = nchan;
    for (unsigned int c = 0; c < nchan; c++) _offsets [c] = 0;
    return 0;
}


void ChannelAlignment::release (void)
{
    delete[] _offsets;
    _offsets = 0;
    _max_shift = 0;
    _nchan = 0;
}


int ChannelAlignment::set_offsets (const double *offsets, unsigned int count)
{
    if (!_offsets || !offsets || (count != _nchan)) return 1;
    for (unsigned int c = 0; c < count; c++)
    {
        double o = offsets [c];
        if (!isfinite (o)) return 1;
        if ((o < -double (_max_shift)) || (o > double (_max_shift))) return 1;
    }
    for (unsigned int c = 0; c < count; c++) _offsets [c] = offsets [c];
    return 0;
}


int ChannelAlignment::channel_inpdist (unsigned int channel,
                                       double       nominal,
                                       double      *distance) const
{
    if (!_offsets || !distance || (channel >= _nchan)) return 1;
    *distance = nominal - _offsets [channel];
    return 0;
}


void ChannelAlignment::split (double fraction, double offset,
                              int *shift, double *frac)
{
    double x = fraction + offset;
    double f = floor (x);
    *shift = (int) f;
    *frac = x - f;
}
