/*
 * Renders the perspective-correction before/after comparison scene and writes
 * it as a binary PPM.
 *
 * Two textured triangles are drawn with a real glFrustum projection so the
 * homogeneous w varies strongly across each face:
 *
 *   left  (screen x 2..15): w = 1.2 / 6.0 / 1.5
 *   right (screen x 17..30): w = 1.0 / 8.0 / 2.0
 *
 * A fixed nearest/repeat procedural RGB texture is used (texel (x,y)=(x,y,3)),
 * so any texture coordinate drift shows up as a colour shift.
 *
 * Build it against any version of the library:
 *   cc -std=c99 -O2 -Iinclude checks/examples/perspective_ppm.c \
 *      <libtinygl.a> -lm -o perspective_ppm
 *   ./perspective_ppm before.ppm
 *
 * Run once against the unfixed tree and once against the fixed tree; the two
 * PPMs (checks/examples/before.ppm and after.ppm) are the reference pair.
 */
#include "zbuffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FB_W 64
#define FB_H 64
#define TEX 256

static unsigned char rgb[TEX * TEX * 3];

static void make_texture(void) {
    for (int y = 0; y < TEX; y++)
        for (int x = 0; x < TEX; x++) {
            unsigned char* p = &rgb[((size_t)y * TEX + x) * 3];
            p[0] = (unsigned char)x;
            p[1] = (unsigned char)y;
            p[2] = 3;
        }
    /* a few coloured bands so nearest drift is visible at a glance too */
    for (int y = 0; y < TEX; y++)
        for (int x = 0; x < TEX; x++) {
            if (((x >> 5) & 1) == 0 && ((y >> 5) & 1) == 0) {
                unsigned char* p = &rgb[((size_t)y * TEX + x) * 3];
                p[2] = 200;
            }
        }
}

/* Convert an integer screen target to clip-space x for a given w. The viewport
 * is the full framebuffer: scale=(dim-0.5)/2, trans=(dim-0.5)/2. The +0.25
 * keeps the float projection just inside the target after truncation. */
static float clip_x(int sx, float w) {
    float scale = (FB_W - 0.5f) / 2.0f;
    float trans = (FB_W - 0.5f) / 2.0f;
    return (((float)sx + 0.25f - trans) / scale) * w;
}
static float clip_y(int sy, float w) {
    float scale = -(FB_H - 0.5f) / 2.0f;
    float trans = (FB_H - 0.5f) / 2.0f;
    return (((float)sy + 0.25f - trans) / scale) * w;
}

static void draw_tri(ZBuffer* z, int sx0, int sy0, int sx1, int sy1, int sx2, int sy2,
                     float w0, float w1, float w2) {
    /* The frustum maps model.z = -w to clip w. z is chosen so the whole
     * triangle is inside the depth range: clip z = -(11/9)*(-w) - 20/9. */
    glBegin(GL_TRIANGLES);
    glTexCoord2f(0, 0);
    glVertex3f(clip_x(sx0, w0), clip_y(sy0, w0), -w0);
    glTexCoord2f(1, 0);
    glVertex3f(clip_x(sx1, w1), clip_y(sy1, w1), -w1);
    glTexCoord2f(0, 1);
    glVertex3f(clip_x(sx2, w2), clip_y(sy2, w2), -w2);
    glEnd();
    (void)z;
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "perspective.ppm";
    make_texture();

    ZBuffer* z = ZB_open(FB_W, FB_H, ZB_MODE_RGBA, 0);
    if (!z) {
        fprintf(stderr, "ZB_open failed\n");
        return 1;
    }
    glInit(z);
    glViewport(0, 0, FB_W, FB_H);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glColor3f(1, 1, 1);
    glShadeModel(GL_SMOOTH);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1, 1, -1, 1, 1, 10);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, 3, TEX, TEX, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);

    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    draw_tri(z, 2, 56, 30, 56, 2, 8, 1.2f, 6.0f, 1.5f);
    draw_tri(z, 34, 8, 62, 54, 34, 56, 1.0f, 8.0f, 2.0f);

    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        glClose();
        ZB_close(z);
        return 1;
    }
    fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
    /* The framebuffer is bottom-up; write it top-down. */
    for (int y = FB_H - 1; y >= 0; y--) {
        for (int x = 0; x < FB_W; x++) {
            unsigned p = ((PIXEL*)z->pbuf)[(size_t)y * FB_W + x];
            unsigned char out[3] = {(unsigned char)((p >> 16) & 0xff), (unsigned char)((p >> 8) & 0xff),
                                    (unsigned char)(p & 0xff)};
            fwrite(out, 1, 3, f);
        }
    }
    fclose(f);

    glClose();
    ZB_close(z);
    return 0;
}
