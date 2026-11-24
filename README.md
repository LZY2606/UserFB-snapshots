Shine: fast fixed-point mp3 encoding
====================================

[shine](https://github.com/savonet/shine) is a blazing fast mp3 encoding library implemented in 
fixed-point arithmetic. The library can thus be used to perform super fast mp3 encoding on architectures
without a FPU, such as `armel`, etc.. It is also super fast on architectures with a FPU!

How to use?
-----------

The encoding API should be quite straight forward:

```c
#include <shine/layer3.h>
  
(...)

/* See if samplerate and bitrate are valid */
if (shine_check_config(config.wave.samplerate, config.mpeg.bitr) < 0)
  error("Unsupported samplerate/bitrate configuration.");

/* Initiate encoder */
s = shine_initialise(&config);

/* Number of samples (per channel) to feed the encoder with. */
int samples_per_pass = shine_samples_per_pass(s);

/* All the magic happens here */
while (read(buffer, infile, samples_per_pass)) {
  data = shine_encode_buffer(s,buffer,&written);
  write(data, written);
}

/* Flush and write remaining data. */
data = shine_flush(s,&written);
write(written, data);

/* Close encoder. */
shine_close(s);
```

Adaptive mid/side stereo
------------------------

Shine always advertised a joint-stereo header but never actually coded the
mid/side (M/S) representation: `JOINT_STEREO` only changed the frame header.
`shine_set_ms()` adds a real, long-block M/S mode for MPEG-1 stereo:

```c
int shine_set_ms(shine_t s, int mode);
```

* `mode == 0` keeps the historical left/right path (the default).
* `mode == 1` forces the M/S representation.
* `mode == 2` quantises both the left/right and the M/S representation for the
  frame, reconstructs each one, and keeps whichever has the smaller total
  squared error in the original left/right domain.  M/S is only chosen when it
  is strictly better than left/right.

It returns `0` on success.  It returns `-1` for a `NULL` handle, an out-of-range
mode, or an encoder that is not a 32 kHz / 44.1 kHz / 48 kHz stereo instance
initialised as `STEREO` or `JOINT_STEREO`; in that case the previous setting and
the encoding state are left untouched.  A successful call takes effect from the
next `shine_encode_buffer()` or `shine_encode_buffer_interleaved()` call, and
either PCM entry point behaves identically.

M/S frames are signalled as `JOINT_STEREO` with `mode_ext == 2` for the whole
frame (both granules share the decision); left/right frames restore the mode the
encoder was initialised with and use `mode_ext == 0`.  Both representations are
held to the same real CBR frame capacity, `main_data_begin` always stays `0`,
and an all-zero channel releases its half of the granule budget to the channel
that still carries signal.

Switching modes between frames is allowed; the analysis history (subband and
MDCT overlap) is shared, so it is the same as if the setting had never changed.
Disabling M/S returns the encoder to the byte-for-byte historical output.

```c
#include <shine/layer3.h>

shine_config_t config;
shine_t s;
int16_t *pcm[2];
int written;

/* ... fill in config and initialise as usual ... */
s = shine_initialise(&config);
if (!s)
  error("cannot initialise encoder");

/* Strongly correlated material: let the encoder decide per frame. */
if (shine_set_ms(s, 2) < 0)
  error("adaptive M/S needs 32/44.1/48 kHz stereo");

while ((pcm[0] = read_channel(infile, 0, shine_samples_per_pass(s))) &&
       (pcm[1] = read_channel(infile, 1, shine_samples_per_pass(s)))) {
  unsigned char *data = shine_encode_buffer(s, pcm, &written);
  write(data, written);
}

/* Later, go back to the plain left/right path for a quiet passage. */
shine_set_ms(s, 0);

data = shine_flush(s, &written);
write(data, written);
shine_close(s);
```

The switch is not limited to the start of the stream: the call may be made
between any two `shine_encode_buffer()` calls, and either PCM entry point
supports it.

How fast is it?
---------------

On a macbook pro (`arm64`/M1 pro, `FPU`, December 30, 2022):

Lame, `88.7x` realtime:
```bash
LAME 3.100 64bits (http://lame.sf.net)
Using polyphase lowpass filter, transition band: 16538 Hz - 17071 Hz
Encoding /tmp/decoded.wav to /tmp/lame.mp3
Encoding as 44.1 kHz j-stereo MPEG-1 Layer III (11x) 128 kbps qval=3
    Frame          |  CPU time/estim | REAL time/estim | play/CPU |    ETA
 12203/12203 (100%)|    0:03/    0:03|    0:04/    0:04|   88.773x|    0:00
---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
   kbps        LR    MS  %     long switch short %
  128.0       32.6  67.4        96.4   1.9   1.7
Writing LAME Tag...done
ReplayGain: -9.3dB
lame -b 128 /tmp/decoded.wav /tmp/lame.mp3  3.55s user 0.05s system 99% cpu 3.609 total
```

Shine, `318.0x` realtime:
```
shineenc (Liquidsoap version)
WAVE PCM Data, stereo 44100Hz 16bit, duration: 00:05:18
MPEG-I layer III, stereo  Psychoacoustic Model: Shine
Bitrate: 128 kbps  De-emphasis: none   Original
Encoding "/tmp/bla.wav" to "/tmp/shine.mp3"
Finished in 00:00:01 (318.0x realtime)
```

### ⚠ The following are outdated tests ⚠

On a [Raspberry Pi](http://www.raspberrypi.org/) (`ARM`, `FPU`):

Lame, `1.8x` realtime:
```bash
pi@raspberrypi ~ $ lame bla.wav bla.mp3
LAME 3.99.5 32bits (http://lame.sf.net)
Using polyphase lowpass filter, transition band: 16538 Hz - 17071 Hz
Encoding bla.wav to bla.mp3
Encoding as 44.1 kHz j-stereo MPEG-1 Layer III (11x) 128 kbps qval=3
    Frame          |  CPU time/estim | REAL time/estim | play/CPU |    ETA
 12987/12987 (100%)|    3:06/    3:06|    3:06/    3:06|   1.8216x|    0:00
-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
   kbps        LR    MS  %     long switch short %
  128.0        0.1  99.9        89.1   6.1   4.9
Writing LAME Tag...done
ReplayGain: -10.5dB
```

Shine, `3.6x` realtime:
```bash
pi@raspberrypi ~ $ shineenc bla.wav bla.mp3
shineenc (Liquidsoap version)
WAVE PCM Data, stereo 44100Hz 16bit, duration: 00:05:39
MPEG-I layer III, stereo  Psychoacoustic Model: Shine
Bitrate: 128 kbps  De-emphasis: none   Original
Encoding "bla.wav" to "bla.mp3"
Finished in 00:01:35 (3.6x realtime)
```

On a Google Nexus 5 (`ARM`, `FPU`):

Shine, `14s`, `24.2x` realtime:
```bash
u0_a65@hammerhead:/mnt/sdcard $ shineenc bla.wav bla.mp3
shineenc (Liquidsoap version)
WAVE PCM Data, stereo 44100Hz 16bit, duration: 00:05:39
MPEG-I layer III, stereo  Psychoacoustic Model: Shine
Bitrate: 128 kbps  De-emphasis: none   Original
Encoding "bla.wav" to "bla.mp3"
Finished in 00:00:14 (24.2x realtime)
```

Limitations
-----------

The code for the encoder has been written a long time ago (see below) and 
the only work done on this fork consists of reorganizing the code and making a 
proper shared API out of it. Thus, the encoder may not be exempt of bugs.

Also, the encoding algorithm is rather simple. In particular, it does not
have any Psychoacoustic Model.

A bit of history
----------------

This code was dug out from the dusty crates of those times before internet 
and github. It apparently was created by Gabriel Bouvigne sometime around 
the end of the 20th century. The encoder was converted circa 2001 by Pete 
Everett to fixed-point arithmetic for the RISC OS. Last we know, Patrick 
Roberts had worked on the code to make it multi-platform and more library
oriented. That was around 2006.

You can consult `README.old` and the various source files for more 
informations on this code.
