Broadway.js
===========
A JavaScript H.264 decoder.


View a Live Demo:  
http://mbebenita.github.io/Broadway/foxDemo.html  
http://mbebenita.github.io/Broadway/storyDemo.html  
http://mbebenita.github.io/Broadway/treeDemo.html  

The video player first needs to download the entire video before it can start playing, thus appearing to be a bit slow at first, so have patience. You can start the video by clicking on each player. The top left player runs on the main thread, while the remaining players run in background worker threads.

Use an example node app as template:  
https://github.com/soliton4/BroadwayStream  

Technical info
==============

The demo is Android's H.264 decoder compiled with Emscripten to JavaScript, then further optimized with
Google's JavaScript closure compiler and further optimized by hand to use WebGL.

Building the demo:

Install and configure Emscripten (https://github.com/kripken/emscripten)  
The current version of Broadway was built with emscripten 1.35.12  

The code for the demo is in the Decoder folder, to build it run the make.py python script. (Requires at least python 2.7)

Encoding Video
==============

The decoder expects an .mp4 file and does not support weighted prediction for P-frames and CABAC entropy encoding. To create such bitstreams use ffmpeg and x264 with the following command line options:

```
ffmpeg -y -i sourceFile -r 30000/1001 -b:a 2M -bt 4M -vcodec libx264 -pass 1 -coder 0 -bf 0 -flags -loop -wpredp 0 -an targetFile.mp4
```

API
===

Player.js, Decoder.js and YUVWebGLCanvas.js all have a unified module definition.  
You can use them as plain js files or with common.js / AMD  

# Player.js:  

```
var p = new Player({
  <options>
});

p.canvas; // the canvas - put it where you want it

p.decode(<h264 data>);
```

## Options:  

useWorker true / false  
Decode in a worker thread  

workerFile <string>  
Path to Decoder.js. Only neccessary when using worker. Defaults to "Decoder.js"  

webgl true / "auto" / false  
Use webgl. defaults to "auto"  

size { width: <num>, height: <num> }  
Initial size of the canvas. Canvas will resize after video starts streaming.  

## Properties:  

canvas  
domNode  

refers to the canvas element.  

## methods:  

decode (<bin>)

Feed the decoder with h264 stream data.  


# Decoder.js:  

```
var p = new Decoder({
  <options>
});

p.onPictureDecoded; // override with a callback function

p.decode(<h264 data>);
```

## options:  

rgb true / false  
If true will convert the image to rgb. sligtly slower.  

## properties:  

onPictureDecoded  callback function(<bin>, width, height)

Will be called for each frame.

## methods:  

decode (<bin>)

Feed the decoder with h264 stream data.


Native recovery point API
=========================

The native C decoder (`Decoder/inc/H264SwDecApi.h`) supports recovery point
driven output gating for progressive refresh streams. This is a native-only
extension: the Emscripten/JavaScript build does not expose it.

## Interface status

Two functions are added alongside the existing API and follow the same
`H264SwDecRet` return conventions:

```c
H264SwDecRet H264SwDecBeginRecovery(H264SwDecInst decInst);
H264SwDecRet H264SwDecGetRecoveryState(H264SwDecInst decInst, u32 *state);
```

`H264SwDecBeginRecovery` arms a recovery session. It requires active
SPS/PPS (`H264SWDEC_HDRS_NOT_RDY` otherwise) and a fully finished picture
(`H264SWDEC_PARAM_ERR` while a picture is half decoded); a null instance
also returns `H264SWDEC_PARAM_ERR`. The call is repeatable, keeps SPS/PPS
and all reference pictures, and immediately revokes the output eligibility
of every picture currently queued. Decoding, reconstruction, deblocking and
DPB handling are never skipped — only display output is gated.

`H264SwDecGetRecoveryState` reports one of:

| state | meaning |
| ----- | ------- |
| 0 `H264SWDEC_RECOVERY_NORMAL`      | ordinary mode after initialization |
| 1 `H264SWDEC_RECOVERY_WAIT_MARKER` | waiting for a recovery_point marker |
| 2 `H264SWDEC_RECOVERY_WAIT_TARGET` | marker seen, waiting for the target picture |
| 3 `H264SWDEC_RECOVERY_DONE`        | recovered, output eligible again |

A null instance or a null `state` pointer returns `H264SWDEC_PARAM_ERR`.
Streams that never call `H264SwDecBeginRecovery` keep their original pixel
output, picture order and return codes unchanged.

## Complete NAL unit input

The prefix SEI NAL unit (type 6) carrying the `recovery_point` message must
be fed as a complete NAL unit, the same way slice and parameter-set NAL units
are fed through `H264SwDecDecode`. Emulation prevention bytes are still
removed by the existing NAL extraction path; the recovery parser works on
the resulting RBSP. `payloadType`/`payloadSize` extension (`0xFF`) bytes are
honored, other payload types are skipped over their declared size, and the
whole NAL is validated before any marker is committed.

Only `exact_match_flag == 1` with `changing_slice_group_idc == 0` is
accepted (`broken_link_flag` may be either value); unsupported combinations
are ignored. A corrupted or truncated `recovery_point` payload makes the NAL
return a stream error without committing a partial marker or overwriting an
existing waiting state. With a binding picture `frame_num = n` and
`MaxFrameNum = m`, the target is `(n + recovery_frame_cnt) % m`; counts
outside `[0, m-1]` are treated as unsupported markers. An IDR picture
completes the recovery immediately. The supported scope is fixed SPS/PPS,
`frame_mbs_only_flag = 1`, Baseline I/P streams without FMO, MMCO 5 or
frame_num gaps; multiple slices, non-reference pictures, frame_num wrap,
POC type 0/2 and both `noOutputReordering` settings are covered.

## Picture buffer lending period

The pointer returned in `H264SwDecPicture.pOutputPicture` by
`H264SwDecNextPicture` borrows a picture buffer owned by the decoder, exactly
as without gating. The YUV bytes are only valid until the buffer is reused
for a later decoded picture; copy them before the next decode run if they
are needed longer. Gating never copies or modifies pixel data, so an
eligible gated picture is byte-for-byte identical to the same picture in an
ungated run. Suppressed pictures remain valid references internally; they
simply never reach the output, including during an end-of-stream flush.


# [Real World Uses of Broadway.js](https://github.com/mbebenita/Broadway/wiki/Real-World-Uses)
