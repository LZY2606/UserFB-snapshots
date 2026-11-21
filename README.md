# y262

y262 is a mpeg-1/2 video encoder

# features
* MPEG-1/2 video output as a raw bitstream
* 420, 422 and 444 chroma format
* All modes for frame pictures, including interlaced
* Multipass bitrate control, do as many passes as you want
* Adaptive quantization, yeah that variance based stuff
* Psyrd, but it does not work good for mpeg2
* Good picture quality, as far as i can tell
* Slice based threading

# Sadly missing
* Field pictures
* Dual prime
* Frame Threading

# How to build
You need cmake.
You need a C compiler supported by cmake.
You need the yasm assembler. cmake will look for it.

In the root directory of the y262 directory it should be as simple as:
```bash
mkdir build
cd build
cmake -G "Visual Studio 16 2019" -A x64 ..
```
You probably need to replace the generator and architecture with what you have.
Then build !


# Running y262
-- Running y262 --
Once you have your executable you can run the encoder without arguments to get a list of possible parameters.

Sample output:
```
        y262app -in <yuv> -size <width> <height> -out <m2vout>

        -frames <number>    : number of frames to encode, 0 for all
        -threads <on> <cnt> :  threading enabled and number of concurrent slices
        -profile <profile>  :  simple, main, high or 422 profile
        -level <level>      :  low main high1440 high 422main or 422high level
        -chromaf            :  chroma format, 420, 422 or 444
        -rec <reconfile>    :  write reconstructed frames to <reconfile>
        -rcmode <pass>      :  0 = CQ, 1 = 1st pass, 2 = subsequent pass
        -mpin <statsfile>   :  stats file of previous pass
        -mpout <statsfile>  :  output stats file of current pass
        -bitrate <kbps>     :  average bitrate
        -vbvrate <kbps>     :  maximum bitrate
        -vbv <kbps>         :  video buffer size
        -quant <quantizer>  :  quantizer for CQ
        -interlaced         :  enable field macroblock modes
        -bff                :  first input frame is bottom field first
        -pulldown_frcode <num>:frame rate code to pull input up to
        -quality <number>   :  encoder complexity, negative faster, positive slower
        -frcode <number>    :  frame rate code, see mpeg2 spec
        -arinfo <number>    :  aspect ratio information, see mpeg2 spec
        -qscale0            :  use more linear qscale type
        -nump <number>      :  number of p frames between i frames
        -numb <number>      :  number of b frames between i/p frames
        -closedgop          :  bframes after i frames use only backwards prediction
        -noaq               :  disable variance based quantizer modulation
        -psyrd <number>     :  psy rd strength
        -avamat6            :  use avamat6 quantization matrices
        -flatmat            :  use flat quantization matrices <for high rate>
        -intramat <textfile>:  use the 64 numbers in the file as intra matrix
        -intermat <textfile>:  use the 64 numbers in the file as inter matrix
        -videoformat <fmt>  :  pal, secam, ntsc, 709 or unknown
        -mpeg1              :  output mpeg1 instead mpeg2, constraints apply
```

Notes about -in, you can specify "-" if you want to pipe the yuv into the application. The yuv format of the input should match -chromaf.
It can also be in .y4m format. Settings detected in a .y4m can be overridden by giving the specific option after -in.
So for example "-in foreman_cif.yuv -frcode 1" will override the 25Hz from the foreman_cif.y4m with 23.976024Hz. Mpeg2 limitations apply.


Example: So to encode something you call it like so:

./y262app -in my_NTSC_video.yuv -size 720 480 -profile main -level high -rcmode 1 -mpout test.stats -bitrate
3000 -vbvrate 6000 -vbv 6000 -quality 0 -frcode 2 -pulldown_frcode 4 -arinfo 2 -nump 4 -numb 2 -out test_p1.m2v
-videoformat ntsc -interlaced

This will encode the 420 raw yuv file my_NTSC_video.yuv with the dimensions 720x480, signaling main profile at
high level. It is the first pass and multipass data is written to the file test.stats for subsequent passes.
It is encoded with an average bitrate of 3 Mbit and a maximum video rate of 6Mbit, using 6Mbit video buffer.
The encode is done at the default quality/speed tradeoff of 0 ( The -quality parameter roughly takes values from
around -50 to 50 ).
The framerate of the input is 24fps and the sequence is encoded at 29.97 fps, which will result in aprox. 3:2
pulldown in the resulting file. Aspect ratio is set to 4:3. The encoder will place 4 P frames between keyframes
and between the frames of the so resulting IPPPPI sequence 2 B Frames each ( IBBPBBPBBPBBPBBIBB.. ). The
elementary stream is written to test_p1.m2v. The video format and colorspace parameters are written out to
specify ntsc. Field macroblock modes are enabled, slowing things down a lot for a slight coding gain.

Possible -frcode and -pulldown_frcode ( frame rate ) values
```
1: ( 24000 / 1001 ) fps
2: 24 fps
3: 25 fps
4: ( 30000 / 1001 ) fps
5: 30 fps
6: 50 fps
7: ( 60000 / 1001 ) fps
8: 60 fps
```

Possible -arinfo ( aspect ratio ) values:
```
1: 1:1
2: 4:3
3: 16:9
4: 2.21:1
```

Possible -arinfo ( pixel aspect ratio ) values for -mpeg1:
```
1: 1.0
2: 0.6735
3: 0.7031
4: 0.7615
5: 0.8055
6: 0.8437
7: 0.8935
8: 0.9375
9: 0.9815
10: 1.0255
11: 1.0695
12: 1.1250
13: 1.1575
14: 1.2015
```

# Per picture quantization matrices

The encoder can switch the luma and chroma quantization matrices from picture to
picture, which is useful for MPEG-2 streams that want independent luminance and
chrominance frequency weightings or that need to change those weightings mid
stream. It is disabled by default; with `b_picture_quant_matrices = 0` ( the
default ) the bitstream and all callbacks are byte for byte unchanged, and the
two new input fields are ignored.

Enable it through `y262_configuration_t.b_picture_quant_matrices`. The feature
is only available for 8 bit MPEG-2, progressive frames, 4:2:0 chroma and constant
quantizer (`BITRATE_CONTROL_CQ`); any other configuration makes
`y262_initialize` return `Y262_INIT_ERROR_PICTURE_MATRICES`.

When enabled, each `y262_input_picture_t` carries

* `i_quant_matrix_update_mask`: bits 0..3 select updates for luma intra, luma
  inter, chroma intra and chroma inter respectively. Bits above the low four
  must be zero.
* `rgui8_quant_matrices[ 4 ][ 64 ]`: the four matrices in that order, elements in
  natural 8x8 frequency coordinates, each in range 1..255. The first element of
  both intra matrices has to be 8. Only arrays selected by the mask are read.

A picture inherits the full four matrix state of the previously and successfully
pushed picture, then applies its selected updates. The first picture starts from
the configured intra / inter matrices, with the chroma matrices copied from the
corresponding luma matrices. The encoder copies everything during
`y262_push_input_picture`, so the caller may overwrite the input structure and
pixel buffers as soon as the push returns. An invalid mask or matrix makes the
push return `Y262_PUSH_INPUT_ERR_ARG` without consuming the picture order number,
user data or input slot and without advancing the inheritance state; fixing the
input and retrying gives the same result as pushing the valid input directly.

Each encoded picture uses its own snapshot for fast forward quantization, trellis
candidate and distortion costs, inverse quantization and local reconstruction,
selecting the matrix by plane ( luma / chroma ) and coding type ( intra / inter ).
When the matrices change the precomputed quantization tables are rebuilt.

On the bitstream side a single `quant_matrix_extension` ( at most one per
picture ) is emitted between the picture coding extension and the first slice
whenever the picture's matrices differ from what the decoder already has in
effect in decode order; unchanged pictures emit nothing. Matrix values are
written in zigzag order. Loading a luma matrix also reloads the matching chroma
matrix, both in the sequence header and in the extension, so those side effects
are part of the differential emission and the picture's required chroma state is
re-emitted explicitly when it differs from the implicit reload.

The `examples/picture_matrices_example.c` file is a small standalone program that
drives the feature. `tests/picture_matrices.c` covers the inheritance,
coefficient and bitstream behaviour.

# Notes

This is an offline encoder. It may do some retries on bitrate control failure to prevent bad quality or video buffer violations. So on complex content it may slow down quite a lot. This is also the main reason for missing frame threading.

Patent situation regarding Mpeg2 you best check with the [MPEG LA](https://www.mpegla.com) or ask your legal department.