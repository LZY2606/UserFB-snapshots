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

RTP/RTCP MULTIPLEXING (IPv4 UNICAST)
====================================

By default an IPv4 unicast session uses two consecutive UDP ports: the even
port base p for RTP and p+1 for RTCP. To interoperate with a peer that has
negotiated rtcp-mux, the transport can instead carry both RTP and RTCP over a
single UDP port.

* Enable it with RTPUDPv4TransmissionParams::SetRTCPMultiplexing(true) before
  passing the parameters to RTPSession::Create. IsRTCPMultiplexing() queries
  the configured value; the default is false.

* When enabled, only the single port given by SetPortbase is bound. That port
  may be odd and no second socket or p+1 port is used. SendRTPData and
  SendRTCPData both transmit from that port to the port passed to
  AddDestination. With multiplexing disabled the original p/p+1 rule still
  applies (the port base must be even).

* Incoming datagrams are classified by their second byte: a length of at
  least 2 and RTP version 2, with the second byte in the range 192..223, is
  RTCP; everything else is RTP. The marker bit is not masked, so payload type
  96 with the marker set is still RTP. Datagrams that are shorter than 2 bytes
  or do not use version 2 are silently ignored (including zero-length
  datagrams); normal traffic after them is still delivered. Full format
  validation is done by the regular RTP and RTCP parsers, and a malformed RTCP
  packet is not reclassified as RTP. Received packets keep their real source
  IP/port and arrival time, so ignore/accept filtering uses the actual sender.

* While multiplexing is enabled, payload types 64..95 must not be used, since
  their marker/PT byte would collide with RTCP packet types. RTPPacketBuilder
  rejects these values in SetDefaultPayloadType and in every BuildPacket /
  BuildPacketEx overload regardless of the marker value; a rejected call
  leaves the sequence number, timestamp, counters and last packet unchanged.
  RTPPacketBuilder::SetRTCPMultiplexing(true) itself fails if a default
  payload type in that range was already configured; RTPSession::Create passes
  the transport mode to the builder it uses. The mode is off by default and is
  reset by RTPPacketBuilder::Destroy.

This extension covers IPv4 unicast only. IPv6 and multicast are unchanged, and
no SDP signalling is provided: the application is responsible for negotiating
rtcp-mux out of band.

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

