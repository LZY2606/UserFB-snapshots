#ifndef TASK_SF2_FIXTURE_H
#define TASK_SF2_FIXTURE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal in-memory SoundFont fixture, built through a temporary RIFF stream.
 * All sample values below are original deterministic signals (CC0-1.0).
 * mode: 0=mono; 1=zone references left; 2=zone references right.
 * The returned bytes are independent of TSF and can be edited by C tests. */
struct task_font_bytes { unsigned char *data; int size; };
static void tf_uint(FILE *f, unsigned x, int n) {
    while (n--) { fputc(x & 255, f); x >>= 8; }
}
static long tf_begin(FILE *f, const char *id) {
    long p;
    fwrite(id, 1, 4, f); p = ftell(f); tf_uint(f, 0, 4); return p;
}
static void tf_end(FILE *f, long p) {
    long e = ftell(f);
    fseek(f, p, SEEK_SET); tf_uint(f, (unsigned)(e-p-4), 4);
    fseek(f, e, SEEK_SET); if ((e-p-4) & 1) fputc(0, f);
}
static void tf_name(FILE *f, const char *s) {
    char b[20] = {0}; strncpy(b, s, 19); fwrite(b, 1, 20, f);
}
static void tf_gen(FILE *f, unsigned op, int amount) {
    tf_uint(f, op, 2); tf_uint(f, (unsigned)(unsigned short)amount, 2);
}
static int task_pcm(int side, int i) {
    return side ? ((i*97)%193-96)*110 : ((i*73)%211-105)*100;
}
static struct task_font_bytes task_make_font(int mode) {
    struct task_font_bytes result = {NULL, 0};
    FILE *f = tmpfile(); long riff, list, c, n; int i, s;
    if (!f) return result;
    riff = tf_begin(f, "RIFF"); fwrite("sfbk", 1, 4, f);
    list = tf_begin(f, "LIST"); fwrite("INFO", 1, 4, f);
    c = tf_begin(f, "ifil"); tf_uint(f, 2, 2); tf_uint(f, 1, 2); tf_end(f, c);
    tf_end(f, list);
    list = tf_begin(f, "LIST"); fwrite("sdta", 1, 4, f);
    c = tf_begin(f, "smpl");
    for (s = 0; s < 2; ++s) {
        for (i = 0; i < 512; ++i) tf_uint(f, (unsigned)(unsigned short)task_pcm(s, i), 2);
        for (i = 0; i < 46; ++i) tf_uint(f, 0, 2);
    }
    tf_end(f, c); tf_end(f, list);
    list = tf_begin(f, "LIST"); fwrite("pdta", 1, 4, f);
    c = tf_begin(f, "phdr");
    for (i = 0; i < 2; ++i) {
        tf_name(f, i ? "EOP" : "Original waves");
        tf_uint(f, 0, 2); tf_uint(f, 0, 2); tf_uint(f, i, 2);
        tf_uint(f, 0, 4); tf_uint(f, 0, 4); tf_uint(f, 0, 4);
    }
    tf_end(f, c);
    c = tf_begin(f, "pbag");
    for (i = 0; i < 2; ++i) { tf_uint(f, i, 2); tf_uint(f, 0, 2); } tf_end(f, c);
    c = tf_begin(f, "pmod"); for (i = 0; i < 10; ++i) fputc(0, f); tf_end(f, c);
    c = tf_begin(f, "pgen"); tf_gen(f, 41, 0); tf_gen(f, 0, 0); tf_end(f, c);
    c = tf_begin(f, "inst"); tf_name(f, "Single region"); tf_uint(f, 0, 2);
    tf_name(f, "EOI"); tf_uint(f, 1, 2); tf_end(f, c);
    c = tf_begin(f, "ibag"); tf_uint(f, 0, 2); tf_uint(f, 0, 2);
    tf_uint(f, 7, 2); tf_uint(f, 0, 2); tf_end(f, c);
    c = tf_begin(f, "imod"); for (i = 0; i < 10; ++i) fputc(0, f); tf_end(f, c);
    c = tf_begin(f, "igen");
    tf_gen(f, 8, 13500); /* bypass filter in the initial witness */
    tf_gen(f, 17, 0); tf_gen(f, 34, -12000); tf_gen(f, 38, -1200);
    tf_gen(f, 54, 1); tf_gen(f, 57, 0); tf_gen(f, 53, mode == 2 ? 1 : 0);
    tf_gen(f, 0, 0); tf_end(f, c);
    c = tf_begin(f, "shdr");
    for (i = 0; i < 3; ++i) {
        unsigned st = (unsigned)i*558;
        tf_name(f, i == 2 ? "EOS" : (i ? "Right" : "Left"));
        tf_uint(f, st, 4); tf_uint(f, i == 2 ? st : st+512, 4);
        tf_uint(f, i == 2 ? st : st+16, 4); tf_uint(f, i == 2 ? st : st+496, 4);
        tf_uint(f, 22050, 4); tf_uint(f, 60, 1); tf_uint(f, 0, 1);
        tf_uint(f, i == 0 ? 1 : 0, 2);
        tf_uint(f, !mode || i == 2 ? 1 : (i ? 2 : 4), 2);
    }
    tf_end(f, c); tf_end(f, list); tf_end(f, riff);
    n = ftell(f);
    if (n <= 0 || n > 1000000 || ferror(f)) { fclose(f); return result; }
    result.data = (unsigned char*)malloc((size_t)n);
    if (result.data) {
        rewind(f);
        if (fread(result.data, 1, (size_t)n, f) == (size_t)n) result.size = (int)n;
        else { free(result.data); result.data = NULL; }
    }
    fclose(f); return result;
}
#endif
