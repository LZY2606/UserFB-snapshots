// File: rg_etc1.h - Fast, high quality ETC1 block packer/unpacker - Rich Geldreich <richgel99@gmail.com>
// Please see ZLIB license at the end of this file.
#pragma once

#include <stddef.h>
#include <vector>

namespace rg_etc1
{
   // Unpacks an 8-byte ETC1 compressed block to a block of 4x4 32bpp RGBA pixels.
   // Returns false if the block is invalid. Invalid blocks will still be unpacked with clamping.
   // This function is thread safe, and does not dynamically allocate any memory.
   // If preserve_alpha is true, the alpha channel of the destination pixels will not be overwritten. Otherwise, alpha will be set to 255.
   bool unpack_etc1_block(const void *pETC1_block, unsigned int* pDst_pixels_rgba, bool preserve_alpha = false);

   // Quality setting = the higher the quality, the slower. 
   // To pack large textures, it is highly recommended to call pack_etc1_block() in parallel, on different blocks, from multiple threads (particularly when using cHighQuality).
   enum etc1_quality
   { 
      cLowQuality,
      cMediumQuality,
      cHighQuality,
   };
      
   struct etc1_pack_params
   {
      etc1_quality m_quality;
      bool m_dithering;
                              
      inline etc1_pack_params() 
      {
         clear();
      }

      void clear()
      {
         m_quality = cHighQuality;
         m_dithering = false;
      }
   };

   // Important: pack_etc1_block_init() must be called before calling pack_etc1_block().
   void pack_etc1_block_init();

   // Packs a 4x4 block of 32bpp RGBA pixels to an 8-byte ETC1 block.
   // 32-bit RGBA pixels must always be arranged as (R,G,B,A) (R first, A last) in memory, independent of platform endianness. A should always be 255.
   // Returns squared error of result.
   // This function is thread safe, and does not dynamically allocate any memory.
   // pack_etc1_block() does not currently support "perceptual" colorspace metrics - it primarily optimizes for RGB RMSE.
   unsigned int pack_etc1_block(void* pETC1_block, const unsigned int* pSrc_pixels_rgba, etc1_pack_params& pack_params);

   // ------------------------------------------------------------------------------
   // ETC2 RGB8 extension (legacy ETC1 individual/differential blocks plus T, H and
   // planar modes). The emitted stream is always a standard 8-byte ETC2 RGB8 block;
   // no private mode markers are stored.
   // ------------------------------------------------------------------------------

   enum etc2_mode
   {
      cETC2Individual,   // ETC1 individual (diff bit = 0)
      cETC2Differential, // ETC1 differential (diff bit = 1, deltas in range)
      cETC2T,            // ETC2 T mode (red differential overflow)
      cETC2H,            // ETC2 H mode (green differential overflow)
      cETC2Planar,       // ETC2 planar mode (blue differential overflow)

      // Short aliases matching the mode names used in the format description.
      individual = cETC2Individual,
      differential = cETC2Differential,
      t = cETC2T,
      h = cETC2H,
      planar = cETC2Planar,
   };

   struct etc2_options
   {
      bool m_allow_t;
      bool m_allow_h;
      bool m_allow_planar;
      etc1_quality m_quality;

      inline etc2_options()
      {
         clear();
      }

      void clear()
      {
         m_allow_t = true;
         m_allow_h = true;
         m_allow_planar = true;
         m_quality = cHighQuality;
      }
   };

   // Packs a 4x4 block of 32bpp RGBA pixels (R,G,B,A byte order, row major) to an
   // 8-byte ETC2 RGB8 block. Alpha is ignored when encoding.
   // valid_mask: bit (4*y+x) selects the pixels that participate in the distortion
   // metric (used for edge blocks of pack_etc2_image). The returned error is the sum
   // of squared RGB byte differences measured on the actual decoded block, restricted
   // to valid pixels.
   // Throws std::invalid_argument when valid_mask is 0 or the quality is invalid.
   // Thread safe, no global mutable search state, input is never modified.
   unsigned int pack_etc2_block(void* pETC2_block, const unsigned int* pSrc_pixels_rgba,
      const etc2_options& options, unsigned int valid_mask = 0xFFFFU);

   // Unpacks an 8-byte ETC2 RGB8 block. Mode detection follows the standard priority:
   // with the diff bit set, a red overflow means T, then green means H, then blue
   // means planar, otherwise the block is a legacy differential/individual ETC1 block.
   // If preserve_alpha is true the destination alpha bytes are kept, otherwise they
   // are set to 255. When mode is non-null it receives the detected mode.
   // The return value mirrors unpack_etc1_block(): false only for a legacy
   // differential block whose deltas are out of range (such a stream is decoded with
   // clamping); T/H/planar/individual blocks always return true.
   bool unpack_etc2_block(const void* pETC2_block, unsigned int* pDst_pixels_rgba,
      bool preserve_alpha = false, etc2_mode* pMode = NULL);

   // Packs a whole RGBA image into 8-byte ETC2 RGB8 blocks, row of blocks first then
   // columns. Width/height must be non-zero, stride must be at least width*4 and every
   // size multiplication is overflow checked; otherwise std::invalid_argument is
   // thrown. Partial 4x4 blocks on the right/bottom border are completed by edge
   // replication; the replicated pixels do not contribute to the search distortion,
   // and row padding bytes are never read.
   // The caller guarantees that rgba points to a buffer holding at least
   // stride*(height-1)+width*4 readable bytes: this raw pointer API is not a
   // length-validating parser.
   std::vector<unsigned char> pack_etc2_image(const unsigned char* pSrc_rgba,
      unsigned int width, unsigned int height, size_t stride, const etc2_options& options);

} // namespace rg_etc1

//------------------------------------------------------------------------------
//
// rg_etc1 uses the ZLIB license:
// http://opensource.org/licenses/Zlib
//
// Copyright (c) 2012 Rich Geldreich
//
// This software is provided 'as-is', without any express or implied
// warranty.  In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not
// claim that you wrote the original software. If you use this software
// in a product, an acknowledgment in the product documentation would be
// appreciated but is not required.
//
// 2. Altered source versions must be plainly marked as such, and must not be
// misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source distribution.
//
//------------------------------------------------------------------------------
