PDMP3
=====

Public domain mp3 decoder

This project is a fork of the mp3 decoder written by Krister Lagerstrom as part of a master's thesis. This version implements a subset op the libmpg123 API which should be a 100% compatible replacement. If it's not that is considered a bug. In theory more libmpg123 compatible functions could be implemented quite easily.

Available functions for the streaming API are:

pdmp3_handle * pdmp3_new(const char *decoder,int *error);
void pdmp3_delete(pdmp3_handle * id);
int pdmp3_open_feed(pdmp3_handle * id);
int pdmp3_feed(pdmp3_handle * id,const unsigned char * in,size_t size);
int pdmp3_read(pdmp3_handle * id,unsigned char * outmemory,size_t outsize,size_t * done);
int pdmp3_decode(pdmp3_handle * id,const unsigned char * in,size_t insize,unsigned char * out,size_t outsize,size_t * done);
int pdmp3_getformat(pdmp3_handle * id,long * rate,int * channels,int * encoding);

Output format changes
---------------------

Segments of MPEG-1 Layer III with different sampling rates or channel
counts may be concatenated in one fed stream. A change of the output
format (sample rate or channel count; joint stereo vs. stereo is not a
change) is published as an explicit switch point:

  * Once a complete physical frame of a new format is available and all
    PCM of the previous format has been delivered, pdmp3_read returns
    PDMP3_NEW_FORMAT with *done == 0. If a read delivers the last PCM of
    the old format it returns PDMP3_OK instead and the notification
    arrives with the next read. Frame fragments (header, side info or
    main data) never trigger a notification, PCM output or a history
    change; they wait for more input.
  * The event is acknowledged by calling pdmp3_getformat, which reports
    the announced format and confirms it. Until then pdmp3_read keeps
    returning PDMP3_NEW_FORMAT and does not decode the new segment.
    Confirming resets the bit reservoir, IMDCT overlap and synthesis
    history of that handle, so the new segment decodes exactly as if it
    were a stream of its own. Frames of the active format keep their
    history no matter how bitrate, padding or CRC change per frame.
  * Without a pending event pdmp3_getformat only queries the active
    format. Before the first complete frame it returns PDMP3_NEED_MORE
    and leaves the output parameters untouched.
  * pdmp3_decode shares the same barrier; feeding it zero input drains
    pending PCM. pdmp3_open_feed starts a fresh stream on a handle.

Units: all sizes passed to and returned from pdmp3_feed, pdmp3_read and
pdmp3_decode (including *done) are in bytes. The PCM encoding is always
PDMP3_ENC_SIGNED_16, native-endian signed 16-bit, channels interleaved;
one sample frame is 2 * channels bytes. The rate returned by
pdmp3_getformat is in Hz.

Return codes: PDMP3_OK (PCM delivered), PDMP3_NEW_FORMAT (format event,
*done == 0), PDMP3_NEED_MORE (nothing deliverable, input exhausted),
PDMP3_NO_SPACE (no pending event, no old PCM, and outsize cannot hold
one sample frame of the active format; *done == 0, state preserved),
PDMP3_ERR (invalid arguments or stream errors).

TODO
----
  * cleanup,
  * switch to bitfields,
  * fix some of the horribly nested logic

>    License: My work is placed in the public domain. You may do whatever you wish with it, including using it for commercial applications.

The thesis and original code are available at:
 * https://sites.google.com/a/kmlager.com/www/projects

From the abstract:

>    Digital compression of audio data is important due to the bandwidth and storage limitations inherent in networks and computers. Algorithms based on perceptual coding are effective and have become feasible with faster computers. The ISO standard 11172-3 MPEG-1 layer III (a.k.a. MP3) is a perceptual codec that is presently very common for compression of CD quality music. An MP3 decoder has a complex structure and is computationally demanding.

>    The purpose of this master's thesis is to present a tutorial on the standard. We have analysed several algorithms suitable for implementing an MP3 decoder, their advantages and disadvantages with respect to speed, memory demands and implementation complexity. We have also designed and implemented a portable reference MP3 decoder in C.

If you are trying to implement an mp3 decoder and found the 11172-3 reference to be vague, the thesis is a a good read.

Some other good references are:
 * http://blog.bjrn.se/2008/10/lets-build-mp3-decoder.html
 * http://www.multiweb.cz/twoinches/mp3inside.htm
 * http://www.mp3-converter.com/mp3codec/
 * http://oreilly.com/catalog/9781565926615
