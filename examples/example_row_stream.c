/*
 * example_row_stream.c - feed a PNG to the provisional-row streaming decoder.
 *
 * The example encodes a small image with LodePNG, then feeds the resulting PNG
 * in fixed-size pieces. A row is printed as soon as it can be unfiltered, even
 * if the IDAT CRC and IEND have not been fed yet.
 *
 * Build (from the repository root), for example:
 *   cc -std=c89 -I. examples/example_row_stream.c lodepng.cpp -lstdc++ -o row_example
 * A C build can compile lodepng.cpp as C:
 *   cc -std=c89 -x c -DLODEPNG_NO_COMPILE_CPP -I. \
 *      lodepng.cpp examples/example_row_stream.c -o row_example
 */

#include "lodepng.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct RowPrinter {
  unsigned rows_seen;
};

static void row_callback(void* user, unsigned w, unsigned h, unsigned y,
                         const unsigned char* rgba) {
  struct RowPrinter* printer = (struct RowPrinter*)user;
  printer->rows_seen++;

  /* The rgba pointer is only valid during this call. */
  printf("provisional row %u/%u: width=%u first=(%u,%u,%u,%u)\n",
         y + 1, h, w, rgba[0], rgba[1], rgba[2], rgba[3]);
}

int main(void) {
  const unsigned w = 7;
  const unsigned h = 5;
  unsigned char* pixels = (unsigned char*)malloc((size_t)w * h * 4u);
  unsigned char* png = 0;
  size_t png_size = 0;
  LodePNGRowDecoder* decoder;
  struct RowPrinter printer;
  unsigned error;
  unsigned x, y;
  size_t pos;

  if(!pixels) {
    fprintf(stderr, "out of memory\n");
    return 1;
  }

  for(y = 0; y != h; ++y) {
    for(x = 0; x != w; ++x) {
      size_t i = ((size_t)y * w + x) * 4u;
      pixels[i + 0] = (unsigned char)(40 + x * 25);
      pixels[i + 1] = (unsigned char)(80 + y * 40);
      pixels[i + 2] = (unsigned char)(160 + x * 10 + y * 5);
      pixels[i + 3] = 255;
    }
  }
  /* Keep one non-opaque pixel so the compact encoder keeps color type RGBA. */
  pixels[3] = 128;

  error = lodepng_encode32(&png, &png_size, pixels, w, h);
  if(error) {
    fprintf(stderr, "encode error %u: %s\n", error, lodepng_error_text(error));
    free(pixels);
    return 1;
  }

  printer.rows_seen = 0;
  decoder = lodepng_row_decoder_create(row_callback, &printer);
  if(!decoder) {
    fprintf(stderr, "could not create row decoder\n");
    lodepng_free(png);
    free(pixels);
    return 1;
  }

  /* Feed in awkward chunks; arbitrary cuts inside headers/CRC are allowed. */
  pos = 0;
  while(pos < png_size) {
    size_t amount = png_size - pos;
    unsigned final_chunk;
    if(amount > 13) amount = 13;
    pos += amount;
    final_chunk = (pos == png_size) ? 1u : 0u;

    error = lodepng_row_decoder_feed(decoder, png + pos - amount, amount,
                                    final_chunk);
    if(error) {
      fprintf(stderr, "streaming decode error %u: %s\n",
              error, lodepng_error_text(error));
      lodepng_row_decoder_destroy(decoder);
      lodepng_free(png);
      free(pixels);
      return 1;
    }
  }

  if(!lodepng_row_decoder_done(decoder) || printer.rows_seen != h) {
    fprintf(stderr, "stream did not complete cleanly\n");
    lodepng_row_decoder_destroy(decoder);
    lodepng_free(png);
    free(pixels);
    return 1;
  }

  /* After completion, an empty feed is still accepted. */
  error = lodepng_row_decoder_feed(decoder, 0, 0, 0);
  if(error) {
    fprintf(stderr, "empty feed after completion returned %u\n", error);
    lodepng_row_decoder_destroy(decoder);
    lodepng_free(png);
    free(pixels);
    return 1;
  }

  printf("completed: %u provisional rows all verified\n", printer.rows_seen);
  lodepng_row_decoder_destroy(decoder);
  lodepng_free(png);
  free(pixels);
  return 0;
}
