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


#ifndef __VRESAMPLER_H
#define __VRESAMPLER_H


#include "zita-resampler/resampler-table.h"


class VResampler
{
public:

    VResampler (void);
    ~VResampler (void);

    int  setup (double       ratio,
                unsigned int nchan,
                unsigned int hlen);

    int  setup (double       ratio,
                unsigned int nchan,
                unsigned int hlen,
                double       frel);

    // Compact variants: the coefficient table keeps at most phase_budget
    // (32..1024) equidistant phase intervals, the endpoint taking one extra
    // row. The phase clock, set_phase (), set_rratio (), set_rrfilt () and
    // inpdist () continue to operate on the true fractional input position.
    int  setup_compact (double       ratio,
                        unsigned int nchan,
                        unsigned int hlen,
                        unsigned int phase_budget);

    int  setup_compact (double       ratio,
                        unsigned int nchan,
                        unsigned int hlen,
                        double       frel,
                        unsigned int phase_budget);

    void   clear (void);
    int    reset (void);
    int    nchan (void) const { return _nchan; }
    int    inpsize (void) const;
    double inpdist (void) const;
    int    process (void);
    
    void set_phase (double p);
    void set_rrfilt (double t);
    void set_rratio (double r);    

    unsigned int         inp_count;
    unsigned int         out_count;
    float               *inp_data;
    float               *out_data;
    void                *inp_list;
    void                *out_list;

private:

    enum { NPHASE = 120 };

    Resampler_table     *_table;
    unsigned int         _nchan;
    unsigned int         _inmax;
    unsigned int         _index;
    unsigned int         _nread;
    unsigned int         _nzero;
    unsigned int         _np;     // logical phase scale of the phase clock
    double               _ratio;
    double               _phase;
    double               _pstep;
    double               _qstep;
    double               _wstep;
    float               *_buff;
    float               *_c1;
    float               *_c2;
    void                *_dummy [7];
};


#endif
