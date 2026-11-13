/*  _______         ____    __         ___    ___
 * \    _  \       \    /  \  /       \   \  /   /       '   '  '
 *  |  | \  \       |  |    ||         |   \/   |         .      .
 *  |  |  |  |      |  |    ||         ||\  /|  |
 *  |  |  |  |      |  |    ||         || \/ |  |         '  '  '
 *  |  |  |  |      |  |    ||         ||    |  |         .      .
 *  |  |_/  /        \  \__//          ||    |  |
 * /_______/ynamic    \____/niversal  /__\  /____\usic   /|  .  . ibliotheque
 *                                                      /  \
 *                                                     / .  \
 * itread.c - Code to read an Impulse Tracker         / / \  \
 *            module from an open file.              | <  /   \_
 *                                                   |  \/ /\   /
 * Based on the loader from an IT player by Bob.      \_  /  > /
 * Adapted for DUMB by entheh.                          | \ / /
 *                                                      |  ' /
 *                                                       \__/
 */

#include <stdlib.h>
#include <string.h> //might not be necessary later; required for memset

#include "dumb.h"
#include "internal/it.h"

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

//#define INVESTIGATE_OLD_INSTRUMENTS

typedef unsigned char byte;
typedef unsigned short word;
typedef unsigned long dword;

typedef struct readblock_crap readblock_crap;

struct readblock_crap {
    unsigned char *sourcebuf;
    size_t sourceend; /* size in bytes */
    size_t bitpos;    /* bits consumed, LSB first within each byte */
    int error;
};

/* Test-only observer. It is called with the size of every buffer allocated
 * while reading IT sample data, so tests can prove that a truncated sample
 * does not allocate for the discarded part of the file. */
void (*_dumb_it_sample_alloc_hook)(size_t) = NULL;

static void *it_sample_malloc(size_t size) {
    if (_dumb_it_sample_alloc_hook)
        _dumb_it_sample_alloc_hook(size);
    return malloc(size);
}

/* Read the 16-bit length-prefixed body of one compressed block. The length
 * delimits exactly how many body bytes this block owns; it is not a hint that
 * may be padded past the end of the sample's source range. A block with no
 * body is invalid. */
static int readblock(DUMBFILE *f, readblock_crap *crap, dumb_off_t end) {
    int size;
    int c;
    dumb_off_t pos;

    pos = dumbfile_pos(f);
    if (pos < 0)
        return -1;
    if (end >= 0 && (pos >= end || end - pos < 2))
        return -1;

    size = dumbfile_igetw(f);
    if (size <= 0 || dumbfile_error(f))
        return -1;

    pos = dumbfile_pos(f);
    if (pos < 0)
        return -1;
    if (end >= 0 && ((dumb_off_t)size > end || pos > end - size))
        return -1;

    crap->sourcebuf = it_sample_malloc((size_t)size);
    if (!crap->sourcebuf)
        return -1;

    c = (int)dumbfile_getnc((char *)crap->sourcebuf, (size_t)size, f);
    if (c < size) {
        free(crap->sourcebuf);
        crap->sourcebuf = NULL;
        return -1;
    }

    crap->sourceend = (size_t)size;
    crap->bitpos = 0;
    crap->error = 0;
    return 0;
}

static void freeblock(readblock_crap *crap) {
    free(crap->sourcebuf);
    crap->sourcebuf = NULL;
}

/* Read exactly bitwidth bits. Returns -1 if the block does not contain
 * enough bits. A legitimately decoded zero is returned as 0, so callers can
 * tell a zero value apart from an exhausted block. */
static int readbits(int bitwidth, readblock_crap *crap) {
    int val = 0;
    size_t bit, nbits;
    int i;

    if (crap->error || bitwidth <= 0 || bitwidth > 17) {
        crap->error = 1;
        return -1;
    }

    nbits = crap->sourceend << 3;
    if (crap->bitpos > nbits ||
        (size_t)bitwidth > nbits - crap->bitpos) {
        crap->error = 1;
        return -1;
    }

    bit = crap->bitpos;
    for (i = 0; i < bitwidth; i++, bit++)
        val |= ((crap->sourcebuf[bit >> 3] >> (bit & 7)) & 1) << i;
    crap->bitpos = bit;

    return val;
}

/* Decode one channel of an 8-bit compressed IT sample. The file holds
 * `disk_len` frames, but only the first `retain` frames are written to
 * `data` (with `stride` and `offset` selecting the channel in the output
 * buffer). The rest of every block is still decoded and validated, so a
 * corrupt tail rejects the whole module instead of being silently skipped.
 *
 * Each block starts a fresh bit reader and integration history. `end` is the
 * end of this sample's source range; a block may not read past it. */
static int decompress8(DUMBFILE *f, dumb_off_t end, long disk_len, long retain,
                       signed char *data, int stride, int offset, int it215) {
    long produced = 0;
    readblock_crap crap;

    memset(&crap, 0, sizeof(crap));

    while (produced < disk_len) {
        long blocklen, blockpos;
        int bitwidth, val;

        if (readblock(f, &crap, end))
            return -1;

        blocklen = disk_len - produced;
        if (blocklen > 0x8000)
            blocklen = 0x8000; /* max 32768 frames per block */

        blockpos = 0;
        bitwidth = 9;
        {
            signed char d1 = 0, d2 = 0;

            while (blockpos < blocklen) {
                int newwidth;

                val = readbits(bitwidth, &crap);
                if (val < 0) {
                    freeblock(&crap);
                    return -1;
                }

                /* Control codes change the bit width and produce no PCM. */
                if (bitwidth < 7) { /* Method 1 */
                    if (val == (1 << (bitwidth - 1))) {
                        int operand = readbits(3, &crap);
                        if (operand < 0) {
                            freeblock(&crap);
                            return -1;
                        }
                        newwidth = operand + 1;
                        newwidth = (newwidth < bitwidth) ? newwidth
                                                         : newwidth + 1;
                        if (newwidth < 1 || newwidth > 9) {
                            freeblock(&crap);
                            return -1;
                        }
                        bitwidth = newwidth;
                        continue;
                    }
                } else if (bitwidth < 9) { /* Method 2 */
                    byte border = (byte)((0xFF >> (9 - bitwidth)) - 4);

                    if (val > border && val <= (border + 8)) {
                        newwidth = val - border;
                        newwidth = (newwidth < bitwidth) ? newwidth
                                                         : newwidth + 1;
                        if (newwidth < 1 || newwidth > 9) {
                            freeblock(&crap);
                            return -1;
                        }
                        bitwidth = newwidth;
                        continue;
                    }
                } else if (bitwidth == 9) { /* Method 3 */
                    if (val & 0x100) {
                        newwidth = (val + 1) & 0xFF;
                        if (newwidth < 1 || newwidth > 9) {
                            freeblock(&crap);
                            return -1;
                        }
                        bitwidth = newwidth;
                        continue;
                    }
                } else { /* Illegal width */
                    freeblock(&crap);
                    return -1;
                }

                {
                    int v;
                    if (bitwidth < 8) {
                        /* Sign-extend without shifting into the sign bit. */
                        int sign = 1 << (bitwidth - 1);
                        v = (val & sign) ? val - (sign << 1) : val;
                    } else
                        v = (signed char)val;

                    d1 = (signed char)(d1 + v);
                    d2 = (signed char)(d2 + d1);
                }

                if (produced + blockpos < retain)
                    data[(produced + blockpos) * stride + offset] =
                        it215 ? d2 : d1;
                blockpos++;
            }
        }

        freeblock(&crap);
        produced += blocklen;
    }

    return 0;
}

static int decompress16(DUMBFILE *f, dumb_off_t end, long disk_len, long retain,
                        short *data, int stride, int offset, int it215) {
    long produced = 0;
    readblock_crap crap;

    memset(&crap, 0, sizeof(crap));

    while (produced < disk_len) {
        long blocklen, blockpos;
        int bitwidth;
        long val;

        if (readblock(f, &crap, end))
            return -1;

        blocklen = disk_len - produced;
        if (blocklen > 0x4000)
            blocklen = 0x4000; /* max 16384 frames per block */

        blockpos = 0;
        bitwidth = 17;
        {
            signed short d1 = 0, d2 = 0;

            while (blockpos < blocklen) {
                int newwidth;

                val = readbits(bitwidth, &crap);
                if (val < 0) {
                    freeblock(&crap);
                    return -1;
                }

                if (bitwidth < 7) { /* Method 1 */
                    if (val == (1 << (bitwidth - 1))) {
                        int operand = readbits(4, &crap);
                        if (operand < 0) {
                            freeblock(&crap);
                            return -1;
                        }
                        newwidth = operand + 1;
                        newwidth = (newwidth < bitwidth) ? newwidth
                                                         : newwidth + 1;
                        if (newwidth < 1 || newwidth > 17) {
                            freeblock(&crap);
                            return -1;
                        }
                        bitwidth = newwidth;
                        continue;
                    }
                } else if (bitwidth < 17) { /* Method 2 */
                    word border =
                        (word)((0xFFFF >> (17 - bitwidth)) - 8);

                    if (val > border && val <= (border + 16)) {
                        newwidth = (int)(val - border);
                        newwidth = (newwidth < bitwidth) ? newwidth
                                                         : newwidth + 1;
                        if (newwidth < 1 || newwidth > 17) {
                            freeblock(&crap);
                            return -1;
                        }
                        bitwidth = newwidth;
                        continue;
                    }
                } else if (bitwidth == 17) { /* Method 3 */
                    if (val & 0x10000) {
                        newwidth = (int)((val + 1) & 0xFF);
                        if (newwidth < 1 || newwidth > 17) {
                            freeblock(&crap);
                            return -1;
                        }
                        bitwidth = newwidth;
                        continue;
                    }
                } else { /* Illegal width */
                    freeblock(&crap);
                    return -1;
                }

                {
                    short v;
                    if (bitwidth < 16) {
                        /* Sign-extend without shifting into the sign bit. */
                        int sign = 1 << (bitwidth - 1);
                        v = (short)((val & sign) ? val - (sign << 1) : val);
                    } else
                        v = (short)val;

                    d1 = (short)(d1 + v);
                    d2 = (short)(d2 + d1);
                }

                if (produced + blockpos < retain)
                    data[(produced + blockpos) * stride + offset] =
                        it215 ? d2 : d1;
                blockpos++;
            }
        }

        freeblock(&crap);
        produced += blocklen;
    }

    return 0;
}

static int it_read_envelope(IT_ENVELOPE *envelope, DUMBFILE *f) {
    int n;

    envelope->flags = dumbfile_getc(f);
    envelope->n_nodes = dumbfile_getc(f);
    if (envelope->n_nodes > 25) {
        TRACE("IT error: wrong number of envelope nodes (%d)\n",
              envelope->n_nodes);
        envelope->n_nodes = 0;
        return -1;
    }
    envelope->loop_start = dumbfile_getc(f);
    envelope->loop_end = dumbfile_getc(f);
    envelope->sus_loop_start = dumbfile_getc(f);
    envelope->sus_loop_end = dumbfile_getc(f);
    for (n = 0; n < envelope->n_nodes; n++) {
        envelope->node_y[n] = dumbfile_getc(f);
        envelope->node_t[n] = dumbfile_igetw(f);
    }
    dumbfile_skip(f, 75 - envelope->n_nodes * 3 + 1);

    if (envelope->n_nodes <= 0)
        envelope->flags &= ~IT_ENVELOPE_ON;
    else {
        if (envelope->loop_end >= envelope->n_nodes ||
            envelope->loop_start > envelope->loop_end)
            envelope->flags &= ~IT_ENVELOPE_LOOP_ON;
        if (envelope->sus_loop_end >= envelope->n_nodes ||
            envelope->sus_loop_start > envelope->sus_loop_end)
            envelope->flags &= ~IT_ENVELOPE_SUSTAIN_LOOP;
    }

    return dumbfile_error(f);
}

static int it_read_old_instrument(IT_INSTRUMENT *instrument, DUMBFILE *f) {
    int n;

    /*if (dumbfile_mgetl(f) != IT_INSTRUMENT_SIGNATURE)
            return -1;*/
    // XXX
    dumbfile_skip(f, 4);

    dumbfile_getnc((char *)instrument->filename, 13, f);
    instrument->filename[13] = 0;

    instrument->volume_envelope.flags = dumbfile_getc(f);
    instrument->volume_envelope.loop_start = dumbfile_getc(f);
    instrument->volume_envelope.loop_end = dumbfile_getc(f);
    instrument->volume_envelope.sus_loop_start = dumbfile_getc(f);
    instrument->volume_envelope.sus_loop_end = dumbfile_getc(f);

    /* Skip two unused bytes. */
    dumbfile_skip(f, 2);

    /* In the old instrument format, fadeout ranges from 0 to 64, and is
     * subtracted at intervals from a value starting at 512. In the new
     * format, all these values are doubled. Therefore we double when loading
     * from the old instrument format - that way we don't have to think about
     * it later.
     */
    instrument->fadeout = dumbfile_igetw(f) << 1;
    instrument->new_note_action = dumbfile_getc(f);
    instrument->dup_check_type = dumbfile_getc(f);
    instrument->dup_check_action = DCA_NOTE_CUT; // This might be wrong!
    /** WARNING - what is the duplicate check action for old-style instruments?
     */

    /* Skip Tracker Version and Number of Samples. These are only used in
     * separate instrument files. Also skip unused byte.
     */
    dumbfile_skip(f, 4);

    dumbfile_getnc((char *)instrument->name, 26, f);
    instrument->name[26] = 0;

    /* Skip unused bytes following the Instrument Name. */
    dumbfile_skip(f, 6);

    instrument->pp_separation = 0;
    instrument->pp_centre = 60;
    instrument->global_volume = 128;
    /** WARNING - should global_volume be 64 or something? */
    instrument->default_pan = 32;
    /** WARNING - should default_pan be 128, meaning don`t use? */
    instrument->random_volume = 0;
    instrument->random_pan = 0;

    for (n = 0; n < 120; n++) {
        instrument->map_note[n] = dumbfile_getc(f);
        instrument->map_sample[n] = dumbfile_getc(f);
    }

    /* Skip "Volume envelope (200 bytes)". */
    // - need to know better what this is for though.
    dumbfile_skip(f, 200);

#ifdef INVESTIGATE_OLD_INSTRUMENTS
    fprintf(stderr, "Inst %02d Env:", n);
#endif

    for (n = 0; n < 25; n++) {
        instrument->volume_envelope.node_t[n] = dumbfile_getc(f);
        instrument->volume_envelope.node_y[n] = dumbfile_getc(f);

#ifdef INVESTIGATE_OLD_INSTRUMENTS
        fprintf(stderr, " %d,%d", instrument->volume_envelope.node_t[n],
                instrument->volume_envelope.node_y[n]);
#endif

        // This loop is unfinished, as we can probably escape from it before
        // the end if we want to. Hence the otherwise useless dumbfile_skip()
        // call below.
    }
    dumbfile_skip(f, 50 - (n << 1));
    instrument->volume_envelope.n_nodes = n;

#ifdef INVESTIGATE_OLD_INSTRUMENTS
    fprintf(stderr, "\n");
#endif

    if (dumbfile_error(f))
        return -1;

    {
        IT_ENVELOPE *envelope = &instrument->volume_envelope;
        if (envelope->n_nodes <= 0)
            envelope->flags &= ~IT_ENVELOPE_ON;
        else {
            if (envelope->loop_end >= envelope->n_nodes ||
                envelope->loop_start > envelope->loop_end)
                envelope->flags &= ~IT_ENVELOPE_LOOP_ON;
            if (envelope->sus_loop_end >= envelope->n_nodes ||
                envelope->sus_loop_start > envelope->sus_loop_end)
                envelope->flags &= ~IT_ENVELOPE_SUSTAIN_LOOP;
        }
    }

    instrument->filter_cutoff = 127;
    instrument->filter_resonance = 0;

    instrument->pan_envelope.flags = 0;
    instrument->pitch_envelope.flags = 0;

    return 0;
}

static int it_read_instrument(IT_INSTRUMENT *instrument, DUMBFILE *f,
                              int maxlen) {
    int n;
    long len;

    /*if (dumbfile_mgetl(f) != IT_INSTRUMENT_SIGNATURE)
            return -1;*/
    // XXX

    if (maxlen)
        len = dumbfile_pos(f);
    else
        len = 0;

    dumbfile_skip(f, 4);

    dumbfile_getnc((char *)instrument->filename, 13, f);
    instrument->filename[13] = 0;

    instrument->new_note_action = dumbfile_getc(f);
    instrument->dup_check_type = dumbfile_getc(f);
    instrument->dup_check_action = dumbfile_getc(f);
    instrument->fadeout = dumbfile_igetw(f);
    instrument->pp_separation = dumbfile_getc(f);
    instrument->pp_centre = dumbfile_getc(f);
    instrument->global_volume = dumbfile_getc(f);
    instrument->default_pan = dumbfile_getc(f);
    instrument->random_volume = dumbfile_getc(f);
    instrument->random_pan = dumbfile_getc(f);

    /* Skip Tracker Version and Number of Samples. These are only used in
     * separate instrument files. Also skip unused byte.
     */
    dumbfile_skip(f, 4);

    dumbfile_getnc((char *)instrument->name, 26, f);
    instrument->name[26] = 0;

    instrument->filter_cutoff = dumbfile_getc(f);
    instrument->filter_resonance = dumbfile_getc(f);

    /* Skip MIDI Channel, Program and Bank. */
    // dumbfile_skip(f, 4);
    /*instrument->output = dumbfile_getc(f);
    if ( instrument->output > 16 ) {
            instrument->output -= 128;
    } else {
            instrument->output = 0;
    }
    dumbfile_skip(f, 3);*/
    dumbfile_skip(f, 4);

    for (n = 0; n < 120; n++) {
        instrument->map_note[n] = dumbfile_getc(f);
        instrument->map_sample[n] = dumbfile_getc(f);
    }

    if (dumbfile_error(f))
        return -1;

    if (it_read_envelope(&instrument->volume_envelope, f))
        return -1;
    if (it_read_envelope(&instrument->pan_envelope, f))
        return -1;
    if (it_read_envelope(&instrument->pitch_envelope, f))
        return -1;

    if (maxlen) {
        len = dumbfile_pos(f) - len;
        if (maxlen - len < 124)
            return 0;
    }

    if (dumbfile_mgetl(f) == IT_MPTX_SIGNATURE) {
        for (n = 0; n < 120; n++) {
            instrument->map_sample[n] += dumbfile_getc(f) << 8;
        }

        if (dumbfile_error(f))
            return -1;
    }

    /*if ( dumbfile_mgetl(f) == IT_INSM_SIGNATURE ) {
            long end = dumbfile_igetl(f);
            end += dumbfile_pos(f);
            while ( dumbfile_pos(f) < end ) {
                    int chunkid = dumbfile_igetl(f);
                    switch ( chunkid ) {
                            case DUMB_ID('P','L','U','G'):
                                    instrument->output = dumbfile_getc(f);
                                    break;
                            default:
                                    chunkid = chunkid / 0x100 + dumbfile_getc(f)
    * 0x1000000; break;
                    }
            }

            if (dumbfile_error(f))
                    return -1;
    }*/

    return 0;
}

static int it_read_sample_header(IT_SAMPLE *sample, unsigned char *convert,
                                 long *offset, long *disk_length,
                                 DUMBFILE *f) {
    /* XXX
    if (dumbfile_mgetl(f) != IT_SAMPLE_SIGNATURE)
            return -1;*/
    int hax = 0;
    long s = dumbfile_mgetl(f);
    if (s != IT_SAMPLE_SIGNATURE) {
        if (s == (IT_SAMPLE_SIGNATURE >> 16)) {
            s <<= 16;
            s |= dumbfile_mgetw(f);
            if (s != IT_SAMPLE_SIGNATURE)
                return -1;
            hax = 1;
        }
    }

    dumbfile_getnc((char *)sample->filename, 13, f);
    sample->filename[13] = 0;

    sample->global_volume = dumbfile_getc(f);
    sample->flags = dumbfile_getc(f);
    sample->default_volume = dumbfile_getc(f);

    dumbfile_getnc((char *)sample->name, 26, f);
    sample->name[26] = 0;

    *convert = dumbfile_getc(f);
    sample->default_pan = dumbfile_getc(f);
    sample->length = dumbfile_igetl(f);
    sample->loop_start = dumbfile_igetl(f);
    sample->loop_end = dumbfile_igetl(f);
    sample->C5_speed = dumbfile_igetl(f);
    sample->sus_loop_start = dumbfile_igetl(f);
    sample->sus_loop_end = dumbfile_igetl(f);

#ifdef STEREO_SAMPLES_COUNT_AS_TWO
    if (sample->flags & IT_SAMPLE_STEREO) {
        sample->length >>= 1;
        sample->loop_start >>= 1;
        sample->loop_end >>= 1;
        sample->C5_speed >>= 1;
        sample->sus_loop_start >>= 1;
        sample->sus_loop_end >>= 1;
    }
#endif

    /* The frame count declared by the file, before any tail is dropped. The
     * public IT_SAMPLE.length is the retained frame count; decoding needs
     * this one to know how much source data the file actually holds. */
    *disk_length = sample->length;

    if (sample->flags & IT_SAMPLE_EXISTS) {
        if (sample->length <= 0)
            sample->flags &= ~IT_SAMPLE_EXISTS;
        else {
            if (sample->loop_end > sample->length ||
                sample->loop_end < 0)
                sample->flags &= ~IT_SAMPLE_LOOP;
            else if (sample->loop_start < 0 ||
                     sample->loop_start >= sample->loop_end)
                sample->flags &= ~IT_SAMPLE_LOOP;

            if (sample->sus_loop_end > sample->length ||
                sample->sus_loop_end < 0)
                sample->flags &= ~IT_SAMPLE_SUS_LOOP;
            else if (sample->sus_loop_start < 0 ||
                     sample->sus_loop_start >= sample->sus_loop_end)
                sample->flags &= ~IT_SAMPLE_SUS_LOOP;

            /* We may be able to truncate the sample to save memory. A valid
             * normal loop keeps only up to loop_end; when a valid sustain
             * loop reaches further, keep up to the larger of the two ends.
             * Without a valid normal loop the whole declared length is
             * kept. Compressed data is still read in full below so that
             * corruption in the dropped tail is detected. As before, the
             * ADPCM4 path (convert == 0xFF) is not truncated. */
            if ((sample->flags & IT_SAMPLE_LOOP) && *convert != 0xFF) {
                if ((sample->flags & IT_SAMPLE_SUS_LOOP) &&
                    sample->sus_loop_end >= sample->loop_end)
                    sample->length = sample->sus_loop_end;
                else
                    sample->length = sample->loop_end;
            }
        }
    }

    *offset = dumbfile_igetl(f);

    sample->vibrato_speed = dumbfile_getc(f);
    sample->vibrato_depth = dumbfile_getc(f);
    if (!hax) {
        sample->vibrato_rate = dumbfile_getc(f);
        sample->vibrato_waveform = dumbfile_getc(f);
    } else {
        sample->vibrato_rate = 0;
        sample->vibrato_waveform = 0;
    }
    sample->finetune = 0;
    sample->max_resampling_quality = -1;

    return dumbfile_error(f);
}

long _dumb_it_read_sample_data_adpcm4(IT_SAMPLE *sample, DUMBFILE *f) {
    long n, len, delta;
    signed char *ptr, *end;
    signed char compression_table[16];
    if (dumbfile_getnc((char *)compression_table, 16, f) != 16)
        return -1;
    ptr = (signed char *)sample->data;
    delta = 0;

    end = ptr + sample->length;
    len = (sample->length + 1) / 2;
    for (n = 0; n < len; n++) {
        int b = dumbfile_getc(f);
        if (b < 0)
            return -1;
        delta += compression_table[b & 0x0F];
        *ptr++ = delta;
        if (ptr >= end)
            break;
        delta += compression_table[b >> 4];
        *ptr++ = delta;
    }

    return 0;
}

static long it_read_sample_data(IT_SAMPLE *sample, unsigned char convert,
                                long disk_length, dumb_off_t end,
                                DUMBFILE *f) {
    long n;
    long retain = sample->length;
    int channels = (sample->flags & IT_SAMPLE_STEREO) ? 2 : 1;
    int bytes = (sample->flags & IT_SAMPLE_16BIT) ? 2 : 1;
    size_t alloc_size;

    /* Checked arithmetic: never let the declared length wrap into a small or
     * negative allocation. */
    if (disk_length <= 0 || retain < 0 || retain > disk_length)
        return -1;
    if ((size_t)retain > ((size_t)-1) / (size_t)channels ||
        (size_t)retain * (size_t)channels > ((size_t)-1) / (size_t)bytes)
        return -1;
    alloc_size = (size_t)retain * (size_t)channels * (size_t)bytes;

    sample->data = it_sample_malloc(alloc_size ? alloc_size : 1);
    if (!sample->data)
        return -1;

    if (!(sample->flags & IT_SAMPLE_16BIT) && (convert == 0xFF)) {
        if (_dumb_it_read_sample_data_adpcm4(sample, f) < 0)
            return -1;
    } else if (sample->flags & 8) {
        /* If the sample is packed, then we must unpack it. */

        /* Behavior as defined by greasemonkey's munch.py and observed by XMPlay
         * and OpenMPT */

        if (sample->flags & IT_SAMPLE_16BIT) {
            if (decompress16(f, end, disk_length, retain,
                             (short *)sample->data, channels, 0, convert & 4))
                return -1;
            if (channels == 2 && decompress16(f, end, disk_length, retain,
                                              (short *)sample->data, channels, 1,
                                              convert & 4))
                return -1;
        } else {
            if (decompress8(f, end, disk_length, retain,
                            (signed char *)sample->data, channels, 0,
                            convert & 4))
                return -1;
            if (channels == 2 && decompress8(f, end, disk_length, retain,
                                             (signed char *)sample->data,
                                             channels, 1, convert & 4))
                return -1;
        }
    } else {
        /* Uncompressed planar data: each channel physically holds every frame
         * declared on disk, even though only the retained head is kept. Read
         * (and validate) the whole left channel first, then locate the right
         * channel by the disk frame count rather than by the retained one. */
        int ch;
        for (ch = 0; ch < channels; ch++) {
            for (n = 0; n < disk_length; n++) {
                int value;
                if (end >= 0 && dumbfile_pos(f) >= end)
                    return -1;
                if (sample->flags & IT_SAMPLE_16BIT)
                    value = (convert & 2) ? dumbfile_mgetw(f) : dumbfile_igetw(f);
                else
                    value = dumbfile_getc(f);
                if (dumbfile_error(f))
                    return -1;
                if (n < retain) {
                    if (sample->flags & IT_SAMPLE_16BIT)
                        ((short *)sample->data)[n * channels + ch] =
                            (short)value;
                    else
                        ((signed char *)sample->data)[n * channels + ch] =
                            (signed char)value;
                }
            }
        }
    }

    if (dumbfile_error(f))
        return -1;

    if (!(convert & 1)) {
        /* Convert to signed. */
        long count = retain * channels;
        if (sample->flags & IT_SAMPLE_16BIT)
            for (n = 0; n < count; n++)
                ((short *)sample->data)[n] ^= 0x8000;
        else
            for (n = 0; n < count; n++)
                ((signed char *)sample->data)[n] ^= 0x80;
    }

    /* NOT SUPPORTED:
     *
     * convert &  4 - Samples stored as delta values
     * convert & 16 - Samples stored as TX-Wave 12-bit values
     * convert & 32 - Left/Right/All Stereo prompt
     */

    return 0;
}

//#define DETECT_DUPLICATE_CHANNELS
#ifdef DETECT_DUPLICATE_CHANNELS
#include <stdio.h>
#endif
static int it_read_pattern(IT_PATTERN *pattern, DUMBFILE *f,
                           unsigned char *buffer) {
    unsigned char cmask[DUMB_IT_N_CHANNELS];
    unsigned char cnote[DUMB_IT_N_CHANNELS];
    unsigned char cinstrument[DUMB_IT_N_CHANNELS];
    unsigned char cvolpan[DUMB_IT_N_CHANNELS];
    unsigned char ceffect[DUMB_IT_N_CHANNELS];
    unsigned char ceffectvalue[DUMB_IT_N_CHANNELS];
#ifdef DETECT_DUPLICATE_CHANNELS
    IT_ENTRY *dupentry[DUMB_IT_N_CHANNELS];
#endif

    int n_entries = 0;
    int buflen;
    int bufpos = 0;

    IT_ENTRY *entry;

    unsigned char channel;
    unsigned char mask;

    memset(cmask, 0, sizeof(cmask));
    memset(cnote, 0, sizeof(cnote));
    memset(cinstrument, 0, sizeof(cinstrument));
    memset(cvolpan, 0, sizeof(cvolpan));
    memset(ceffect, 0, sizeof(ceffect));
    memset(ceffectvalue, 0, sizeof(ceffectvalue));
#ifdef DETECT_DUPLICATE_CHANNELS
    {
        int i;
        for (i = 0; i < DUMB_IT_N_CHANNELS; i++)
            dupentry[i] = NULL;
    }
#endif

    buflen = dumbfile_igetw(f);
    pattern->n_rows = dumbfile_igetw(f);

    /* Skip four unused bytes. */
    dumbfile_skip(f, 4);

    if (dumbfile_error(f))
        return -1;

    /* Read in the pattern data. */
    dumbfile_getnc((char *)buffer, buflen, f);

    if (dumbfile_error(f))
        return -1;

    /* Scan the pattern data, and work out how many entries we need room for. */
    while (bufpos < buflen) {
        unsigned char b = buffer[bufpos++];

        if (b == 0) {
            /* End of row */
            n_entries++;
            continue;
        }

        channel = (b - 1) & 63;

        if (b & 128)
            cmask[channel] = mask = buffer[bufpos++];
        else
            mask = cmask[channel];

        {
            static const unsigned char used[16] = {0, 1, 1, 2, 1, 2, 2, 3,
                                                   2, 3, 3, 4, 3, 4, 4, 5};
            n_entries += (mask != 0);
            bufpos += used[mask & 15];
        }
    }

    pattern->n_entries = n_entries;

    pattern->entry = malloc(n_entries * sizeof(*pattern->entry));

    if (!pattern->entry)
        return -1;

    bufpos = 0;
    memset(cmask, 0, sizeof(cmask));

    entry = pattern->entry;

    while (bufpos < buflen) {
        unsigned char b = buffer[bufpos++];

        if (b == 0) {
            /* End of row */
            IT_SET_END_ROW(entry);
            entry++;
#ifdef DETECT_DUPLICATE_CHANNELS
            {
                int i;
                for (i = 0; i < DUMB_IT_N_CHANNELS; i++)
                    dupentry[i] = NULL;
            }
#endif
            continue;
        }

        channel = (b - 1) & 63;

        if (b & 128) {
            if (bufpos >= buflen)
                return -1;

            cmask[channel] = mask = buffer[bufpos++];
        } else
            mask = cmask[channel];

        if (mask) {
            entry->mask = (mask & 15) | (mask >> 4);
            entry->channel = channel;

            if (mask & IT_ENTRY_NOTE) {
                if (bufpos >= buflen)
                    return -1;

                cnote[channel] = entry->note = buffer[bufpos++];
            } else if (mask & (IT_ENTRY_NOTE << 4))
                entry->note = cnote[channel];

            if (mask & IT_ENTRY_INSTRUMENT) {
                if (bufpos >= buflen)
                    return -1;

                cinstrument[channel] = entry->instrument = buffer[bufpos++];
            } else if (mask & (IT_ENTRY_INSTRUMENT << 4))
                entry->instrument = cinstrument[channel];

            if (mask & IT_ENTRY_VOLPAN) {
                if (bufpos >= buflen)
                    return -1;

                cvolpan[channel] = entry->volpan = buffer[bufpos++];
            } else if (mask & (IT_ENTRY_VOLPAN << 4))
                entry->volpan = cvolpan[channel];

            if (mask & IT_ENTRY_EFFECT) {
                if (bufpos + 1 >= buflen)
                    return -1;

                ceffect[channel] = entry->effect = buffer[bufpos++];
                ceffectvalue[channel] = entry->effectvalue = buffer[bufpos++];
            } else {
                entry->effect = ceffect[channel];
                entry->effectvalue = ceffectvalue[channel];
            }

#ifdef DETECT_DUPLICATE_CHANNELS
            if (dupentry[channel]) {
                FILE *f = fopen("dupentry.txt", "a");
                if (!f)
                    abort();
                fprintf(f, "Two events on channel %d:", channel);
                fprintf(f, "  Event #1:");
                if (dupentry[channel]->mask & IT_ENTRY_NOTE)
                    fprintf(f, " %03d", dupentry[channel]->note);
                else
                    fprintf(f, " ...");
                if (dupentry[channel]->mask & IT_ENTRY_INSTRUMENT)
                    fprintf(f, " %03d", dupentry[channel]->instrument);
                else
                    fprintf(f, " ...");
                if (dupentry[channel]->mask & IT_ENTRY_VOLPAN)
                    fprintf(f, " %03d", dupentry[channel]->volpan);
                else
                    fprintf(f, " ...");
                if (dupentry[channel]->mask & IT_ENTRY_EFFECT)
                    fprintf(f, " %c%02X\n", 'A' - 1 + dupentry[channel]->effect,
                            dupentry[channel]->effectvalue);
                else
                    fprintf(f, " ...\n");
                fprintf(f, "  Event #2:");
                if (entry->mask & IT_ENTRY_NOTE)
                    fprintf(f, " %03d", entry->note);
                else
                    fprintf(f, " ...");
                if (entry->mask & IT_ENTRY_INSTRUMENT)
                    fprintf(f, " %03d", entry->instrument);
                else
                    fprintf(f, " ...");
                if (entry->mask & IT_ENTRY_VOLPAN)
                    fprintf(f, " %03d", entry->volpan);
                else
                    fprintf(f, " ...");
                if (entry->mask & IT_ENTRY_EFFECT)
                    fprintf(f, " %c%02X\n", 'A' - 1 + entry->effect,
                            entry->effectvalue);
                else
                    fprintf(f, " ...\n");
                fclose(f);
            }
            dupentry[channel] = entry;
#endif

            entry++;
        }
    }

    ASSERT(entry == pattern->entry + n_entries);

    return 0;
}

/* Currently we assume the sample data are stored after the sample headers in
 * module files. This assumption may be unjustified; let me know if you have
 * trouble.
 */

#define IT_COMPONENT_SONG_MESSAGE 1
#define IT_COMPONENT_INSTRUMENT 2
#define IT_COMPONENT_PATTERN 3
#define IT_COMPONENT_SAMPLE 4

typedef struct IT_COMPONENT {
    unsigned char type;
    unsigned short n;
    long offset;
    long data_offset; /* for samples: start of sample data, once known */
    short sampdata_valid;
} IT_COMPONENT;

static int it_component_compare(const void *e1, const void *e2) {
    long a = ((const IT_COMPONENT *)e1)->offset;
    long b = ((const IT_COMPONENT *)e2)->offset;
    return (a < b) ? -1 : (a > b) ? 1 : 0;
}

static sigdata_t *it_load_sigdata(DUMBFILE *f) {
    DUMB_IT_SIGDATA *sigdata;

    int cwt, cmwt;
    int special;
    int message_length;
    long message_offset;

    IT_COMPONENT *component;
    int min_components;
    int n_components = 0;

    unsigned char sample_convert[4096];
    long disk_length[4096];

    int n;

    unsigned char *buffer;

    if (dumbfile_mgetl(f) != IT_SIGNATURE) {
        return NULL;
    }

    sigdata = malloc(sizeof(*sigdata));

    if (!sigdata) {
        return NULL;
    }

    sigdata->song_message = NULL;
    sigdata->order = NULL;
    sigdata->instrument = NULL;
    sigdata->sample = NULL;
    sigdata->pattern = NULL;
    sigdata->midi = NULL;
    sigdata->checkpoint = NULL;

    dumbfile_getnc((char *)sigdata->name, 26, f);
    sigdata->name[26] = 0;

    /* Skip pattern row highlight info. */
    dumbfile_skip(f, 2);

    sigdata->n_orders = dumbfile_igetw(f);
    sigdata->n_instruments = dumbfile_igetw(f);
    sigdata->n_samples = dumbfile_igetw(f);
    sigdata->n_patterns = dumbfile_igetw(f);

    cwt = dumbfile_igetw(f);
    cmwt = dumbfile_igetw(f);

    sigdata->flags = dumbfile_igetw(f);
    special = dumbfile_igetw(f);

    sigdata->global_volume = dumbfile_getc(f);
    sigdata->mixing_volume = dumbfile_getc(f);
    sigdata->speed = dumbfile_getc(f);
    if (sigdata->speed == 0)
        sigdata->speed = 6; // Should we? What about tempo?
    sigdata->tempo = dumbfile_getc(f);
    sigdata->pan_separation = dumbfile_getc(f); /** WARNING: use this */

    /* Skip Pitch Wheel Depth */
    dumbfile_skip(f, 1);

    message_length = dumbfile_igetw(f);
    message_offset = dumbfile_igetl(f);

    /* Skip Reserved. */
    dumbfile_skip(f, 4);

    dumbfile_getnc((char *)sigdata->channel_pan, DUMB_IT_N_CHANNELS, f);
    dumbfile_getnc((char *)sigdata->channel_volume, DUMB_IT_N_CHANNELS, f);

    // XXX sample count
    if (dumbfile_error(f) || sigdata->n_orders <= 0 ||
        sigdata->n_orders > 1024 || // Whoa, nelly.
        sigdata->n_instruments > 256 || sigdata->n_samples > 4000 ||
        sigdata->n_patterns > 256) {
        _dumb_it_unload_sigdata(sigdata);
        return NULL;
    }

    sigdata->order = malloc(sigdata->n_orders);
    if (!sigdata->order) {
        _dumb_it_unload_sigdata(sigdata);
        return NULL;
    }

    if (sigdata->n_instruments) {
        sigdata->instrument =
            malloc(sigdata->n_instruments * sizeof(*sigdata->instrument));
        if (!sigdata->instrument) {
            _dumb_it_unload_sigdata(sigdata);
            return NULL;
        }
    }

    if (sigdata->n_samples) {
        sigdata->sample = malloc(sigdata->n_samples * sizeof(*sigdata->sample));
        if (!sigdata->sample) {
            _dumb_it_unload_sigdata(sigdata);
            return NULL;
        }
        for (n = 0; n < sigdata->n_samples; n++)
            sigdata->sample[n].data = NULL;
    }

    if (sigdata->n_patterns) {
        sigdata->pattern =
            malloc(sigdata->n_patterns * sizeof(*sigdata->pattern));
        if (!sigdata->pattern) {
            _dumb_it_unload_sigdata(sigdata);
            return NULL;
        }
        for (n = 0; n < sigdata->n_patterns; n++)
            sigdata->pattern[n].entry = NULL;
    }

    if (dumbfile_getnc((char *)sigdata->order, sigdata->n_orders, f) <
        sigdata->n_orders) {
        _dumb_it_unload_sigdata(sigdata);
        return NULL;
    }
    sigdata->restart_position = 0;

    min_components = (special & 1) + sigdata->n_instruments +
                     sigdata->n_samples + sigdata->n_patterns;

    component = malloc(min_components * sizeof(*component));
    if (!component) {
        _dumb_it_unload_sigdata(sigdata);
        return NULL;
    }

    if (special & 1) {
        component[n_components].type = IT_COMPONENT_SONG_MESSAGE;
        component[n_components].offset = message_offset;
        component[n_components].data_offset = 0;
        component[n_components].sampdata_valid = 0;
        n_components++;
    }

    for (n = 0; n < sigdata->n_instruments; n++) {
        component[n_components].type = IT_COMPONENT_INSTRUMENT;
        component[n_components].n = n;
        component[n_components].offset = dumbfile_igetl(f);
        component[n_components].data_offset = 0;
        component[n_components].sampdata_valid = 0;
        n_components++;
    }

    for (n = 0; n < sigdata->n_samples; n++) {
        component[n_components].type = IT_COMPONENT_SAMPLE;
        component[n_components].n = n;
        component[n_components].offset = dumbfile_igetl(f);
        component[n_components].data_offset = 0;
        component[n_components].sampdata_valid = 0;
        n_components++;
    }

    for (n = 0; n < sigdata->n_patterns; n++) {
        long offset = dumbfile_igetl(f);
        if (offset) {
            component[n_components].type = IT_COMPONENT_PATTERN;
            component[n_components].n = n;
            component[n_components].offset = offset;
            component[n_components].data_offset = 0;
            component[n_components].sampdata_valid = 0;
            n_components++;
        } else {
            /* Empty 64-row pattern */
            sigdata->pattern[n].n_rows = 64;
            sigdata->pattern[n].n_entries = 0;
        }
    }

    if (dumbfile_error(f)) {
        free(component);
        _dumb_it_unload_sigdata(sigdata);
        return NULL;
    }

    /* Reject out-of-range offsets before any component is sought. A negative
     * (sign-extended) or beyond-EOF offset is invalid; end-of-file itself is
     * allowed. */
    {
        dumb_off_t file_size = dumbfile_get_size(f);
        int valid = 1;
        for (n = 0; n < n_components; n++) {
            if (component[n].offset < 0 ||
                (file_size >= 0 && component[n].offset > file_size)) {
                valid = 0;
                break;
            }
        }
        if (!valid) {
            free(component);
            _dumb_it_unload_sigdata(sigdata);
            return NULL;
        }
    }

    /*
    if (!(sigdata->flags & 128) != !(special & 8)) {
            fprintf(stderr, "Flags   Bit 7 (\"Request embedded MIDI
    configuration\"): %s\n", sigdata->flags & 128 ? "=SET=" : "clear");
            fprintf(stderr, "Special Bit 3     (\"MIDI configuration embedded\")
    : %s\n", special        &   8 ? "=SET=" : "clear"); fprintf(stderr, "entheh
    would like to investigate this IT file.\n"); fprintf(stderr, "Please contact
    him! entheh@users.sf.net\n");
    }
    */

    if (special & 8) {
        /* MIDI configuration is embedded. */
        unsigned char mididata[32];
        int i;
        sigdata->midi = malloc(sizeof(*sigdata->midi));
        if (!sigdata->midi) {
            free(component);
            _dumb_it_unload_sigdata(sigdata);
            return NULL;
            // Should we be happy with this outcome in some situations?
        }
        // What are we skipping?
        i = dumbfile_igetw(f);
        if (dumbfile_error(f) || dumbfile_skip(f, 8 * i)) {
            free(component);
            _dumb_it_unload_sigdata(sigdata);
            return NULL;
        }
        /* Read embedded MIDI configuration */
        // What are the first 9 commands for?
        if (dumbfile_skip(f, 32 * 9)) {
            free(component);
            _dumb_it_unload_sigdata(sigdata);
            return NULL;
        }
        for (i = 0; i < 16; i++) {
            unsigned char len = 0;
            int j, leftdigit = -1;
            if (dumbfile_getnc((char *)mididata, 32, f) < 32) {
                free(component);
                _dumb_it_unload_sigdata(sigdata);
                return NULL;
            }
            sigdata->midi->SFmacroz[i] = 0;
            for (j = 0; j < 32; j++) {
                if (leftdigit >= 0) {
                    if (mididata[j] == 0) {
                        sigdata->midi->SFmacro[i][len++] = leftdigit;
                        break;
                    } else if (mididata[j] == ' ')
                        sigdata->midi->SFmacro[i][len++] = leftdigit;
                    else if (mididata[j] >= '0' && mididata[j] <= '9')
                        sigdata->midi->SFmacro[i][len++] =
                            (leftdigit << 4) | (mididata[j] - '0');
                    else if (mididata[j] >= 'A' && mididata[j] <= 'F')
                        sigdata->midi->SFmacro[i][len++] =
                            (leftdigit << 4) | (mididata[j] - 'A' + 0xA);
                    leftdigit = -1;
                } else if (mididata[j] == 0)
                    break;
                else if (mididata[j] == 'z')
                    sigdata->midi->SFmacroz[i] |= 1 << len++;
                else if (mididata[j] >= '0' && mididata[j] <= '9')
                    leftdigit = mididata[j] - '0';
                else if (mididata[j] >= 'A' && mididata[j] <= 'F')
                    leftdigit = mididata[j] - 'A' + 0xA;
            }
            sigdata->midi->SFmacrolen[i] = len;
        }
        for (i = 0; i < 128; i++) {
            unsigned char len = 0;
            int j, leftdigit = -1;
            dumbfile_getnc((char *)mididata, 32, f);
            for (j = 0; j < 32; j++) {
                if (leftdigit >= 0) {
                    if (mididata[j] == 0) {
                        sigdata->midi->Zmacro[i][len++] = leftdigit;
                        break;
                    } else if (mididata[j] == ' ')
                        sigdata->midi->Zmacro[i][len++] = leftdigit;
                    else if (mididata[j] >= '0' && mididata[j] <= '9')
                        sigdata->midi->Zmacro[i][len++] =
                            (leftdigit << 4) | (mididata[j] - '0');
                    else if (mididata[j] >= 'A' && mididata[j] <= 'F')
                        sigdata->midi->Zmacro[i][len++] =
                            (leftdigit << 4) | (mididata[j] - 'A' + 0xA);
                    leftdigit = -1;
                } else if (mididata[j] == 0)
                    break;
                else if (mididata[j] >= '0' && mididata[j] <= '9')
                    leftdigit = mididata[j] - '0';
                else if (mididata[j] >= 'A' && mididata[j] <= 'F')
                    leftdigit = mididata[j] - 'A' + 0xA;
            }
            sigdata->midi->Zmacrolen[i] = len;
        }
    }

    sigdata->flags &= IT_REAL_FLAGS;

    qsort(component, n_components, sizeof(IT_COMPONENT), &it_component_compare);

    buffer = malloc(65536);
    if (!buffer) {
        free(component);
        _dumb_it_unload_sigdata(sigdata);
        return NULL;
    }

    for (n = 0; n < n_components; n++) {
        long offset;
        int m;

        /* XXX */
        if (component[n].offset == 0) {
            switch (component[n].type) {
            case IT_COMPONENT_INSTRUMENT:
                memset(&sigdata->instrument[component[n].n], 0,
                       sizeof(IT_INSTRUMENT));
                break;
            case IT_COMPONENT_SAMPLE:
                memset(&sigdata->sample[component[n].n], 0, sizeof(IT_SAMPLE));
                break;
            case IT_COMPONENT_PATTERN: {
                IT_PATTERN *p = &sigdata->pattern[component[n].n];
                p->entry = 0;
                p->n_rows = 64;
                p->n_entries = 0;
            } break;
            }
            continue;
        }

        if (dumbfile_seek(f, component[n].offset, DFS_SEEK_SET)) {
            free(buffer);
            free(component);
            _dumb_it_unload_sigdata(sigdata);
            return NULL;
        }

        switch (component[n].type) {

        case IT_COMPONENT_SONG_MESSAGE:
            if (n + 1 < n_components) {
                message_length =
                    min(message_length,
                        (int)(component[n + 1].offset - component[n].offset));
            }
            sigdata->song_message = malloc(message_length + 1);
            if (sigdata->song_message) {
                if (dumbfile_getnc((char *)sigdata->song_message,
                                   message_length, f) < message_length) {
                    free(buffer);
                    free(component);
                    _dumb_it_unload_sigdata(sigdata);
                    return NULL;
                }
                sigdata->song_message[message_length] = 0;
            }
            break;

        case IT_COMPONENT_INSTRUMENT:
            if (cmwt < 0x200)
                m = it_read_old_instrument(&sigdata->instrument[component[n].n],
                                           f);
            else
                m = it_read_instrument(
                    &sigdata->instrument[component[n].n], f,
                    (n + 1 < n_components)
                        ? (int)(component[n + 1].offset - component[n].offset)
                        : 0);

            if (m) {
                free(buffer);
                free(component);
                _dumb_it_unload_sigdata(sigdata);
                return NULL;
            }
            break;

        case IT_COMPONENT_PATTERN:
            if (it_read_pattern(&sigdata->pattern[component[n].n], f, buffer)) {
                free(buffer);
                free(component);
                _dumb_it_unload_sigdata(sigdata);
                return NULL;
            }
            break;

        case IT_COMPONENT_SAMPLE:
            if (it_read_sample_header(&sigdata->sample[component[n].n],
                                      &sample_convert[component[n].n], &offset,
                                      &disk_length[component[n].n], f)) {
                free(buffer);
                free(component);
                _dumb_it_unload_sigdata(sigdata);
                return NULL;
            }

            if (sigdata->sample[component[n].n].flags & IT_SAMPLE_EXISTS &&
                offset > 0) {
                component[n].data_offset = offset;
                component[n].sampdata_valid = 1;
            }
        }
    }

    /* Discover the end of every sample's source range only after every header
     * and data start is known. The boundaries are the file positions of all
     * nonzero components (instruments, sample headers, patterns and the song
     * message) plus the actual sample data starts. Two samples sharing the
     * same data start each get the same, non-zero range. */
    {
        long *starts;
        int n_starts = 0, max_starts = n_components + sigdata->n_samples + 1;

        starts = malloc((size_t)max_starts * sizeof(*starts));
        if (!starts) {
            free(buffer);
            free(component);
            _dumb_it_unload_sigdata(sigdata);
            return NULL;
        }
        for (n = 0; n < n_components; n++) {
            if (component[n].offset > 0 &&
                (n_starts == 0 || starts[n_starts - 1] != component[n].offset))
                starts[n_starts++] = component[n].offset;
        }
        for (n = 0; n < n_components; n++) {
            long off;
            int i, j;
            if (component[n].type != IT_COMPONENT_SAMPLE ||
                !component[n].sampdata_valid)
                continue;
            off = component[n].data_offset;
            for (i = 0; i < n_starts && starts[i] < off; i++)
                ;
            if (i < n_starts && starts[i] == off)
                continue;
            for (j = n_starts; j > i; j--)
                starts[j] = starts[j - 1];
            starts[i] = off;
            n_starts++;
        }
        /* Every start is now known, so each sample's source range ends at the
         * next distinct start, or at end-of-file when there is none. This is
         * computed before any compressed body is read. */
        for (n = 0; n < n_components; n++) {
            dumb_off_t end;
            int i;
            if (component[n].type != IT_COMPONENT_SAMPLE ||
                !component[n].sampdata_valid)
                continue;
            end = -1;
            for (i = 0; i < n_starts; i++)
                if (starts[i] > component[n].data_offset) {
                    end = starts[i];
                    break;
                }
            if (dumbfile_get_size(f) >= 0 &&
                (end < 0 || dumbfile_get_size(f) < end))
                end = dumbfile_get_size(f);

            /* Reject out-of-range data offsets before seeking to them. */
            if (component[n].data_offset < 0 ||
                (end >= 0 && component[n].data_offset > end)) {
                free(starts);
                free(buffer);
                free(component);
                _dumb_it_unload_sigdata(sigdata);
                return NULL;
            }

            if (dumbfile_seek(f, component[n].data_offset, DFS_SEEK_SET)) {
                free(starts);
                free(buffer);
                free(component);
                _dumb_it_unload_sigdata(sigdata);
                return NULL;
            }

            if (it_read_sample_data(&sigdata->sample[component[n].n],
                                    sample_convert[component[n].n],
                                    disk_length[component[n].n], end, f)) {
                free(starts);
                free(buffer);
                free(component);
                _dumb_it_unload_sigdata(sigdata);
                return NULL;
            }
        }
        free(starts);
    }

    for (n = 0; n < 10; n++) {
        if (dumbfile_getc(f) == 'X') {
            if (dumbfile_getc(f) == 'T') {
                if (dumbfile_getc(f) == 'P') {
                    if (dumbfile_getc(f) == 'M') {
                        break;
                    }
                }
            }
        }
    }

    if (!dumbfile_error(f) && n < 10) {
        unsigned int mptx_id = (unsigned int)dumbfile_igetl(f);
        while (!dumbfile_error(f) && mptx_id != DUMB_ID('M', 'P', 'T', 'S')) {
            unsigned int size = dumbfile_igetw(f);
            switch (mptx_id) {
                /* TODO: Add instrument extension readers */

            default:
                dumbfile_skip(f, size * sigdata->n_instruments);
                break;
            }

            mptx_id = (unsigned int)dumbfile_igetl(f);
        }

        mptx_id = (unsigned int)dumbfile_igetl(f);
        while (!dumbfile_error(f) && dumbfile_pos(f) < dumbfile_get_size(f)) {
            unsigned int size = dumbfile_igetw(f);
            switch (mptx_id) {
                /* TODO: Add more song extension readers */

            case DUMB_ID('D', 'T', '.', '.'):
                if (size == 2)
                    sigdata->tempo = dumbfile_igetw(f);
                else if (size == 4)
                    sigdata->tempo = (int)dumbfile_igetl(f);
                break;

            default:
                dumbfile_skip(f, size);
                break;
            }
            mptx_id = (unsigned int)dumbfile_igetl(f);
        }
    }

    free(buffer);
    free(component);

    if (_dumb_it_fix_invalid_orders(sigdata) < 0) {
        _dumb_it_unload_sigdata(sigdata);
        return NULL;
    }

    return sigdata;
}

DUH *dumb_read_it_quick(DUMBFILE *f) {
    sigdata_t *sigdata;

    DUH_SIGTYPE_DESC *descptr = &_dumb_sigtype_it;

    sigdata = it_load_sigdata(f);

    if (!sigdata)
        return NULL;

    {
        const char *tag[2][2];
        tag[0][0] = "TITLE";
        tag[0][1] = (const char *)(((DUMB_IT_SIGDATA *)sigdata)->name);
        tag[1][0] = "FORMAT";
        tag[1][1] = "IT";
        return make_duh(-1, 2, (const char *const(*)[2])tag, 1, &descptr,
                        &sigdata);
    }
}
