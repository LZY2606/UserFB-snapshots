#ifndef TASK_IT_FIXTURE_H
#define TASK_IT_FIXTURE_H
/* A normal signed, uncompressed mono IT sample and one ordinary pattern.
   Author material only: no proposed loop processing or reference algorithm. */
#include <stddef.h>
#include <string.h>
#include <stdint.h>
static void fixture_u16(unsigned char *p, unsigned n){p[0]=(unsigned char)n;p[1]=(unsigned char)(n>>8);}
static void fixture_u32(unsigned char *p, unsigned n){fixture_u16(p,n);fixture_u16(p+2,n>>16);}
static size_t fixture_it(unsigned char *b, size_t cap, int release_row) {
    enum {HEADER=192,SAMPLE=208,PATTERN=288,DATA=512,LENGTH=256};
    unsigned row, pos=PATTERN+8, packed;
    if(cap<DATA+LENGTH || release_row>31)return 0;
    memset(b,0,DATA+LENGTH);
    memcpy(b,"IMPM",4);memcpy(b+4,"ordinary sustain",16);
    fixture_u16(b+32,2); /* orders: pattern 0 then end */
    fixture_u16(b+36,1);fixture_u16(b+38,1);
    fixture_u16(b+40,0x214);fixture_u16(b+42,0x214);
    fixture_u16(b+44,1); /* stereo song, sample mode */
    b[48]=128;b[49]=48;b[50]=1;b[51]=125;b[52]=128;
    memset(b+64,32,64);memset(b+128,64,64);
    b[192]=0;b[193]=255;
    fixture_u32(b+194,SAMPLE);fixture_u32(b+198,PATTERN);
    memcpy(b+SAMPLE,"IMPS",4);b[SAMPLE+17]=64;
    b[SAMPLE+18]=1|32; /* exists, forward sustain; no regular loop */
    b[SAMPLE+19]=64;b[SAMPLE+46]=1;b[SAMPLE+47]=32|128;
    fixture_u32(b+SAMPLE+48,LENGTH);fixture_u32(b+SAMPLE+60,6000);
    fixture_u32(b+SAMPLE+64,32);fixture_u32(b+SAMPLE+68,160);
    fixture_u32(b+SAMPLE+72,DATA);
    for(row=0;row<32;row++) {
        if(row==0){b[pos++]=0x81;b[pos++]=3;b[pos++]=60;b[pos++]=1;}
        else if((int)row==release_row){b[pos++]=0x81;b[pos++]=1;b[pos++]=255;}
        b[pos++]=0;
    }
    packed=pos-(PATTERN+8);fixture_u16(b+PATTERN,packed);fixture_u16(b+PATTERN+2,32);
    for(row=0;row<LENGTH;row++) {
        int n=(int)((row*37u+row*row*3u)%151u)-75;
        b[DATA+row]=(unsigned char)(signed char)n;
    }
    return DATA+LENGTH;
}

/* Flexible builder used by the sustain-crossfade tests. It can produce 8/16
 * bit, mono/stereo samples with any combination of forward/ping-pong sustain
 * loops and normal loops, an optional instrument (so NNA can be exercised),
 * several note rows and a note-off row. Author material only: it never applies
 * any crossfade itself.
 */
struct fixture_opts {
    int bits;          /* 8 or 16 */
    int stereo;        /* 0 or 1 */
    int sus_flags;     /* IT sample flag bits for the sustain/normal loops */
    long length, sus_start, sus_end, loop_start, loop_end;
    int note_row;      /* first note row, -1 for none */
    int note2_row;     /* second note row (same channel/sample), -1 for none */
    int release_row;   /* note-off row, -1 for none */
    int release2_row;  /* a second note-off row (repeat), -1 for none */
    int nna;           /* instrument NNA, -1 for no instrument at all */
    int c5;            /* C5 speed */
};

static size_t fixture_instrument(unsigned char *b, size_t at, int nna) {
    size_t p = at, n;
    fixture_u32(b + p, 0x494D5049u); p += 4;       /* "IMPI" */
    memset(b + p, 0, 13); p += 13;                  /* filename */
    b[p++] = (unsigned char)nna;                    /* new note action */
    b[p++] = 0;                                     /* dup check type */
    b[p++] = 0;                                     /* dup check action */
    fixture_u16(b + p, 1024); p += 2;               /* fadeout */
    b[p++] = 0; b[p++] = 60;                        /* pp separation/centre */
    b[p++] = 128; b[p++] = 32;                      /* global vol, pan */
    b[p++] = 0; b[p++] = 0;                         /* random vol/pan */
    memset(b + p, 0, 4); p += 4;                    /* tracker version etc. */
    memset(b + p, 0, 26); p += 26;                  /* name */
    b[p++] = 127; b[p++] = 0;                       /* filter cutoff/res */
    memset(b + p, 0, 4); p += 4;                    /* MIDI */
    for (n = 0; n < 120; n++) b[p++] = (unsigned char)n;      /* map note */
    for (n = 0; n < 120; n++) { fixture_u16(b + p, 1); p += 2; } /* sample */
    /* Three empty envelopes: 6 header bytes, zero nodes, 76 skip bytes. */
    for (n = 0; n < 3; n++) {
        memset(b + p, 0, 6); p += 6;
        memset(b + p, 0, 76); p += 76;
    }
    return p - at;
}

static size_t fixture_it_x(unsigned char *b, size_t cap,
                           const struct fixture_opts *o) {
    enum { HEADER = 192, ORDERS = 2, ROWS = 32, SAMPLE_HDR = 208,
           PAT_HDR = 8, INST_SIZE = 670 };
    int has_inst = o->nna >= 0;
    size_t table_off, inst_off, pat_off, sh_off, data_off, end, pat_cap;
    size_t pos, packed;
    unsigned row;
    long frame;
    int ch, frame_bytes = (o->bits / 8) * (o->stereo ? 2 : 1);

    if (o->bits != 8 && o->bits != 16) return 0;
    if (o->note2_row >= ROWS || o->release_row >= ROWS ||
        o->release2_row >= ROWS || o->note_row >= ROWS)
        return 0;

    /* Linear layout: header plus order list, offset table, instrument,
     * pattern, sample header, sample data. The component offset table begins
     * right after the two order bytes. */
    table_off = HEADER + ORDERS;
    inst_off = table_off + 4u * (size_t)(has_inst ? 3 : 2);
    pat_off = inst_off + (has_inst ? INST_SIZE : 0);
    pat_cap = PAT_HDR + ROWS * 16;
    sh_off = pat_off + pat_cap;
    data_off = sh_off + SAMPLE_HDR;
    end = data_off + (size_t)o->length * frame_bytes;
    if (end > cap) return 0;
    memset(b, 0, end);

    memcpy(b, "IMPM", 4);
    memcpy(b + 4, "xfade sustain", 14);
    fixture_u16(b + 32, ORDERS);                            /* n_orders */
    fixture_u16(b + 34, (unsigned short)(has_inst ? 1 : 0)); /* instruments */
    fixture_u16(b + 36, 1);                                /* samples */
    fixture_u16(b + 38, 1);                                /* patterns */
    fixture_u16(b + 40, 0x214);                            /* cwt */
    fixture_u16(b + 42, 0x214);                            /* cmwt */
    fixture_u16(b + 44, 1);                                /* flags: stereo */
    b[48] = 128; b[49] = 48; b[50] = 1; b[51] = 125; b[52] = 128;
    memset(b + 64, 32, 64);
    memset(b + 128, 64, 64);
    b[HEADER] = 0; b[HEADER + 1] = 255;             /* order list: 0, end */

    /* Offset table: absolute file positions, instrument first if present. */
    if (has_inst) {
        fixture_u32(b + table_off, inst_off);
        fixture_u32(b + table_off + 4, sh_off);
        fixture_u32(b + table_off + 8, pat_off);
    } else {
        fixture_u32(b + table_off, sh_off);
        fixture_u32(b + table_off + 4, pat_off);
    }

    if (has_inst) {
        if (fixture_instrument(b, inst_off, o->nna) != INST_SIZE) return 0;
    }

    /* Sample header. All loop flags live in the flags byte at offset 18; the
     * convert byte (46) selects signed linear PCM and 47 is the default pan. */
    memcpy(b + sh_off, "IMPS", 4);
    b[sh_off + 17] = 64;
    b[sh_off + 18] = (unsigned char)(1 | o->sus_flags);
    if (o->bits == 16) b[sh_off + 18] |= 2;
    if (o->stereo) b[sh_off + 18] |= 4;
    b[sh_off + 19] = 64;
    b[sh_off + 46] = 1;
    b[sh_off + 47] = 32 | 128;
    fixture_u32(b + sh_off + 48, o->length);
    fixture_u32(b + sh_off + 52, o->loop_start);
    fixture_u32(b + sh_off + 56, o->loop_end);
    fixture_u32(b + sh_off + 60, o->c5);
    fixture_u32(b + sh_off + 64, o->sus_start);
    fixture_u32(b + sh_off + 68, o->sus_end);
    fixture_u32(b + sh_off + 72, data_off);

    /* Pattern: pack rows. */
    pos = pat_off + PAT_HDR;
    for (row = 0; row < ROWS; row++) {
        if ((int)row == o->note_row || (int)row == o->note2_row) {
            b[pos++] = 0x81; b[pos++] = 3; b[pos++] = 60; b[pos++] = 1;
        } else if ((int)row == o->release_row ||
                   (int)row == o->release2_row) {
            b[pos++] = 0x81; b[pos++] = 1; b[pos++] = 255;
        }
        b[pos++] = 0;
    }
    packed = pos - (pat_off + PAT_HDR);
    fixture_u16(b + pat_off, packed);
    fixture_u16(b + pat_off + 2, ROWS);

    /* Deterministic PCM, distinct per frame so loops/blends are observable. */
    for (frame = 0; frame < o->length; frame++) {
        int n = (int)((frame * 37u + frame * frame * 3u) % 151u) - 75;
        for (ch = 0; ch < (o->stereo ? 2 : 1); ch++) {
            int v = n + ch * 17 - 8;
            size_t idx = (size_t)frame * (o->stereo ? 2 : 1) + ch;
            if (o->bits == 16) {
                short sv = (short)(v * 257);
                b[data_off + idx * 2] = (unsigned char)(sv & 255);
                b[data_off + idx * 2 + 1] = (unsigned char)((sv >> 8) & 255);
            } else {
                b[data_off + idx] = (unsigned char)(signed char)v;
            }
        }
    }
    return end;
}
#endif
