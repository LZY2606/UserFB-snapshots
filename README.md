# AAC-LC 960 samples frame length

Besides the default 1024 samples frame length, the AAC-LC encoder supports
960 samples per access unit (20 ms at 48 kHz), signalled in the
AudioSpecificConfig via `frameLengthFlag`. The 960 mode uses a true 960-point
long window and eight 120-point short windows; no resampling or zero padding
is involved. Mono and stereo at 32/44.1/48 kHz are supported with RAW and
LATM/LOAS (with config) transports. ADTS cannot signal this frame length and
is rejected with `AACENC_INVALID_CONFIG`, as are SBR/PS object types.

## Example

```c
HANDLE_AACENCODER enc;
AACENC_InfoStruct info;
INT_PCM pcm[960 * 2]; /* one frame, interleaved stereo */

aacEncOpen(&enc, 0, 2);
aacEncoder_SetParam(enc, AACENC_AOT, AOT_AAC_LC);
aacEncoder_SetParam(enc, AACENC_SAMPLERATE, 48000);
aacEncoder_SetParam(enc, AACENC_CHANNELMODE, MODE_2);
aacEncoder_SetParam(enc, AACENC_CHANNELORDER, 1);
aacEncoder_SetParam(enc, AACENC_BITRATE, 128000);
aacEncoder_SetParam(enc, AACENC_TRANSMUX, TT_MP4_RAW); /* or TT_MP4_LOAS */
aacEncoder_SetParam(enc, AACENC_GRANULE_LENGTH, 960);
aacEncEncode(enc, NULL, NULL, NULL, NULL); /* initialize */

aacEncInfo(enc, &info);
/* info.frameLength == 960, info.confBuf holds the AudioSpecificConfig
   with frameLengthFlag set. Feed the decoder exactly one access unit per
   aacDecoder_Fill() call for TT_MP4_RAW. */

/* Encode: each call consumes up to 960 samples per channel. */
{
  AACENC_BufDesc inBuf = {0}, outBuf = {0};
  AACENC_InArgs inArgs = {0};
  AACENC_OutArgs outArgs = {0};
  int inId = IN_AUDIO_DATA, outId = OUT_BITSTREAM_DATA;
  int inSize = sizeof(pcm), outSize = sizeof(outBytes);
  int inElSize = sizeof(INT_PCM), outElSize = 1;
  void *inPtr = pcm, *outPtr = outBytes;

  inBuf.numBufs = outBuf.numBufs = 1;
  inBuf.bufs = &inPtr;
  outBuf.bufs = &outPtr;
  inBuf.bufferIdentifiers = &inId;
  outBuf.bufferIdentifiers = &outId;
  inBuf.bufSizes = &inSize;
  outBuf.bufSizes = &outSize;
  inBuf.bufElSizes = &inElSize;
  outBuf.bufElSizes = &outElSize;
  inArgs.numInSamples = 960 * 2;

  aacEncEncode(enc, &inBuf, &outBuf, &inArgs, &outArgs);
  /* outArgs.numInSamples == 960 * 2, outArgs.numOutBytes bytes in outBytes */
}

/* Flush: call with inArgs.numInSamples = -1 until AACENC_ENCODE_EOF. The
   incomplete trailing frame is zero padded and emitted as tail frames. */

aacEncClose(&enc);
```

When `AACENC_GRANULE_LENGTH` is not set, the encoder behaves exactly as
before (1024 samples per frame, ADTS allowed).
