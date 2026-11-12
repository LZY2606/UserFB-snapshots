TwoLAME
=======

[![Build Status](https://travis-ci.com/njh/twolame.svg?branch=main)](https://travis-ci.com/njh/twolame)

Based on tooLAME by Michael Cheng

All changes to the ISO source are licensed under the LGPL
(see COPYING for details)

    TwoLAME is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    TwoLAME is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with TwoLAME; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA


INTRODUCTION
------------

TwoLAME is an optimized MPEG Audio Layer 2 (MP2) encoder.
It is based heavily on:

* tooLAME by Michael Cheng
* the ISO dist10 code
* improvement to algorithms as part of the LAME project (lame.sf.net)
* other contributors (see AUTHORS)

TwoLAME should be able to be used as a drop-in replacement for
LAME (a MPEG Layer 3 encoder). The frontend takes very similar
command line options to LAME, and the backend library has a very
similar API to LAME.

For the latest version of TwoLAME, visit the project homepage:
http://www.twolame.org/


MPEG Audio Layer 2 (MP2)
------------------------
(taken from Wikipedia article on MP2)

MP2 (sometimes incorrectly named Musicam) is a short form of MPEG Audio Layer II,
and it is also used as a file extension for files containing audio data of this
type. While it has largely been superseded by MP3 for PC and Internet applications,
it remains a dominant standard for audio broadcasting as part of the DAB digital
radio and DVB digital television standards. It is also used internally within the
radio industry, for example in NPR's PRSS Content Depot programming distribution
system.


INSTALLATION
------------

Standard automake process:

    ./configure
    make
    make install


FULL-RANGE VBR
--------------

By default VBR keeps every frame inside the bitrate range covered by a
single MPEG-1 bit-allocation table, and joint stereo is forced back to
plain stereo.  Full-range VBR lifts that restriction: each frame picks
the lowest legal MPEG-1 two-channel bitrate (32/48/56/80 kbps excluded)
that meets the VBR quality target, using whatever allocation table and
stereo mode (stereo, or joint stereo with a 16/12/8/4 subband boundary)
that frame needs.  The configured bitrate acts as the lower bound and
the VBR max bitrate as the upper bound.  It requires MPEG-1, 2 channels,
VBR mode and stereo or joint stereo mode, and is off by default:

    twolame_options *opts = twolame_init();
    twolame_set_num_channels(opts, 2);
    twolame_set_in_samplerate(opts, 44100);
    twolame_set_out_samplerate(opts, 44100);
    twolame_set_mode(opts, TWOLAME_JOINT_STEREO);
    twolame_set_VBR(opts, 1);
    twolame_set_VBR_full_range(opts, 1);        /* enable full-range VBR */
    twolame_set_bitrate(opts, 64);              /* lower bound (kbps) */
    twolame_set_VBR_max_bitrate_kbps(opts, 384);/* upper bound (kbps) */
    if (twolame_init_params(opts) != 0) {
        /* handle error */
    }



REFERENCE PAPERS
----------------

(Specifically Layer II Papers)

Kumar, M & Zubair, M., A high performance software implementation of mpeg audio
encoder, 1996, ICASSP Conf Proceedings (I think)

Fischer, K.A., Calculation of the psychoacoustic simultaneous masked threshold
based on MPEG/Audio Encoder Model One, ICSI Technical Report, 1997
ftp://ftp.icsi.berkeley.edu/pub/real/kyrill/PsychoMpegOne.tar.Z

Hyen-O et al, New Implementation techniques of a real-time mpeg-2 audio encoding
system. p2287, ICASSP 99.

Imai, T., et al, MPEG-1 Audio real-time encoding system, IEEE Trans on Consumer
Electronics, v44, n3 1998. p888

Teh, D., et al, Efficient bit allocation algorithm for ISO/MPEG audio encoder,
Electronics Letters, v34, n8, p721

Murphy, C & Anandakumar, K, Real-time MPEG-1 audio coding and decoding on a DSP
Chip, IEEE Trans on Consumer Electronics, v43, n1, 1997 p40

Hans, M & Bhaskaran, V., A compliant MPEG-1 layer II audio decoder with 16-B
arithmetic operations, IEEE Signal Proc Letters v4 n5 1997 p121

