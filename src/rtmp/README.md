RTMP module
-----------

This module implements Real Time Messaging Protocol (RTMP) [1].




Functional overview:
-------------------

```
RTMP Specification v1.0 .......... YES
RTMP with TCP transport .......... YES

RTMPS (RTMP over TLS) ............ NO
RTMPE (RTMP over Adobe Encryption) NO
RTMPT (RTMP over HTTP) ........... NO
RTMFP (RTMP over UDP) ............ NO

Transport:
Client ........................... YES
Server ........................... YES
IPv4 ............................. YES
IPv6 ............................. YES
DNS Resolving A/AAAA ............. YES

RTMP Components:
RTMP Handshake ................... YES
RTMP Header encoding and decoding. YES
RTMP Chunking .................... YES
RTMP Dechunking .................. YES
AMF0 (Action Message Format) ..... YES
AMF3 (Action Message Format) ..... NO
Send and receive audio/video ..... YES
Regular and extended timestamp ... YES
Multiple streams ................. YES
Sender flow control .............. YES


Sender flow control
-------------------

The sender keeps three independent positions per connection:

- sent:   cumulative bytes successfully handed to TCP from connection
           establishment, including the handshake and every RTMP chunk
           header and payload
- acked:  cumulative position acknowledged by the peer through
           Acknowledgement (type 3) messages
- window: peer acknowledgement window installed by Set Peer Bandwidth
           (type 6)

Set Peer Bandwidth limit semantics:

- hard (0):    the window is replaced and the effective type becomes hard
- soft (1):    the window becomes min(current, new); with no previous
               limit the new value is taken; the type becomes soft
- dynamic (2): honoured like hard only while the effective type is hard,
               otherwise the message is ignored

A zero window, a window of 2^31 or more, or an unknown limit type is a
protocol error and leaves all existing state untouched.  Set Peer
Bandwidth only drives the sender window; it never changes the local
Window Acknowledgement Size, which solely decides when this side sends its
own Acknowledgements.  A Window Acknowledgement Size message never
releases sender credit or unblocks queued media.

Acknowledgement sequence numbers are 32-bit values expanded to the 64-bit
send position nearest the current `sent` value, so ACKs wrap at 4 GB
without a counter reset.  Only acknowledgements that advance strictly
monotonically and do not exceed `sent` are accepted; duplicate, stale and
future acknowledgements release no credit.

A media chunk may be submitted while (sent - acked) < window.  A complete
chunk is always allowed when credit is available, even if it makes the
outstanding amount slightly exceed the window; once the window is reached
media submission stops at the chunk boundary.  This keeps a window smaller
than one chunk from deadlocking the connection.  An ACK or a larger window
resumes draining immediately, without re-sending or skipping bytes.

Protocol control messages (types 1-6) keep using the control chunk stream
and overtake media waiting for credit; they still advance `sent`.  AMF
commands are not control messages and queue behind media.  A drain running
from inside a receive callback (for example the Ping Request answer) is
guarded against re-entrancy.

rtmp_send_audio(), rtmp_send_video() and the control/AMF entry points
return success once the message has been accepted: with no peer limit the
whole message goes out immediately, while under a limit the unsent
remainder is copied into an internal, library-owned queue and the caller
may release its payload right away.  Closing the connection flushes that
queue; TCP send failures keep the existing close semantics.  Window and
queue state are per connection and are never inherited across reconnects.

```




TODO:
----

- [x] improve AMF encoding API
- [x] implement AMF transaction matching
- [x] add support for Data Message
- [x] add support for AMF Strict Array (type 10)
- [ ] add support for TLS encryption
- [x] add support for extended timestamp




Protocol stack:
--------------

    .-------.  .-------.  .-------.
    |  AMF  |  | Audio |  | Video |
    '-------'  '-------'  '-------'
        |          |          |
        +----------+----------'
                   |
               .-------.
               |  RTMP |
               '-------'
                   |
                   |
               .-------.
               |  TCP  |
               '-------'




Message Sequence:
----------------


```
Client                                      Server

|----------------- TCP Connect -------------->|
|                                             |
|                                             |
|                                             |
|<-------------- 3-way Handshake ------------>|
|                                             |
|                                             |
|                                             |
|----------- Command Message(connect) ------->| chunkid=3, streamid=0, tid=1
|                                             |
|<------- Window Acknowledgement Size --------| chunkid=2, streamid=0
|                                             |
|<----------- Set Peer Bandwidth -------------| chunkid=2, streamid=0
|                                             |
|-------- Window Acknowledgement Size ------->|
|                                             |
|<------ User Control Message(StreamBegin) ---| chunkid=2, streamid=0
|                                             |
|<------------ Command Message ---------------| chunkid=3, streamid=0, tid=1
|        (_result- connect response)          |
```


Interop:
-------

- Wowza Streaming Engine 4.7.1
- Youtube service
- FFmpeg's RTMP module




References:
----------

[1] http://wwwimages.adobe.com/www.adobe.com/content/dam/acom/en/devnet/rtmp/pdf/rtmp_specification_1.0.pdf

[2] https://wwwimages2.adobe.com/content/dam/acom/en/devnet/flv/video_file_format_spec_v10_1.pdf

[3] https://en.wikipedia.org/wiki/Action_Message_Format

[4] https://wwwimages2.adobe.com/content/dam/acom/en/devnet/pdf/amf0-file-format-specification.pdf
