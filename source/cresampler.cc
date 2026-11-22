// ----------------------------------------------------------------------------
//
//  Copyright (C) 2013 Fons Adriaensen <fons@linuxaudio.org>
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


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <zita-resampler/cresampler.h>


CResampler::CResampler (void) :
    _nchan (0),
    _buff  (0),
    _maxshift (0),
    _touched (false)
{
    reset ();
}


CResampler::~CResampler (void)
{
    clear ();
}


int CResampler::setup (double       ratio,
                       unsigned int nchan)
{
    if (! nchan) return 1;
    clear ();
    _inmax = 50;
    _buff = new float [nchan * (3 + _inmax)];
    _nchan = nchan;
    _pstep = 1 / ratio;
    return reset ();
}


void CResampler::clear (void)
{
    delete[] _buff;
    _buff  = 0;
    _nchan = 0;
    _inmax = 0;
    _pstep = 0;
    _maxshift = 0;
    _align.release ();
    reset ();
}


void CResampler::set_phase (double p)
{
    _phase = p - floor (p);
}


void CResampler::set_ratio (double r)
{
    _pstep = 1.0 / r;
}


int CResampler::prepare_channel_alignment (unsigned int max_shift)
{
    unsigned int n, di, i;
    float        *buff;

    if (!_buff || _touched) return 1;
    if (!max_shift || (max_shift > 32)) return 1;

    di = 4 + 2 * max_shift + _inmax;
    n = _nchan * di;
    buff = new float [n];
    if (!buff) return 1;
    for (i = 0; i < n; i++) buff [i] = 0;
    if (_align.prepare (max_shift, _nchan))
    {
        delete[] buff;
        return 1;
    }
    delete[] _buff;
    _buff = buff;
    _maxshift = max_shift;

    // No input or output has been committed: reset the audio state but keep
    // the phase and ratio settings the caller may have made.
    inp_count = 0;
    out_count = 0;
    inp_data = 0;
    out_data = 0;
    _index = 0;
    _nread = 4 + 2 * max_shift;
    _nzero = 0;
    _touched = false;
    return 0;
}


int CResampler::set_channel_offsets (const double *offsets, unsigned int count)
{
    return _align.set_offsets (offsets, count);
}


int CResampler::channel_inpdist (unsigned int channel, double *distance) const
{
    if (!_align.prepared ()) return 1;
    return _align.channel_inpdist (channel, inpdist (), distance);
}


double CResampler::inpdist (void) const
{
    return (int)(3 + _maxshift - _nread) - _phase;
}


int CResampler::inpsize (void) const
{
    return 4 + 2 * _maxshift;
}


int CResampler::reset (void)
{
    inp_count = 0;
    out_count = 0;
    inp_data = 0;
    out_data = 0;
    _index = 0;
    _phase = 0;
    _nread = 4;
    _nzero = 0;
    _touched = false;
    if (_maxshift)
    {
        unsigned int di = 4 + 2 * _maxshift + _inmax;
        for (unsigned int i = 0; i < _nchan * di; i++) _buff [i] = 0;
        _nread = 4 + 2 * _maxshift;
    }
    return 0;
}


int CResampler::process (void)
{
    if (_align.prepared ()) return process_aligned ();
    return process_unaligned ();
}


int CResampler::process_unaligned (void)
{
    unsigned int   in, nr, n, c, in0, out0;
    int            nz;
    double         ph;
    float          *pb, a, b, d, m0, m1, m2, m3;

    in0 = inp_count;
    out0 = out_count;
    in = _index;
    nr = _nread;
    nz = _nzero;
    ph = _phase;
    pb = _buff + in * _nchan;

    while (out_count)
    {
	if (nr)
	{
	    if (inp_count == 0) break;
            n = (4 - nr) * _nchan;
  	    if (inp_data)
	    {
                for (c = 0; c < _nchan; c++) pb [n + c] = inp_data [c];
		inp_data += _nchan;
		nz = 0;
	    }
	    else
	    {
                for (c = 0; c < _nchan; c++) pb [n + c] = 0;
		if (nz < 4) nz++;
	    }
	    nr--;
	    inp_count--;
	}
	else
	{
	    n = _nchan;
	    if (out_data)
	    {
		if (nz < 4)
		{
		    a = ph;
		    b = 1 - a;
		    d = a * b / 2;
		    m0 = -d * b;
		    m1 = b + (3 * b - 1) * d;
		    m2 = a + (3 * a - 1) * d;
		    m3 = -d * a;
		    for (c = 0; c < n; c++)
		    {
			*out_data++ = m0 * pb [0]
                                    + m1 * pb [n]
                                    + m2 * pb [2 * n]
			            + m3 * pb [3 * n];
			pb++;
		    }
		    pb -= n;
		}
		else
		{
		    for (c = 0; c < n; c++) *out_data++ = 0;
		}
	    }
	    out_count--;

	    ph += _pstep;
	    if (ph >= 1.0)
	    {
		nr = (unsigned int) floor (ph);
		ph -= nr;
		in += nr;
		pb += nr * _nchan;
		if (in >= _inmax)
		{
		    memcpy (_buff, pb, (4 - nr) * _nchan * sizeof (float));
		    in = 0;
		    pb = _buff;
		}
	    }
	}
    }

    _index = in;
    _nread = nr;
    _nzero = nz;
    _phase = ph;
    if ((inp_count != in0) || (out_count != out0)) _touched = true;

    return 0;
}


int CResampler::process_aligned (void)
{
    // Support per channel: [maxshift history][4 nominal][maxshift look-ahead]
    // so that every offset in [-maxshift, maxshift] has its four points.
    // The nominal cubic window is at p1 + maxshift; a channel offset d
    // moves its four points to p1 + maxshift + d. The nominal position
    // (and hence inpdist / phase) is independent of the offsets.
    unsigned int   in, nr, nzlim, c, n, in0, out0, base;
    int            nz, d, m;
    double         ph, f;
    float          *p1, *pb, a, b, dd, m0, m1, m2, m3;

    in0 = inp_count;
    out0 = out_count;
    in = _index;
    nr = _nread;
    nz = _nzero;
    ph = _phase;
    nzlim = 4 + 2 * _maxshift;
    p1 = _buff + in * _nchan;

    while (out_count)
    {
	if (nr)
	{
	    if (inp_count == 0) break;
            n = (nzlim - nr) * _nchan;
  	    if (inp_data)
	    {
                for (c = 0; c < _nchan; c++) p1 [n + c] = inp_data [c];
		inp_data += _nchan;
		nz = 0;
	    }
	    else
	    {
                for (c = 0; c < _nchan; c++) p1 [n + c] = 0;
		if (nz < (int) nzlim) nz++;
	    }
	    nr--;
	    inp_count--;
	}
	else
	{
	    n = _nchan;
	    if (out_data)
	    {
		if (nz < (int) nzlim)
		{
		    for (c = 0; c < _nchan; c++)
		    {
			ChannelAlignment::split (ph, _align.offset (c), &d, &f);
			base = _maxshift + d;
			pb = p1 + base * _nchan + c;
			a = f;
			b = 1 - a;
			dd = a * b / 2;
			m0 = -dd * b;
			m1 = b + (3 * b - 1) * dd;
			m2 = a + (3 * a - 1) * dd;
			m3 = -dd * a;
			*out_data++ = m0 * pb [0]
			            + m1 * pb [n]
			            + m2 * pb [2 * n]
			            + m3 * pb [3 * n];
		    }
		}
		else
		{
		    for (c = 0; c < _nchan; c++) *out_data++ = 0;
		}
	    }
	    out_count--;

	    ph += _pstep;
	    if (ph >= 1.0)
	    {
		m = (int) floor (ph);
		ph -= m;
		nr = (unsigned int) m;
		in += nr;
		p1 += nr * _nchan;
		if (in >= _inmax)
		{
		    memmove (_buff, p1, (nzlim - nr) * _nchan * sizeof (float));
		    in = 0;
		    p1 = _buff;
		}
	    }
	}
    }

    _index = in;
    _nread = nr;
    _nzero = nz;
    _phase = ph;
    if ((inp_count != in0) || (out_count != out0)) _touched = true;

    return 0;
}

