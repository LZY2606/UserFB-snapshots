------------------------------------------------------------------------------
                                                               August 18, 2004


                                   JRTPLIB (v3.0.0)


                      Developed at the The Expertise Centre for 
                    Digital Media (EDM), a research institute of 
                       the Limburgs Universitair Centrum (LUC)

                              http://www.edm.luc.ac.be/
                                http://www.luc.ac.be/


LIBRARY LOCATION AND CONTACT
============================
Normally, you should be able to download the latest version of the library
from this url:
	http://research.edm.luc.ac.be/jori/jrtplib/jrtplib.html

If you have questions about the library, you can mail me at:
	jori@lumumba.luc.ac.be

ACKNOWLEDGMENT
==============
I would like thank the people at the Expertise Centre for Digital Media
for giving me the opportunity to create this rewrite of the library.

DISCLAIMER & COPYRIGHT
======================

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.

INSTALLATION NOTES
==================

* To compile the library on a unix-like platform, just start the configure
  script. This should generate a makefile which you can use to start
  building the library using 'make'.

* To compile the library on an MS-Windows platfrom: an Visual Studio
  workspace file jrtplib.dsw is included. The project will search
  for JThread files in ..\jthread-1.0.1
  To compile the library without using JThread, comment the line
  with RTP_SUPPORT_THREAD in rtpconfig_win.h

* For a manual about the library, please refer to doc/ subdirectory.
  Using 'make doc', a PDF version of the documentation will be built.
  Alternatively, you can download the documentation from the homepage.

* For systems with low memory or for applications which will involve only
  a few participants at a time:
  You can set the HASHSIZE defines in rtpsources.h, rtpudpv4transmitter.h
  and rtpudpv6transmitter.h to a lower value to avoid memory being wasted.
  Note that the library will have to be recompiled.

* Used defines:
	- WIN32:
		For compilation on an MS-Windows platform.
	- RTP_HAVE_SYS_FILIO:
		Set if <sys/filio.h> exists.
	- RTP_HAVE_SYS_SOCKIO:
		Set if <sys/sockio.h> exists.
	- RTP_BIG_ENDIAN:
		If set, assume big-endian byte ordering.
	- RTP_SOCKLENTYPE_UINT:
		Indicates that getsockname used an unsigned int as its
		third parameter.
	- RTP_HAVE_SOCKADDR_LEN:
		Indicates that struct sockaddr has an sa_len field.
	- RTP_SUPPORT_IPV4MULTICAST:
		Enables support for IPv4 multicasting.
	- RTP_SUPPORT_THREAD:
		Enables support for JThread.
	- RTP_SUPPORT_SDESPRIV:
		Enables support for RTCP SDES private items.
	- RTP_SUPPORT_INLINETEMPLATEPARAM:
		If set, the compiler should be able to handle an inline
		function as a template parameter.
	- RTP_SUPPORT_PROBATION:
		If set, a few consecutive RTP packets are needed to validate
		a member.
	- RTP_SUPPORT_GNUDRAND:
		If set, the RTPRandom class will use drand48_r and srand48_r
	- RTP_SUPPORT_RANDR:
		If set and RTP_SUPPORT_GNUDRAND is not set, the RTPRandom
		class will use rand_r.
	- RTP_SUPPORT_GETLOGINR:
		If set, the library will use getlogin_r instead of getlogin.
	- RTP_SUPPORT_IPV6:
		If set, IPv6 support is enabled.
	- RTP_SUPPORT_IPV6MULTICAST:
		If set, IPv6 multicasting support is enabled.
	- RTPDEBUG:
		Enables some memory tracking functions and some debug routines.

------------------------------------------------------------------------------

PCMU CONFERENCE MIXER
=====================

The mixer adds a receive-side PCMU (G.711 mu-law, 8 kHz mono) conference mix
to RTPSources. It is implemented in src/rtppcmumixer.h and src/rtppcmumixer.cpp
and is disabled by default; the legacy reception queue and packet retrieval
interfaces behave exactly as before when it is not configured.

Configuration
-------------

  RTPSources::ConfigurePCMUMix(inputs, start_us, window_slots)

declares up to 15 remote SSRCs together with the RTP timestamp each one uses
for playout slot 0. Configuration is only accepted while no mix is active and
none of the input SSRCs is present in the source table yet. inputs must be
non-empty with distinct SSRCs, and window_slots must be between 1 and 256.
On any failure the existing state is left unchanged. RTPSources::Clear()
removes the configuration together with all sources.

Slot timing
-----------

The output head starts at slot 0. Slot k is due at

    start_us + k * 20000 microseconds

and covers 160 PCMU samples (one 20 ms frame). BuildPCMUMixPacket(now_us,...)
returns 1 without changing anything when the head slot is not due yet, and
otherwise builds exactly one slot per call. A packet qualifies for a source
only when, using 32-bit signed modular arithmetic,

    d = packet_timestamp - configured_slot0_timestamp

is non-negative and divisible by 160; the packet then belongs to slot d/160.
Packets whose slot is older than the head, outside [head, head+window_slots),
or which carry CSRCs, a payload type other than 0, or a payload other than
exactly 160 bytes do not participate and are not cached. The first qualifying
packet received for each (source, slot) wins; duplicates never add volume.
RTP timestamps and sequence numbers are allowed to wrap; a single configured
session spans less than 2^31 timestamp ticks and less than 32768 sequence
numbers.

For each due slot every qualifying source is decoded to linear PCM, the
linear samples are summed in a wide integer and only the total is saturated
to int16, then re-encoded to PCMU (a zero result is encoded as 0xff). Absent
sources contribute silence. The output packet is payload type 0 with a 160
byte payload, the marker set only on slot 0, and the builder's SSRC, sequence
number and timestamp; each successful packet advances the builder by one
sequence number, 160 timestamp ticks and 160 payload octets. The CSRC list is
the set of sources that actually supplied a qualifying packet for the slot
(including all-silence frames), sorted by ascending SSRC, and is empty when
no source contributed.

Member removal
--------------

When a configured source is removed through Timeout(), BYETimeout() or
MultipleTimeouts(), its mixer membership and every not-yet-output
contribution are revoked. Merely receiving a BYE does not remove the source:
the existing BYE-timeout timing still applies. If the same SSRC appears
again later it is handled as an ordinary new source and cannot inherit the
old source's contributions.

Packet ownership and build failures
-----------------------------------

Configured-member packets still pass through ProcessRawPacket/
ProcessRTPPacket and keep the normal reception statistics, but their samples
are copied into the mixer and the packets never enter the legacy
GetNextPacket queue, so the new and old queues never own the same pointer.
Other sources keep the classic ownership rules.

RTPPacketBuilder::BuildPacketWithCSRC() uses the supplied CSRC list for one
packet only and restores the persistent CSRC list whether the build succeeds
or fails; a list longer than 15 entries or containing duplicates is rejected.
If a build fails (for example because the maximum packet size is too small),
the previous packet's bytes, the builder's counters/sequence/timestamp, the
mixer's head and stored samples, and the output vector are all preserved, so
after enlarging the packet buffer the same slot can be retried. The head only
advances after a successful build, after which late packets cannot revive the
consumed slot. A successful build is registered with the own source so the
existing RTCP sender report reflects the real packet and octet counts.
	
------------------------------------------------------------------------------

