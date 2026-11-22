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


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#undef ENABLE_VEC4
#if defined(ENABLE_SSE2)
#  define ENABLE_VEC4
#  include <pmmintrin.h>
#endif

#include "zita-resampler/vresampler.h"


VResampler::VResampler (void) :
    _table (0),
    _nchan (0),
    _buff  (0),
    _c1 (0),
    _c2 (0),
    _maxshift (0),
    _touched (false)
{
    reset ();
}


VResampler::~VResampler (void)
{
    clear ();
}


int VResampler::setup (double       ratio,
                       unsigned int nchan,
                       unsigned int hlen)
{
    if ((hlen < 8) || (hlen > 96) || (16 * ratio < 1) || (ratio > 256)) return 1;
    return setup (ratio, nchan, hlen, 1.0 - 2.6 / hlen);
}


int VResampler::setup (double       ratio,
                       unsigned int nchan,
                       unsigned int hlen,
                       double       frel)
{
    unsigned int       hl, mi, n;
    double             dp;
    Resampler_table    *T = 0;

    if (!nchan || (64 * ratio < 1.0) || (ratio > 64))
    {
	clear ();
        return 1; 
    }

    dp = NPHASE / ratio;
    hl = hlen;
    mi = 32;
    if (ratio < 1.0) 
    {
        frel *= ratio;
        hl = (unsigned int)(ceil (hl / ratio));
        mi = (unsigned int)(ceil (mi / ratio));
    }
#ifdef ENABLE_VEC4
	    hl = (hl + 3) & ~3;
#endif
    T = Resampler_table::create (frel, hl, NPHASE);
    clear ();
    if (T)
    {
	_table = T;
        n = nchan * (2 * hl + mi);
#ifdef ENABLE_VEC4
	posix_memalign ((void **)(&_buff), 16, n * sizeof (float));
	posix_memalign ((void **)(&_c1), 16, hl * sizeof (float));
	posix_memalign ((void **)(&_c2), 16, hl * sizeof (float));
#else	
	_buff  = new float [n];
	_c1 = new float [hl];
	_c2 = new float [hl];
#endif	
	_nchan = nchan;
 	_ratio = ratio;
	_inmax = mi;
	_pstep = dp;
	_qstep = dp;
	_wstep = 1;
	return reset ();
    }
    else return 1;
}


void VResampler::clear (void)
{
    Resampler_table::destroy (_table);
#ifdef ENABLE_VEC4
    free (_buff);
    free (_c1);
    free (_c2);
#else
    delete[] _buff;
    delete[] _c1;
    delete[] _c2;
#endif    
    _buff  = 0;
    _c1 = 0;
    _c2 = 0;
    _table = 0;
    _nchan = 0;
    _inmax = 0;
    _pstep = 0;
    _qstep = 0;
    _wstep = 1;
    _maxshift = 0;
    _align.release ();
    reset ();
}


void VResampler::set_phase (double p)
{
    if (!_table) return;
    _phase = (p - floor (p)) * _table->_np;
}


void VResampler::set_rrfilt (double t)
{
    if (!_table) return;
    _wstep =  (t < 1) ? 1 : 1 - exp (-1 / t);
}


void VResampler::set_rratio (double r)
{
    if (!_table) return;
    if (r > 16.0) r = 16.0;
    if (r < 0.95) r = 0.95;
    _qstep = _table->_np / (_ratio * r);
}


int VResampler::prepare_channel_alignment (unsigned int max_shift)
{
    unsigned int hl, n, di, i;
    float        *buff;

    if (!_table || _touched) return 1;
    if (!max_shift || (max_shift > 32)) return 1;

    hl = _table->_hl;
    di = 2 * hl + 2 * max_shift + _inmax;
    n = _nchan * di;
#ifdef ENABLE_VEC4
    if (posix_memalign ((void **)(&buff), 16, n * sizeof (float))) return 1;
#else
    buff = new float [n];
#endif
    if (!buff) return 1;
    for (i = 0; i < n; i++) buff [i] = 0;
    if (_align.prepare (max_shift, _nchan))
    {
#ifdef ENABLE_VEC4
        free (buff);
#else
        delete[] buff;
#endif
        return 1;
    }
#ifdef ENABLE_VEC4
    free (_buff);
#else
    delete[] _buff;
#endif
    _buff = buff;
    _maxshift = max_shift;

    // No input or output has been committed, so the engine is in its initial
    // state apart from the larger support window. Keep ratio and smoothing.
    inp_count = 0;
    out_count = 0;
    inp_data = 0;
    out_data = 0;
    _index = 0;
    _nread = 2 * hl + 2 * max_shift;
    _nzero = 0;
    _touched = false;
    return 0;
}


int VResampler::set_channel_offsets (const double *offsets, unsigned int count)
{
    return _align.set_offsets (offsets, count);
}


int VResampler::channel_inpdist (unsigned int channel, double *distance) const
{
    if (!_align.prepared ()) return 1;
    return _align.channel_inpdist (channel, inpdist (), distance);
}


double VResampler::inpdist (void) const
{
    if (!_table) return 0;
    return (int)(_maxshift + _table->_hl + 1 - _nread) - _phase / _table->_np;
}


int VResampler::inpsize (void) const
{
    if (!_table) return 0;
    return 2 * _table->_hl + 2 * _maxshift;
}


int VResampler::reset (void)
{
    if (!_table) return 1;

    inp_count = 0;
    out_count = 0;
    inp_data = 0;
    out_data = 0;
    _index = 0;
    _nread = 0;
    _nzero = 0;
    _phase = 0;
    _touched = false;
    if (_table)
    {
        // With alignment the buffer keeps maxshift frames of history before
        // the nominal window and maxshift frames of look-ahead after it.
        // Clear the audio so a reset stream starts from silence.
        if (_maxshift)
        {
            unsigned int di = 2 * _table->_hl + 2 * _maxshift + _inmax;
            memset (_buff, 0, _nchan * di * sizeof (float));
        }
        _index = 0;
        _nread = 2 * _table->_hl + 2 * _maxshift;
	return 0;
    }
    return 1;
}


int VResampler::process (void)
{
    if (!_table) return 1;
    if (_align.prepared ()) return process_aligned ();
    return process_unaligned ();
}


int VResampler::process_unaligned (void)
{
    int            nr, np, hl, nz, di, i, n;
    unsigned int   in, j, in0, out0;
    double         ph, dp, dd;
    float          a, b, *p1, *p2, *q1, *q2;

    in0 = inp_count;
    out0 = out_count;
    hl = _table->_hl;
    np = _table->_np;
    in = _index;
    nr = _nread;
    nz = _nzero;
    ph = _phase;
    dp = _pstep;

    p1 = _buff + in;
    p2 = p1 + 2 * hl - nr;
    di = 2 * hl + _inmax;

    while (out_count)
    {
        while (nr && inp_count)
        {
            if (inp_data)
            {
                for (j = 0; j < _nchan; j++) p2 [j * di] = inp_data [j];
                inp_data += _nchan;
                nz = 0;
            }
            else
            {
                for (j = 0; j < _nchan; j++) p2 [j * di] = 0;
                if (nz < 2 * hl) nz++;
            }
            p2++;
            nr--;
            inp_count--;
        }
        if (nr) break;

	if (out_data)
	{
	    if (nz < 2 * hl)
	    {
		n = (unsigned int) ph;
		b = (float)(ph - n);
		a = 1.0f - b;
		q1 = _table->_ctab + hl * n;
		q2 = _table->_ctab + hl * (np - n);

#if defined(ENABLE_SSE2)
                __m128 C1, C2, Q1, Q2, S;
   	        C1 = _mm_load1_ps (&a);
		C2 = _mm_load1_ps (&b);
		for (i = 0; i < hl; i += 4)
		{
 		    Q1 = _mm_load_ps (q1 + i);
		    Q2 = _mm_load_ps (q1 + i + hl);
		    S = _mm_add_ps (_mm_mul_ps (Q1, C1), _mm_mul_ps (Q2, C2));
		    _mm_store_ps (_c1 + i, S);
		    Q1 = _mm_load_ps (q2 + i);
		    Q2 = _mm_load_ps (q2 + i - hl);
		    S = _mm_add_ps (_mm_mul_ps (Q1, C1), _mm_mul_ps (Q2, C2));
		    _mm_store_ps (_c2 + i, S);
		}
                for (j = 0; j < _nchan; j++)
                {
                    q1 = p1 + j * di;
                    q2 = p2 + j * di;
                    S = _mm_setzero_ps ();
                    for (i = 0; i < hl; i += 4)
                    {
                        C1 = _mm_load_ps (_c1 + i);
                        Q1 = _mm_loadu_ps (q1);
                        q2 -= 4;
                        S = _mm_add_ps (S, _mm_mul_ps (C1, Q1));
                        C2 = _mm_loadr_ps (_c2 + i);
                        Q2 = _mm_loadu_ps (q2);
                        q1 += 4;
                        S = _mm_add_ps (S, _mm_mul_ps (C2, Q2));
                    }
		    *out_data++ = S [0] + S [1] + S [2] + S [3];
                }

#else
		float s;
		for (i = 0; i < hl; i++)
		{
		    _c1 [i] = a * q1 [i] + b * q1 [i + hl];
		    _c2 [i] = a * q2 [i] + b * q2 [i - hl];
		}
		for (j = 0; j < _nchan; j++)
		{
		    q1 = p1 + j * di;
		    q2 = p2 + j * di;
		    s = 1e-20f;
		    for (i = 0; i < hl; i++)
		    {
			q2--;
			s += *q1 * _c1 [i] + *q2 * _c2 [i];
			q1++;
		    }
		    *out_data++ = s - 1e-20f;
		}
#endif
	    }
	    else
	    {
		for (j = 0; j < _nchan; j++) *out_data++ = 0;
	    }
	}
	out_count--;

	dd =  _qstep - dp;
	if (fabs (dd) < 1e-20) dp = _qstep;
	else dp += _wstep * dd;
	ph += dp;
        if (ph >= np)
        {
  	    nr = (unsigned int) floor (ph / np);
	    ph -= nr * np;;
	    in += nr;
	    p1 += nr;

            if (in >= _inmax)
            {
		n = 2 * hl - nr;
		p2 = _buff;
		for (j = 0; j < _nchan; j++)
		{
                    memmove (p2 + j * di, p1 + j * di, n * sizeof (float));
		}
                in = 0;
                p1 = _buff;
                p2 = p1 + n;
            }
        }
    }

    _index = in;
    _nread = nr;
    _phase = ph;
    _pstep = dp;
    _nzero = nz;
    if ((inp_count != in0) || (out_count != out0)) _touched = true;

    return 0;
}


int VResampler::process_aligned (void)
{
    // Buffer layout per channel, with W = 2*hl + 2*maxshift:
    //   [maxshift history][nominal 2*hl window][maxshift look-ahead][_inmax]
    // p1 is the anchor: the full support starts at p1 and the nominal window
    // at p1 + maxshift. A channel offset d shifts its window to
    // p1 + maxshift + d, with d in [-maxshift, maxshift], so every window
    // lies inside [p1, p1 + W). The nominal source position is still p1,
    // unchanged by any offset.
    int            nr, np, hl, nz, nzlim, di, i, n, d;
    unsigned int   in, j, in0, out0;
    double         ph, dp, dd, q, f;
    float          a, b, *p1, *p2, *w, *q1, *q2;
    float          s;

    in0 = inp_count;
    out0 = out_count;
    hl = _table->_hl;
    np = _table->_np;
    in = _index;
    nr = _nread;
    nz = _nzero;
    ph = _phase;
    dp = _pstep;

    di = 2 * hl + 2 * _maxshift + _inmax;
    p1 = _buff + in;
    p2 = p1 + 2 * hl + 2 * _maxshift - nr;
    nzlim = 2 * hl + 2 * _maxshift;

    while (out_count)
    {
        while (nr && inp_count)
        {
            if (inp_data)
            {
                for (j = 0; j < _nchan; j++) p2 [j * di] = inp_data [j];
                inp_data += _nchan;
                nz = 0;
            }
            else
            {
                for (j = 0; j < _nchan; j++) p2 [j * di] = 0;
                if (nz < nzlim) nz++;
            }
            p2++;
            nr--;
            inp_count--;
        }
        if (nr) break;

        if (out_data)
        {
            if (nz < nzlim)
            {
                q = ph / np;
                for (j = 0; j < _nchan; j++)
                {
                    ChannelAlignment::split (q, _align.offset (j), &d, &f);
                    n = (unsigned int)(f * np);
                    if (n >= np) n = np - 1;
                    b = (float)(f * np - n);
                    a = 1.0f - b;
                    q1 = _table->_ctab + hl * n;
                    q2 = _table->_ctab + hl * (np - n);
                    for (i = 0; i < hl; i++)
                    {
                        _c1 [i] = a * q1 [i] + b * q1 [i + hl];
                        _c2 [i] = a * q2 [i] + b * q2 [i - hl];
                    }
                    w = p1 + j * di + _maxshift + d;
                    q2 = w + 2 * hl;
                    s = 1e-20f;
                    for (i = 0; i < hl; i++)
                    {
                        q2--;
                        s += *w * _c1 [i] + *q2 * _c2 [i];
                        w++;
                    }
                    *out_data++ = s - 1e-20f;
                }
            }
            else
            {
                for (j = 0; j < _nchan; j++) *out_data++ = 0;
            }
        }
        out_count--;

        dd =  _qstep - dp;
        if (fabs (dd) < 1e-20) dp = _qstep;
        else dp += _wstep * dd;
        ph += dp;
        if (ph >= np)
        {
            nr = (unsigned int) floor (ph / np);
            ph -= nr * np;
            in += nr;
            p1 += nr;

            if (in >= _inmax)
            {
                // Keep the whole support: move the W - nr filled frames that
                // start at the support anchor p1 to the buffer start.
                if (nr > (int)(2 * hl + 2 * _maxshift)) nr = 2 * hl + 2 * _maxshift;
                n = 2 * hl + 2 * _maxshift - nr;
                p2 = _buff;
                for (j = 0; j < _nchan; j++)
                {
                    memmove (p2 + j * di, p1 + j * di, n * sizeof (float));
                }
                in = 0;
                p1 = _buff;
                p2 = _buff + n;
            }
        }
    }

    _index = in;
    _nread = nr;
    _phase = ph;
    _pstep = dp;
    _nzero = nz;
    if ((inp_count != in0) || (out_count != out0)) _touched = true;

    return 0;
}


