/* SPDX-License-Identifier: MIT
 *
 * Offline COLR v1 preview: renders one icon from colrcheck/geometry.ttf in
 * two CPAL palettes onto a binary P6 PPM image. Transparent texels are shown
 * over a checkerboard so coverage and alpha are visible.
 *
 * Usage: colr_preview <output.ppm>
 */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FONT_PATH "colrcheck/geometry.ttf"
#define ICON_GLYPH 1
#define CELL 56          /* one palette tile, including margin */
#define MARGIN 8
#define TILE_W (CELL * 2)
#define TILE_H CELL
#define SCALE 0.05f

static unsigned char *read_all(const char *path, size_t *out_size)
{
   FILE *f = fopen(path, "rb");
   long n;
   unsigned char *b;
   if (!f) return NULL;
   fseek(f, 0, SEEK_END);
   n = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (n <= 0) { fclose(f); return NULL; }
   b = (unsigned char *) malloc((size_t) n);
   if (!b) { fclose(f); return NULL; }
   if (fread(b, 1, (size_t) n, f) != (size_t) n) { free(b); fclose(f); return NULL; }
   fclose(f);
   *out_size = (size_t) n;
   return b;
}

static void blend_checker(unsigned char *rgb, const unsigned char *rgba, int w, int h, int stride)
{
   int x, y;
   for (y = 0; y < h; ++y) {
      for (x = 0; x < w; ++x) {
         const unsigned char *p = rgba + (size_t) y * stride + (size_t) x * 4;
         unsigned char *o = rgb + ((size_t) y * w + x) * 3;
         int checker = ((x / 8) + (y / 8)) & 1;
         /* premultiplied RGBA over an opaque checkerboard background */
         double a = p[3] / 255.0;
         double br = checker ? 204.0 : 235.0;
         double bg = checker ? 204.0 : 235.0;
         double bb = checker ? 204.0 : 235.0;
         double fr = p[0] / 255.0, fg = p[1] / 255.0, fb = p[2] / 255.0;
         int r = (int) (fr + br * (1.0 - a) + 0.5);
         int g = (int) (fg + bg * (1.0 - a) + 0.5);
         int bch = (int) (fb + bb * (1.0 - a) + 0.5);
         o[0] = (unsigned char) (r > 255 ? 255 : r);
         o[1] = (unsigned char) (g > 255 ? 255 : g);
         o[2] = (unsigned char) (bch > 255 ? 255 : bch);
      }
   }
}

int main(int argc, char **argv)
{
   size_t font_size;
   unsigned char *font, *rgba, *rgb;
   stbtt_fontinfo info;
   int stride, palette, x, y;
   FILE *out;

   if (argc != 2) { fprintf(stderr, "usage: %s <output.ppm>\n", argv[0]); return 2; }

   font = read_all(FONT_PATH, &font_size);
   if (!font) { fprintf(stderr, "cannot read %s\n", FONT_PATH); return 2; }
   if (!stbtt_InitFont(&info, font, 0)) { fprintf(stderr, "InitFont failed\n"); return 2; }

   stride = TILE_W * 4;
   rgba = (unsigned char *) calloc((size_t) stride * TILE_H, 1);
   rgb = (unsigned char *) malloc((size_t) TILE_W * TILE_H * 3);
   if (!rgba || !rgb) { fprintf(stderr, "out of memory\n"); return 2; }

   for (palette = 0; palette < 2; ++palette) {
      unsigned char fg[4] = {255, 255, 255, 255};
      unsigned char tile[CELL * CELL * 4];
      int tstride = CELL * 4;
      /* the 800-unit square occupies 40 px at SCALE; center it in the cell */
      float shift_x = (float) MARGIN;
      float shift_y = (float) (MARGIN + 40);
      memset(tile, 0, sizeof tile);
      if (!stbtt_MakeGlyphCOLRBitmap(&info, tile, CELL, CELL, tstride,
                                     SCALE, SCALE, shift_x, shift_y,
                                     ICON_GLYPH, palette, fg)) {
         fprintf(stderr, "COLR render failed for palette %d\n", palette);
         return 2;
      }
      for (y = 0; y < CELL; ++y)
         for (x = 0; x < CELL; ++x) {
            const unsigned char *s = tile + ((size_t) y * CELL + x) * 4;
            unsigned char *d = rgba + (size_t) y * stride + (size_t) (palette * CELL + x) * 4;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
         }
   }

   blend_checker(rgb, rgba, TILE_W, TILE_H, stride);

   out = fopen(argv[1], "wb");
   if (!out) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
   fprintf(out, "P6\n%d %d\n255\n", TILE_W, TILE_H);
   if (fwrite(rgb, 1, (size_t) TILE_W * TILE_H * 3, out) != (size_t) TILE_W * TILE_H * 3) {
      fprintf(stderr, "write failed\n"); fclose(out); return 2;
   }
   fclose(out);

   free(rgb);
   free(rgba);
   free(font);
   printf("COLR_PREVIEW_WROTE %s glyph=%d palettes=2\n", argv[1], ICON_GLYPH);
   return 0;
}
