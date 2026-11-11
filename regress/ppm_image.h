/*
 * regress/ppm_image.h
 *
 * Minimal binary PPM (P6) and raw coverage dump helpers for the fill
 * regression.  No external image libraries are used.
 */
#ifndef REGRESS_PPM_IMAGE_H
#define REGRESS_PPM_IMAGE_H

#include <stdint.h>

typedef struct {
    int width;
    int height;
    uint32_t* argb; /* native-endian premultiplied ARGB, matches the surface */
} ppm_canvas_t;

void ppm_write_argb(const char* filename, const uint32_t* argb,
    int width, int height, int stride);

/*
 * Writes a side-by-side diagnostic strip: reference coverage, product
 * coverage, and a 4x amplified absolute difference, each as grayscale over a
 * transparent checkerboard.  width/height describe one tile.
 */
void ppm_write_triptych(const char* filename, const uint8_t* ref,
    const uint8_t* got, int width, int height);

#endif /* REGRESS_PPM_IMAGE_H */
