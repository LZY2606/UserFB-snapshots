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


#ifndef __CHANNEL_ALIGNMENT_H
#define __CHANNEL_ALIGNMENT_H


// Shared per-channel source position offsets for VResampler and CResampler.
//
// A positive offset makes a channel read its input later than the common
// nominal source position, a negative one earlier. Offsets are expressed in
// input frames and are limited to +/- max_shift. This class only manages the
// offsets and the *coordinates*: it does not touch any audio or any timing
// state owned by the resampler.
class ChannelAlignment
{
public:

    ChannelAlignment (void);
    ~ChannelAlignment (void);

    // Reserve support for offsets in [-max_shift, max_shift]. May allocate.
    // Initialises all offsets to zero. Returns zero on success.
    int  prepare (unsigned int max_shift, unsigned int nchan);

    // Drop any preparation and the offsets.
    void release (void);

    bool     prepared  (void) const { return _offsets != 0; }
    unsigned max_shift (void) const { return _max_shift; }
    unsigned nchan     (void) const { return _nchan; }

    double offset (unsigned int channel) const { return _offsets [channel]; }

    // Validate and copy count offsets. Validation is completed before any
    // value is committed, so a failure leaves the current offsets unchanged.
    // Must not allocate: the offset array was reserved by prepare ().
    int  set_offsets (const double *offsets, unsigned int count);

    // distance = nominal - offset [channel], nonzero on invalid arguments.
    int  channel_inpdist (unsigned int channel,
                          double       nominal,
                          double      *distance) const;

    // Split fraction + offset into an integer frame shift and a fraction in
    // [0, 1). Negative values are split with floor(), as required for
    // selecting the filter window and interpolation phase.
    static void split (double fraction, double offset,
                       int *shift, double *frac);

private:

    ChannelAlignment (const ChannelAlignment &);
    ChannelAlignment &operator = (const ChannelAlignment &);

    double        *_offsets;
    unsigned int   _max_shift;
    unsigned int   _nchan;
};


#endif
