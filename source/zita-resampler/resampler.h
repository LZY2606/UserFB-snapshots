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


#ifndef __RESAMPLER_H
#define __RESAMPLER_H


#include "zita-resampler/resampler-table.h"


class Resampler
{
public:

    Resampler (void);
    ~Resampler (void);

    int  setup (unsigned int fs_inp,
                unsigned int fs_out,
                unsigned int nchan,
                unsigned int hlen);

    int  setup (unsigned int fs_inp,
                unsigned int fs_out,
                unsigned int nchan,
                unsigned int hlen,
                double       frel);

    // Compact variants take the same parameters as the setup() overloads
    // above plus a phase_budget in [32, 1024]: the maximum number of
    // equidistant phase intervals kept in the coefficient table (the endpoint
    // may take one extra row). The exact rational clock still advances with
    // the true, unreduced sample rates; only the coefficient lookup is mapped
    // onto the smaller table. This allows logical output phase denominators
    // beyond the legacy limit of 1000.
    int  setup_compact (unsigned int fs_inp,
                        unsigned int fs_out,
                        unsigned int nchan,
                        unsigned int hlen,
                        unsigned int phase_budget);

    int  setup_compact (unsigned int fs_inp,
                        unsigned int fs_out,
                        unsigned int nchan,
                        unsigned int hlen,
                        double       frel,
                        unsigned int phase_budget);

    void   clear (void);
    int    reset (void);
    int    nchan (void) const { return _nchan; }
    int    filtlen (void) const { return inpsize (); } // Deprecated
    int    inpsize (void) const;
    double inpdist (void) const;
    int    process (void);

    unsigned int         inp_count;
    unsigned int         out_count;
    float               *inp_data;
    float               *out_data;
    float              **inp_list;
    float              **out_list;

private:

    // Shared implementation. The public compact entry points enforce the
    // 32..1024 budget range; the legacy setup() entry may pass the exact
    // reduced denominator, which can be smaller.
    int  _setup_compact (unsigned int fs_inp,
                         unsigned int fs_out,
                         unsigned int nchan,
                         unsigned int hlen,
                         double       frel,
                         unsigned int phase_budget);

    Resampler_table     *_table;
    unsigned int         _nchan;
    unsigned int         _inmax;
    unsigned int         _index;
    unsigned int         _nread;
    unsigned int         _nzero;
    unsigned int         _phase;
    unsigned int         _pstep;
    unsigned int         _np;     // logical output phase denominator
    float               *_buff;
    void                *_dummy [7];
};


#endif
