# bcdec
Small header-only C library to decompress any BC compressed image inspired by incredible stb libaries (<http://nothings.org/stb>)

Written by Sergii *"iOrange"* Kudlai in 2022.

This library provides functions to decompress blocks of BC compressed images.  

This library does not allocate memory and is trying to use as less stack as possible.

This library was never optimized specifically for speed but for the overall size.  
It has zero external dependencies and is not using any runtime functions.

**This library was writtem by humans and will never contain any LLM-generated code**

### Supported BC formats:
- BC1 (also known as DXT1) + it's "binary alpha" variant BC1A (DXT1A)
- BC2 (also known as DXT3)
- BC3 (also known as DXT5)
- BC4 (also known as ATI1N)
- BC5 (also known as ATI2N)
- BC6H (HDR format)
- BC7

---

BC1/BC2/BC3/BC7 are expected to decompress into 4x4 RGBA blocks 8bit per component (32bit pixel)  
BC4/BC5 are expected to decompress into 4x4 R/RG blocks 8bit per component (8bit and 16bit pixel)  
BC6H is expected to decompress into 4x4 RGB blocks of either 32bit float or 16bit "half" per
component (96bit or 48bit pixel)  

*\* Note that BCDEC\_BC4BC5\_PRECISE option enables signed/unsigned and float support for BC4/BC5*

---

### Rotating and mirroring BC7 blocks without recompressing

`bcdec_transform.h` / `bcdec_transform.c` add `bcdec_bc7_transform()`, which
rearranges the contents of a *compressed* 16 byte BC7 block. Nothing is ever
decompressed to RGBA and nothing is recompressed, so there is no generation
loss: decoding the result with `bcdec_bc7()` gives exactly the pixels you would
get by decoding the source and then applying the same pixel permutation.

This is handy when packing textures: an asset that only needs a quarter turn or
a mirror can reuse its existing compressed blocks.

```c
#include <stdlib.h>
#include <string.h>
#include "bcdec.h"
#include "bcdec_transform.h"

/* Rotates a whole BC7 texture 90 degrees. blocks holds (w/4) * (h/4) blocks;
   the caller swaps w and h afterwards, because the result is h wide by w tall. */
int rotate_texture_90(void* blocks, int w, int h)
{
    unsigned char* src = (unsigned char*)blocks;
    unsigned char* dst = (unsigned char*)malloc((size_t)w * h);
    int bw = w / 4, bh = h / 4;      /* source blocks across / down */
    int sbx, sby;

    if (!dst) return 0;
    for (sby = 0; sby < bh; ++sby) {
        for (sbx = 0; sbx < bw; ++sbx) {
            /* source block (sbx, sby) becomes block (bh - 1 - sby, sbx) */
            int obx = bh - 1 - sby;
            int oby = sbx;
            if (bcdec_bc7_transform(src + (sby * bw + sbx) * 16,
                                    dst + (oby * bh + obx) * 16,
                                    BCDEC_ROTATE_90) != BCDEC_TRANSFORM_OK) {
                free(dst);
                return 0;            /* some block was not representable */
            }
        }
    }
    memcpy(blocks, dst, (size_t)w * h);
    free(dst);
    return 1;
}
```

The operation applies inside each 4x4 block using the top-left origin:

| value                  | source (x, y) maps to |
|------------------------|-----------------------|
| `BCDEC_IDENTITY`       | (x, y)                |
| `BCDEC_ROTATE_90`      | (3 - y, x)            |
| `BCDEC_ROTATE_180`     | (3 - x, 3 - y)        |
| `BCDEC_ROTATE_270`     | (y, 3 - x)            |
| `BCDEC_FLIP_X`         | (3 - x, y)            |
| `BCDEC_FLIP_Y`         | (x, 3 - y)            |
| `BCDEC_TRANSPOSE`      | (y, x)                |
| `BCDEC_ANTI_TRANSPOSE` | (3 - y, 3 - x)        |

Return values:

- `BCDEC_TRANSFORM_OK` - the destination block was written.
- `BCDEC_TRANSFORM_INVALID` - a NULL pointer, an unknown operation, or a source
  block whose first eight bits contain no BC7 mode bit.
- `BCDEC_TRANSFORM_UNREPRESENTABLE` - the block is valid BC7, but the permuted
  pixels cannot be described by the subset structure of its mode.

On any failure the destination is left untouched. `src16` and `dst16` may point
to the same block (in-place works); any other overlap is the caller's
responsibility. The mode, the channel rotation and the mode 4 index-selection
bit are preserved, and `BCDEC_IDENTITY` reproduces the source bytes exactly.

#### What "structurally unrepresentable" means

A BC7 block does not just store 16 pixels. It stores one to three *subsets*,
each with its own pair of quantized endpoints, plus one selector per pixel saying
how to blend them. Which pixels belong to which subset is dictated by a fixed
standard *partition* chosen by the block's header. The selector at a subset's
*fix-up anchor* is stored with one bit fewer than the others, so the format
relies on a particular pixel being at that anchor.

A pure pixel permutation is representable only if, after the permutation, the
pixels of each source subset still form exactly one of the standard partitions
of that mode (subsets may be renamed) **and** the anchor bookkeeping still lines
up. When it does, the block can be rebuilt bit for bit from the existing
quantized data. When it does not, no amount of shuffling the existing bits can
describe the result - and re-fitting colors would change the image. The function
refuses instead of silently recompressing, hence `BCDEC_TRANSFORM_UNREPRESENTABLE`.
The obvious example is a mode 0 block using partition 14 rotated by 180 degrees:
that shape simply is not one of the sixteen partitions mode 0 can address.

Single subset modes (4, 5, 6) always have exactly one subset and no partition,
so they can express every operation in the table above.

This header can be included from C and C++.

---

You will also find included test program that converts compressed DDS files into TGA/HDR.  
It is a good start to learn on how to use the **bcdec** library.  
It comes with some test images in the *test_images* folder and a batch script *test_bcdec.bat* to run over them.

---

Used HDRI image "Lythwood Room" from <https://polyhaven.com/a/lythwood_room> licensed under CC0 license.

---

### CREDITS:
 - Aras Pranckevičius (@aras-p)
     - BC1/BC3 decoders optimizations (up to 3x the speed)
     - BC6H/BC7 bits pulling routines optimizations
     - optimized BC6H by moving unquantize out of the loop
     - Split BC6H decompression function into 'half' and 'float' variants
  
 - Michael Schmidt (@RunDevelopment)
     - Found better "magic" coefficients for integer interpolation
       of reference colors in BC1 color block, that match with
       the floating point interpolation. This also made it faster
       than integer division.

### bugfixes:
  @linkmauve  
  @wkjarosz  

