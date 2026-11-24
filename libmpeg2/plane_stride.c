/*
 * plane_stride.c
 *
 * Per-plane buffer stride registration and stride-separated C motion
 * compensation.  The regular equal-stride motion compensation entries are
 * left untouched; this file is only used when a call supplies different
 * source and destination strides.
 */

#include "config.h"

#include <stdint.h>
#include <string.h>

#include "mpeg2.h"
#include "attributes.h"
#include "mpeg2_internal.h"

extern mpeg2_mc_t mpeg2_mc;

int mpeg2_set_buf_strides (mpeg2dec_t * mpeg2dec, uint8_t * buf[3],
			   const int strides[3], void * id)
{
    const mpeg2_sequence_t * sequence;
    int plane_width[3];
    int i;

    if (mpeg2dec == NULL || buf == NULL || strides == NULL)
	return -1;
    if (!mpeg2dec->custom_fbuf)
	return -1;
    if (mpeg2dec->convert != NULL || mpeg2dec->convert_start != NULL ||
	mpeg2dec->decoder.convert != NULL)
	return -1;
    if (mpeg2dec->state != STATE_SEQUENCE &&
	mpeg2dec->state != STATE_PICTURE)
	return -1;

    sequence = &mpeg2dec->sequence;
    plane_width[0] = (int) sequence->width;
    plane_width[1] = plane_width[2] = (int) sequence->chroma_width;

    /* This entry point currently supports native 8-bit 4:2:0 MPEG-2. */
    if (sequence->width == (unsigned) -1 || plane_width[0] <= 0 ||
	!(sequence->flags & SEQ_FLAG_MPEG2) ||
	mpeg2dec->decoder.chroma_format != 0)
	return -1;

    for (i = 0; i < 3; i++) {
	if (buf[i] == NULL || strides[i] < plane_width[i])
	    return -1;
    }

    /* All validation is complete before changing the reference ring. */
    if (mpeg2dec->state == STATE_SEQUENCE) {
	mpeg2dec->fbuf[2] = mpeg2dec->fbuf[1];
	mpeg2dec->fbuf[1] = mpeg2dec->fbuf[0];
    }
    mpeg2_set_fbuf (mpeg2dec,
		    mpeg2dec->decoder.coding_type == PIC_FLAG_CODING_TYPE_B);

    {
	mpeg2_fbuf_t * fbuf = mpeg2dec->fbuf[0];
	fbuf_alloc_t * alloc = (fbuf_alloc_t *) fbuf;

	if (fbuf == NULL)
	    return -1;

	fbuf->buf[0] = buf[0];
	fbuf->buf[1] = buf[1];
	fbuf->buf[2] = buf[2];
	fbuf->id = id;
	alloc->strides[0] = strides[0];
	alloc->strides[1] = strides[1];
	alloc->strides[2] = strides[2];
	alloc->has_plane_strides = 1;
    }

    return 0;
}

#define PLANE_AVG2(a,b) (((a) + (b) + 1) >> 1)
#define PLANE_AVG4(a,b,c,d) (((a) + (b) + (c) + (d) + 2) >> 2)

void mpeg2_plane_mc_call (mpeg2_mc_fct * const * table, int index,
			  uint8_t * dest, const uint8_t * ref,
			  int dest_stride, int src_stride, int height)
{
    int width = (index < 4) ? 16 : 8;
    int xy = index & 3;
    int average;
    int y, x;

    if (dest_stride == src_stride) {
	table[index] (dest, ref, dest_stride, height);
	return;
    }

    average = (table[index] == mpeg2_mc.avg[index]);

    for (y = 0; y < height; y++) {
	const uint8_t * r = ref + y * src_stride;
	const uint8_t * rn = r + src_stride;
	uint8_t * d = dest + y * dest_stride;

	for (x = 0; x < width; x++) {
	    uint8_t pixel;

	    switch (xy) {
	    case 0:
		pixel = r[x];
		break;
	    case 1:
		pixel = PLANE_AVG2 (r[x], r[x + 1]);
		break;
	    case 2:
		pixel = PLANE_AVG2 (r[x], rn[x]);
		break;
	    default:
		pixel = PLANE_AVG4 (r[x], r[x + 1], rn[x], rn[x + 1]);
		break;
	    }

	    if (average)
		d[x] = PLANE_AVG2 (pixel, d[x]);
	    else
		d[x] = pixel;
	}
    }
}
