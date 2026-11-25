/*
 * conceal.c
 * Copyright (C) 2026
 *
 * This file is part of mpeg2dec, a free MPEG-2 video stream decoder.
 *
 * mpeg2dec is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpeg2dec is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Whole-slice loss concealment.
 *
 * Scope: scalar C, fixed sequence parameters, progressive 4:2:0 MPEG-2
 * I/P/B frame pictures whose coded width and height are multiples of 16,
 * starting with an I picture. Field pictures are not handled. Only whole
 * missing slices are concealed; every picture still contains at least one
 * complete slice.
 */

#include "config.h"

#include <string.h>
#include <inttypes.h>

#include "mpeg2.h"
#include "attributes.h"
#include "mpeg2_internal.h"

#define CONCEAL_NEUTRAL_Y 16
#define CONCEAL_NEUTRAL_C 128

/* Clear every piece of concealment state; the user switch is preserved. */
void mpeg2_conceal_reset (mpeg2dec_t * mpeg2dec)
{
    mpeg2dec->conceal_armed = 0;
    mpeg2dec->conceal_mbw = mpeg2dec->conceal_mbh = 0;
    mpeg2dec->conceal_mbs = 0;
    mpeg2dec->conceal_current = NULL;
    mpeg2dec->info.display_concealment = NULL;
    mpeg2dec->info.display_concealed_mbs = 0;
    if (mpeg2dec->conceal_mem)
	memset (mpeg2dec->conceal_mem, 0, 3 * mpeg2dec->conceal_mbs);
    memset (mpeg2dec->conceal_count, 0, sizeof (mpeg2dec->conceal_count));
    memset (mpeg2dec->conceal_complete, 0,
	    sizeof (mpeg2dec->conceal_complete));
}

void mpeg2_conceal_free (mpeg2dec_t * mpeg2dec)
{
    mpeg2_free (mpeg2dec->conceal_mem);
    mpeg2dec->conceal_mem = NULL;
    mpeg2dec->conceal_map[0] = mpeg2dec->conceal_map[1] =
	mpeg2dec->conceal_map[2] = NULL;
    mpeg2dec->conceal_current = NULL;
}

/* (Re)allocate the per-slot coverage maps for a new sequence grid. */
void mpeg2_conceal_sequence (mpeg2dec_t * mpeg2dec)
{
    const mpeg2_sequence_t * sequence = &(mpeg2dec->sequence);
    mpeg2_decoder_t * decoder = &(mpeg2dec->decoder);
    unsigned int mbw, mbh, mbs;
    int eligible;

    mpeg2dec->conceal_armed = 0;
    mpeg2dec->conceal_current = NULL;

    if (!mpeg2dec->conceal_enabled)
	return;

    mbw = sequence->width >> 4;
    mbh = sequence->height >> 4;
    mbs = mbw * mbh;

    eligible = (mpeg2dec->conceal_enabled &&
		(sequence->flags & SEQ_FLAG_MPEG2) &&
		(sequence->flags & SEQ_FLAG_PROGRESSIVE_SEQUENCE) &&
		(sequence->width & 15) == 0 &&
		(sequence->height & 15) == 0 &&
		decoder->chroma_format == 0 && mbw && mbh);

    if (!eligible)
	return;

    /*
     * A repeated sequence header keeps the same grid: the coverage of the
     * existing reference pictures must survive. Only a new grid (or the
     * first sequence after a reset) re-initializes the storage.
     */
    if (mbs == mpeg2dec->conceal_mbs && mpeg2dec->conceal_mem &&
	mpeg2dec->conceal_mbw) {
	mpeg2dec->conceal_armed = 1;
	return;
    }

    if (mbs != mpeg2dec->conceal_mbs || !mpeg2dec->conceal_mem) {
	mpeg2_free (mpeg2dec->conceal_mem);
	mpeg2dec->conceal_mem = (uint8_t *) mpeg2_malloc (3 * mbs,
							  MPEG2_ALLOC_CHUNK);
	if (!mpeg2dec->conceal_mem) {
	    mpeg2dec->conceal_mbs = 0;
	    return;
	}
	mpeg2dec->conceal_mbs = mbs;
	mpeg2dec->conceal_map[0] = mpeg2dec->conceal_mem;
	mpeg2dec->conceal_map[1] = mpeg2dec->conceal_mem + mbs;
	mpeg2dec->conceal_map[2] = mpeg2dec->conceal_mem + 2 * mbs;
    }

    mpeg2dec->conceal_mbw = mbw;
    mpeg2dec->conceal_mbh = mbh;
    memset (mpeg2dec->conceal_mem, 0, 3 * mbs);
    memset (mpeg2dec->conceal_count, 0, sizeof (mpeg2dec->conceal_count));
    memset (mpeg2dec->conceal_complete, 0,
	    sizeof (mpeg2dec->conceal_complete));
    mpeg2dec->conceal_armed = 1;
}

static int fbuf_slot (mpeg2dec_t * mpeg2dec, const mpeg2_fbuf_t * fbuf)
{
    int slot = (int) (fbuf - &mpeg2dec->fbuf_alloc[0].fbuf);

    return (slot >= 0 && slot < 3) ? slot : -1;
}

/* Start coverage bookkeeping for the picture about to be decoded. */
void mpeg2_conceal_picture_start (mpeg2dec_t * mpeg2dec)
{
    int slot;

    mpeg2dec->conceal_current = NULL;

    if (!mpeg2dec->conceal_armed)
	return;

    if (mpeg2dec->decoder.picture_structure != FRAME_PICTURE ||
	mpeg2dec->decoder.coding_type == D_TYPE) {
	/* outside the supported scope: leave the mode idle for safety */
	mpeg2dec->conceal_armed = 0;
	return;
    }

    if (mpeg2dec->decoder.coding_type != I_TYPE &&
	!(mpeg2dec->conceal_complete[0] || mpeg2dec->conceal_complete[1] ||
	  mpeg2dec->conceal_complete[2])) {
	/* stream does not start with an I picture: out of scope */
	mpeg2dec->conceal_armed = 0;
	return;
    }

    if (!mpeg2dec->fbuf[0])
	return;

    slot = fbuf_slot (mpeg2dec, mpeg2dec->fbuf[0]);
    if (slot < 0)
	return;

    mpeg2dec->decoder.conceal_owner = mpeg2dec;
    mpeg2dec->conceal_current = mpeg2dec->conceal_map[slot];
    /* every macroblock starts as missing; rebuilt ones are cleared to 0 */
    memset (mpeg2dec->conceal_current, 1, mpeg2dec->conceal_mbs);
    mpeg2dec->conceal_count[slot] = 0;
    mpeg2dec->conceal_complete[slot] = 0;
}

static inline unsigned int mb_index (mpeg2dec_t * mpeg2dec,
				     mpeg2_decoder_t * decoder)
{
    return ((decoder->v_offset >> 4) * mpeg2dec->conceal_mbw +
	    (decoder->offset >> 4));
}

/* Record that count macroblocks starting at first_mb were really rebuilt. */
void mpeg2_conceal_mb_range (mpeg2_decoder_t * decoder,
			     unsigned int first_mb, unsigned int count)
{
    mpeg2dec_t * mpeg2dec = decoder->conceal_owner;
    uint8_t * map;
    unsigned int i;

    if (!mpeg2dec || !(map = mpeg2dec->conceal_current))
	return;
    for (i = 0; i < count; i++) {
	unsigned int index = first_mb + i;

	if (index < mpeg2dec->conceal_mbs)
	    map[index] = 0;
    }
}

/* Record that the macroblock at the current decoder position was rebuilt. */
void mpeg2_conceal_mb_done (mpeg2_decoder_t * decoder)
{
    mpeg2dec_t * mpeg2dec = decoder->conceal_owner;

    if (!mpeg2dec || !mpeg2dec->conceal_current)
	return;
    mpeg2_conceal_mb_range (decoder, mb_index (mpeg2dec, decoder), 1);
}

static void fill_neutral (mpeg2_decoder_t * decoder,
			  unsigned int mbx, unsigned int mby)
{
    unsigned int x, y;
    uint8_t * d;

    d = decoder->picture_dest[0] + mby * 16 * decoder->stride + mbx * 16;
    for (y = 0; y < 16; y++) {
	for (x = 0; x < 16; x++)
	    d[x] = CONCEAL_NEUTRAL_Y;
	d += decoder->stride;
    }
    d = decoder->picture_dest[1] + mby * 8 * decoder->uv_stride + mbx * 8;
    for (y = 0; y < 8; y++) {
	for (x = 0; x < 8; x++)
	    d[x] = CONCEAL_NEUTRAL_C;
	d += decoder->uv_stride;
    }
    d = decoder->picture_dest[2] + mby * 8 * decoder->uv_stride + mbx * 8;
    for (y = 0; y < 8; y++) {
	for (x = 0; x < 8; x++)
	    d[x] = CONCEAL_NEUTRAL_C;
	d += decoder->uv_stride;
    }
}

static void copy_plane (uint8_t * dest, const uint8_t * ref,
			unsigned int mb_pixels, unsigned int stride)
{
    unsigned int y;

    for (y = 0; y < mb_pixels; y++) {
	memcpy (dest, ref, mb_pixels);
	dest += stride;
	ref += stride;
    }
}

static void avg_plane (uint8_t * dest, const uint8_t * a, const uint8_t * b,
		       unsigned int mb_pixels, unsigned int stride)
{
    unsigned int x, y;

    for (y = 0; y < mb_pixels; y++) {
	for (x = 0; x < mb_pixels; x++)
	    dest[x] = (a[x] + b[x] + 1) >> 1;
	dest += stride;
	a += stride;
	b += stride;
    }
}

/*
 * Fill every macroblock that no slice rebuilt, before the picture is shown
 * or reused as a reference. Reference pictures are themselves complete at
 * this point, so only fully reconstructed pixels are copied.
 */
void mpeg2_conceal_finish (mpeg2dec_t * mpeg2dec)
{
    mpeg2_decoder_t * decoder = &(mpeg2dec->decoder);
    uint8_t * map;
    unsigned int slot, index, mbx, mby, concealed;
    mpeg2_fbuf_t * fwd_fbuf;
    mpeg2_fbuf_t * bwd_fbuf;
    uint8_t * const * fwd;
    uint8_t * const * bwd;
    int fwd_slot, bwd_slot, have_fwd, have_bwd;

    if (!mpeg2dec->conceal_armed || !(map = mpeg2dec->conceal_current))
	return;

    slot = fbuf_slot (mpeg2dec, mpeg2dec->fbuf[0]);
    if ((int) slot < 0)
	return;

    /*
     * fbuf[] is a permutation of the allocated slots: keep the actual fbuf
     * pointers for pixel access, and only translate to a slot number to look
     * up the per-slot completion flag.
     */
    if (decoder->coding_type == B_TYPE) {
	fwd_fbuf = mpeg2dec->fbuf[2];
	bwd_fbuf = mpeg2dec->fbuf[1];
    } else {
	fwd_fbuf = mpeg2dec->fbuf[1];
	bwd_fbuf = NULL;
    }
    fwd_slot = fwd_fbuf ? fbuf_slot (mpeg2dec, fwd_fbuf) : -1;
    bwd_slot = bwd_fbuf ? fbuf_slot (mpeg2dec, bwd_fbuf) : -1;
    have_fwd = (fwd_slot >= 0 && mpeg2dec->conceal_complete[fwd_slot]);
    have_bwd = (decoder->coding_type == B_TYPE && bwd_slot >= 0 &&
		mpeg2dec->conceal_complete[bwd_slot]);
    fwd = have_fwd ? fwd_fbuf->buf : NULL;
    bwd = have_bwd ? bwd_fbuf->buf : NULL;

    concealed = 0;
    for (index = 0; index < mpeg2dec->conceal_mbs; index++) {
	if (!map[index])
	    continue;

	mbx = index % mpeg2dec->conceal_mbw;
	mby = index / mpeg2dec->conceal_mbw;
	concealed++;

	if (decoder->coding_type == I_TYPE ||
	    (!have_fwd && !have_bwd)) {
	    fill_neutral (decoder, mbx, mby);
	} else if (decoder->coding_type == P_TYPE ||
		   (have_fwd ^ have_bwd)) {
	    /* P: forward copy. B with a single reference: copy it. */
	    uint8_t * const * ref = have_fwd ? fwd : bwd;
	    unsigned int plane;

	    copy_plane (decoder->picture_dest[0] +
			mby * 16 * decoder->stride + mbx * 16,
			ref[0] + mby * 16 * decoder->stride + mbx * 16,
			16, decoder->stride);
	    for (plane = 1; plane < 3; plane++)
		copy_plane (decoder->picture_dest[plane] +
			    mby * 8 * decoder->uv_stride + mbx * 8,
			    ref[plane] +
			    mby * 8 * decoder->uv_stride + mbx * 8,
			    8, decoder->uv_stride);
	} else {
	    /* B with both references: per-byte (a + b + 1) / 2 */
	    unsigned int plane;

	    avg_plane (decoder->picture_dest[0] +
		       mby * 16 * decoder->stride + mbx * 16,
		       fwd[0] + mby * 16 * decoder->stride + mbx * 16,
		       bwd[0] + mby * 16 * decoder->stride + mbx * 16,
		       16, decoder->stride);
	    for (plane = 1; plane < 3; plane++)
		avg_plane (decoder->picture_dest[plane] +
			   mby * 8 * decoder->uv_stride + mbx * 8,
			   fwd[plane] +
			   mby * 8 * decoder->uv_stride + mbx * 8,
			   bwd[plane] +
			   mby * 8 * decoder->uv_stride + mbx * 8,
			   8, decoder->uv_stride);
	}
    }

    mpeg2dec->conceal_count[slot] = concealed;
    mpeg2dec->conceal_complete[slot] = 1;
    mpeg2dec->conceal_current = NULL;
}

/* Expose the coverage map of the picture currently offered for display. */
void mpeg2_conceal_publish (mpeg2dec_t * mpeg2dec)
{
    int slot;

    mpeg2dec->info.display_concealment = NULL;
    mpeg2dec->info.display_concealed_mbs = 0;

    if (!mpeg2dec->conceal_enabled || !mpeg2dec->info.display_fbuf)
	return;

    slot = fbuf_slot (mpeg2dec, mpeg2dec->info.display_fbuf);
    if (slot < 0 || !mpeg2dec->conceal_complete[slot])
	return;

    mpeg2dec->info.display_concealment = mpeg2dec->conceal_map[slot];
    mpeg2dec->info.display_concealed_mbs = mpeg2dec->conceal_count[slot];
}
