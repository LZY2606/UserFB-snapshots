/*
 * regress/ppm_image.c
 *
 * Binary PPM writers used by the fill regression diagnostics.
 */
#include "ppm_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ppm_write_argb(const char* filename, const uint32_t* argb,
    int width, int height, int stride)
{
    FILE* f = fopen(filename, "wb");
    if(!f) {
        perror(filename);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    unsigned char row[3];
    for(int y = 0; y < height; y++) {
        const uint32_t* p = (const uint32_t*)((const unsigned char*)argb +
            (size_t)y * (size_t)stride);
        for(int x = 0; x < width; x++) {
            uint32_t v = p[x];
            row[0] = (unsigned char)((v >> 16) & 0xff);
            row[1] = (unsigned char)((v >> 8) & 0xff);
            row[2] = (unsigned char)(v & 0xff);
            fwrite(row, 1, 3, f);
        }
    }
    fclose(f);
}

static uint8_t clamp_byte(int v)
{
    if(v < 0) return 0;
    if(v > 255) return 255;
    return (uint8_t)v;
}

void ppm_write_triptych(const char* filename, const uint8_t* ref,
    const uint8_t* got, int width, int height)
{
    const int gap = 4;
    const int total_w = width * 3 + gap * 2;
    FILE* f = fopen(filename, "wb");
    if(!f) {
        perror(filename);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", total_w, height);

    unsigned char* line = (unsigned char*)malloc((size_t)total_w * 3);
    for(int y = 0; y < height; y++) {
        size_t pos = 0;
        for(int tile = 0; tile < 3; tile++) {
            if(tile)
                for(int g = 0; g < gap; g++)
                    line[pos++] = 40, line[pos++] = 40, line[pos++] = 40;
            for(int x = 0; x < width; x++) {
                int idx = y * width + x;
                int gray;
                if(tile == 0)
                    gray = ref[idx];
                else if(tile == 1)
                    gray = got[idx];
                else {
                    int d = (int)got[idx] - (int)ref[idx];
                    if(d < 0) d = -d;
                    gray = clamp_byte(d * 4);
                }
                line[pos++] = (unsigned char)gray;
                line[pos++] = (unsigned char)gray;
                line[pos++] = (unsigned char)gray;
            }
        }
        fwrite(line, 1, (size_t)total_w * 3, f);
    }
    free(line);
    fclose(f);
}
