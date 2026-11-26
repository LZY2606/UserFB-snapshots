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

### Lossless rearrangement of BC7 blocks

`bcdec_transform.h` / `bcdec_transform.c` provide
`bcdec_bc7_transform(const void *src16, void *dst16, bcdec_transform_op op)`.
It rearranges a standard 16-byte BC7 block directly—mode, partition bits,
endpoints, p-bits, and selector planes—without decoding to RGBA or
recompressing. `src16` and `dst16` may be identical.

```c
#include "bcdec_transform.h"

void rotate_bc7_block(const unsigned char src[16], unsigned char dst[16])
{
    bcdec_transform_status status =
        bcdec_bc7_transform(src, dst, BCDEC_ROTATE_90);

    if (status == BCDEC_TRANSFORM_INVALID) {
        /* Null pointer, bad operation, or no valid BC7 mode bit. */
    } else if (status == BCDEC_TRANSFORM_UNREPRESENTABLE) {
        /*
         * The transformed partition layout is not one of the standard
         * layouts allowed for this mode.  Decode/recompress here if your
         * pipeline permits it; this library never does that implicitly.
         * dst has been left unchanged.
         */
    }
}
```

Compile `bcdec_transform.c` with the rest of the project; it includes
`bcdec.h` for the shared partition table but does not require a decoder call.

Supported operations are `BCDEC_IDENTITY`, `BCDEC_ROTATE_90`,
`BCDEC_ROTATE_180`, `BCDEC_ROTATE_270`, `BCDEC_FLIP_X`, `BCDEC_FLIP_Y`,
`BCDEC_TRANSPOSE`, and `BCDEC_ANTI_TRANSPOSE`.

`BCDEC_TRANSFORM_UNREPRESENTABLE` is a structural statement about BC7's
fixed partition tables. After moving the subset labels with the requested
pixel transform, the implementation looks for an equivalent standard
partition for the same mode, allowing subsets to be renamed. Mode 0 may only
use partition IDs 0–15. If there is no equivalent partition/subset mapping,
endpoint colors cannot make the block structurally rearrangeable, even when
all endpoints happen to be equal. Single-subset modes (4, 5, and 6) are
always structurally representable. When a partition does match, anchor
indices and endpoint order are adjusted as needed, but endpoint quantized
values are never refit.

The `task_tests/target.cpp` integration test writes its transformed 8×8
example as `task_tests/target.ppm`.

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

