/*
 * motion_contract.c - independent pixel-level motion compensation contract.
 *
 * This file generates legal MPEG-1 elementary streams deterministically in
 * memory (no ffmpeg, no downloaded assets), decodes them through pl_mpeg and
 * compares every byte of the Y/Cb/Cr planes against a small reference model
 * that is written independently from the decoder:
 *
 *   - reference pictures are kept by picture identity;
 *   - each macroblock states its explicit type and FINAL displacement;
 *   - sampling/rounding follows ISO/IEC 11172-2 semantics described in
 *     tests/motion_contract.md.
 *
 * The expectations never reuse the library's VLC traversal tables, its
 * motion-vector differential decoder or the PLM_BLOCK_SET macros.
 *
 * Registration is through task_cases / task_case_count; there is no main().
 */

#include "task_support/check.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

/* ------------------------------------------------------------------ */
/* Fixed geometry: 3x3 macroblocks = 48x48 luma, 24x24 chroma (4:2:0). */
/* Interior macroblocks leave a margin for negative/positive vectors. */
/* ------------------------------------------------------------------ */

#define MC_MBW 3
#define MC_MBH 3
#define MC_NMB (MC_MBW * MC_MBH)
#define MC_W   (MC_MBW * 16)
#define MC_H   (MC_MBH * 16)
#define MC_CW  (MC_MBW * 8)
#define MC_CH  (MC_MBH * 8)
#define MC_YN  (MC_W * MC_H)
#define MC_CN  (MC_CW * MC_CH)

#define MC_WIRE_CAP 20000
#define MC_MAX_PICS 8
#define MC_MAX_ID   6

/* Named reference identities used by the scenarios. */
enum {
	MC_ID_A = 0,  /* first deliberately uneven all-intra picture  */
	MC_ID_C = 1,  /* second, different all-intra reference       */
	MC_ID_P2 = 2, /* a predictive picture kept as a reference    */
	MC_ID_P4 = 3, /* a later predictive reference                */
	MC_ID_B1 = 4,
	MC_ID_B3 = 5
};

enum {
	MC_INTRA = 1,
	MC_FWD   = 2,
	MC_BACK  = 3,
	MC_BIDI  = 4
};

typedef struct {
	unsigned char y[MC_YN];
	unsigned char cb[MC_CN];
	unsigned char cr[MC_CN];
} mc_planes;

typedef struct {
	/* Wire representation. skip==1 means this MB is reached through an
	   address increment gap; kind/deltas are then not emitted for it. */
	int skip;

	/* Contract / reference description for this macroblock. */
	int kind;            /* MC_INTRA / FWD / BACK / BIDI */
	int fref, bref;      /* reference identity indices */
	int fh, fv;          /* explicit FINAL forward displacement  */
	int bh, bv;          /* explicit FINAL backward displacement */

	/* Exact differential symbols put on the wire. When auto_deltas is
	   set for a picture, the builder derives these from the finals. */
	int dh, dv, dbh, dbv;

	int dc[6];           /* intra DC levels: Y0..Y3, Cb, Cr */
} mc_mb;

typedef struct {
	int ptype;           /* 1 I, 2 P, 3 B */
	int temp_ref;
	int fcode_f;         /* 1..7 (header value, r_size = fcode-1) */
	int fcode_b;
	int full_f, full_b;
	int slices;          /* 1 = whole picture, MC_MBH = one row/slice */
	int auto_deltas;
	int store_id;
	mc_mb mb[MC_NMB];
} mc_picdesc;

typedef struct {
	unsigned char wire[MC_WIRE_CAP];
	mc_planes id[MC_MAX_ID];
	mc_picdesc pics[MC_MAX_PICS];
	int npics;
	int display[MC_MAX_PICS];
	int ndisplay;
} mc_clip;

/* Block-DC texture specification: every 8x8 block is one flat value. */
typedef struct {
	int ydc[MC_MBH * 2][MC_MBW * 2];
	int cbdc[MC_MBH][MC_MBW];
	int crdc[MC_MBH][MC_MBW];
} mc_blocks;

/* ------------------------------------------------------------------ */
/* Bitstream constructor (our own small MSB-first writer).            */
/* ------------------------------------------------------------------ */

typedef struct {
	unsigned char *b;
	size_t cap;
	size_t pos; /* bits */
	int overflow;
} mc_wire;

static void mc_bits(mc_wire *w, unsigned value, int n) {
	int i;
	if (n == 0) { return; }
	for (i = n; i > 0; i--) {
		size_t byte_i = w->pos >> 3;
		int bit_i = 7 - (int)(w->pos & 7);
		unsigned bit = (value >> (i - 1)) & 1u;
		if (byte_i >= w->cap) { w->overflow = 1; return; }
		if (bit) { w->b[byte_i] |= (unsigned char)(1u << bit_i); }
		w->pos++;
	}
}

static void mc_align(mc_wire *w) {
	while (w->pos & 7u) { mc_bits(w, 0, 1); }
}

static void mc_start_code(mc_wire *w, unsigned id) {
	mc_align(w);
	mc_bits(w, 0x000001u, 24);
	mc_bits(w, id & 0xffu, 8);
}

/* Standard ISO/IEC 11172-2 macroblock_address_increment codewords. */
static void mc_put_increment(mc_wire *w, int inc) {
	switch (inc) {
		case 1: mc_bits(w, 0x1, 1); break;   /* 1    */
		case 2: mc_bits(w, 0x3, 3); break;   /* 011  */
		case 3: mc_bits(w, 0x2, 3); break;   /* 010  */
		case 4: mc_bits(w, 0x3, 4); break;   /* 0011 */
		case 5: mc_bits(w, 0x2, 4); break;   /* 0010 */
		case 6: mc_bits(w, 0x3, 5); break;   /* 00011 */
		default: w->overflow = 1; break;     /* enough for 3x3 geometry */
	}
}

static void mc_put_mb_type(mc_wire *w, int ptype, int kind) {
	if (ptype == 1) {
		/* I: only Intra, code "1". */
		mc_bits(w, 0x1, 1);
	}
	else if (ptype == 2) {
		if (kind == MC_INTRA) { mc_bits(w, 0x03, 5); } /* 00011 */
		else if (kind == MC_FWD) { mc_bits(w, 0x1, 3); } /* 001   */
		else { w->overflow = 1; }
	}
	else {
		if (kind == MC_INTRA) { mc_bits(w, 0x03, 5); } /* 00011 */
		else if (kind == MC_FWD) { mc_bits(w, 0x2, 4); } /* 0010  */
		else if (kind == MC_BACK) { mc_bits(w, 0x2, 3); } /* 010   */
		else if (kind == MC_BIDI) { mc_bits(w, 0x2, 2); } /* 10    */
		else { w->overflow = 1; }
	}
}

/* Positive motion m_code magnitudes 1..16: {codeword, bit length}.
   The sign is the final codeword bit: positive 0, negative 1. */
static const unsigned mc_motion_code[17] = {
	0, 0x2, 0x2, 0x2, 0x6, 0xa, 0x8, 0x6,
	0x16, 0x14, 0x12, 0x22, 0x20, 0x1e, 0x1c, 0x1a, 0x18
};
static const int mc_motion_len[17] = {
	1, 3, 4, 5, 7, 8, 8, 8,
	10, 10, 10, 11, 11, 11, 11, 11, 11
};

static void mc_put_motion_delta(mc_wire *w, int delta, int fcode) {
	int r_size = fcode - 1;
	int fscale = 1 << r_size;
	int code, mag, k, r;

	if (delta == 0) {
		mc_bits(w, 0x1, 1); /* m_code 0: "1" */
		return;
	}

	if (fscale == 1) {
		/* r_size == 0: the VLC value itself is the differential. */
		code = delta;
		if (code < -16 || code > 16) { w->overflow = 1; return; }
		mag = code < 0 ? -code : code;
		{
			unsigned cw = mc_motion_code[mag];
			if (code < 0) { cw |= 1u; } /* sign bit */
			mc_bits(w, cw, mc_motion_len[mag]);
		}
		return;
	}

	/* f_code > 1: split into m_code + residual bits. */
	mag = delta < 0 ? -delta : delta;
	k = (mag + fscale - 1) / fscale; /* m_code magnitude */
	if (k < 1 || k > 16) { w->overflow = 1; return; }
	r = mag - (((k - 1) * fscale) + 1);
	{
		unsigned cw = mc_motion_code[k];
		if (delta < 0) { cw |= 1u; }
		mc_bits(w, cw, mc_motion_len[k]);
	}
	if (r_size > 0) { mc_bits(w, (unsigned)r, r_size); }
}

/* dct_dc_size codewords (our own table, standard Annex B). */
static void mc_put_dc_size(mc_wire *w, int plane, int size) {
	static const unsigned lum_code[9] = {
		0x4, 0x0, 0x1, 0x5, 0x6, 0xe, 0x1e, 0x3e, 0x7e
	};
	static const int lum_len[9] = { 3, 2, 2, 3, 3, 4, 5, 6, 7 };
	static const unsigned chr_code[9] = {
		0x0, 0x1, 0x2, 0x6, 0xe, 0x1e, 0x3e, 0x7e, 0xfe
	};
	static const int chr_len[9] = { 2, 2, 2, 3, 4, 5, 6, 7, 8 };
	if (plane == 0) { mc_bits(w, lum_code[size], lum_len[size]); }
	else { mc_bits(w, chr_code[size], chr_len[size]); }
}

static int mc_dc_size_for(int delta) {
	int mag, s;
	if (delta == 0) { return 0; }
	mag = delta < 0 ? -delta : delta;
	for (s = 1; s <= 8; s++) {
		if (mag >= (1 << (s - 1)) && mag <= (1 << s) - 1) { return s; }
	}
	return -1;
}

/* Emit one intra DC block with running predictor *pred. */
static void mc_put_dc_block(mc_wire *w, int plane, int dc, int pred[3]) {
	int pidx = plane == 0 ? 0 : plane; /* caller passes 0=Y,1=Cb,2=Cr */
	int delta, size, code;
	delta = dc - pred[pidx];
	size = mc_dc_size_for(delta);
	mc_put_dc_size(w, pidx, size);
	if (size > 0) {
		if (delta > 0) {
			code = delta; /* MSB set: positive */
		}
		else {
			int mag = -delta;
			code = (1 << size) - 1 - mag; /* decoder inverse: d=u+1-2^s */
		}
		mc_bits(w, (unsigned)code, size);
	}
	pred[pidx] = dc;
	/* end_of_block as decoded by this library: VLC "1" plus a 0 bit. */
	mc_bits(w, 0x2, 2); /* "10" */
}

static void mc_put_intra_blocks(mc_wire *w, const mc_mb *mb, int pred[3]) {
	int b;
	for (b = 0; b < 4; b++) { mc_put_dc_block(w, 0, mb->dc[b], pred); }
	mc_put_dc_block(w, 1, mb->dc[4], pred); /* Cb */
	mc_put_dc_block(w, 2, mb->dc[5], pred); /* Cr */
}

static void mc_build_sequence_header(mc_wire *w) {
	mc_start_code(w, 0xb3);
	mc_bits(w, MC_W, 12);
	mc_bits(w, MC_H, 12);
	mc_bits(w, 1, 4);  /* pixel aspect ratio */
	mc_bits(w, 2, 4);  /* frame rate code 2 == 24.000 fps */
	mc_bits(w, 1, 18); /* bit rate */
	mc_bits(w, 1, 1);  /* marker */
	mc_bits(w, 20, 10);/* vbv buffer size */
	mc_bits(w, 0, 1);  /* constrained parameters flag */
	mc_bits(w, 0, 1);  /* no custom intra quant matrix */
	mc_bits(w, 0, 1);  /* no custom non-intra quant matrix */
}

static void mc_build_picture_header(mc_wire *w, const mc_picdesc *p) {
	mc_start_code(w, 0x00);
	mc_bits(w, (unsigned)p->temp_ref, 10);
	mc_bits(w, (unsigned)p->ptype, 3);
	mc_bits(w, 0xffffu, 16); /* vbv_delay */
	if (p->ptype == 2 || p->ptype == 3) {
		mc_bits(w, (unsigned)p->full_f, 1);
		mc_bits(w, (unsigned)p->fcode_f, 3);
	}
	if (p->ptype == 3) {
		mc_bits(w, (unsigned)p->full_b, 1);
		mc_bits(w, (unsigned)p->fcode_b, 3);
	}
}

/* Fill dh/dv/... from explicit finals for clips without address gaps.
   This is the constructor's differential encoder; expectations only use
   the final values. */
static void mc_auto_deltas(mc_picdesc *p) {
	int pfh = 0, pfv = 0, pbh = 0, pbv = 0;
	int i;
	for (i = 0; i < MC_NMB; i++) {
		mc_mb *m = &p->mb[i];
		if (m->skip) { continue; }
		if (m->kind == MC_INTRA) {
			pfh = pfv = pbh = pbv = 0;
			continue;
		}
		if (m->kind == MC_FWD || m->kind == MC_BIDI) {
			m->dh = m->fh - pfh;
			m->dv = m->fv - pfv;
			pfh = m->fh; pfv = m->fv;
		}
		if (m->kind == MC_BACK || m->kind == MC_BIDI) {
			m->dbh = m->bh - pbh;
			m->dbv = m->bv - pbv;
			pbh = m->bh; pbv = m->bv;
		}
	}
}

static void mc_build_picture_data(mc_wire *w, const mc_picdesc *p) {
	int multi_slice = (p->slices >= MC_MBH);
	int seg;
	int segments = multi_slice ? MC_MBH : 1;

	for (seg = 0; seg < segments; seg++) {
		int i0 = multi_slice ? seg * MC_MBW : 0;
		int i1 = multi_slice ? i0 + MC_MBW : MC_NMB;
		int base = i0 - 1;         /* decoder address base for this slice */
		int last_addr = base;
		int pred[3] = { 128, 128, 128 };
		int i;

		mc_start_code(w, (unsigned)(0x01 + seg)); /* slice 1..MBH */
		mc_bits(w, 8, 5); /* quantizer scale */
		mc_bits(w, 0, 1); /* extra_bit_slice */

		for (i = i0; i < i1; i++) {
			const mc_mb *m = &p->mb[i];
			int inc;
			if (m->skip) {
				/* Represented solely by the next coded MB's
				   address_increment; no bits emitted here. */
				continue;
			}
			inc = i - last_addr;
			last_addr = i;

			mc_put_increment(w, inc);
			mc_put_mb_type(w, p->ptype, m->kind);

			if (m->kind == MC_INTRA) {
				mc_put_intra_blocks(w, m, pred);
				continue;
			}

			if (m->kind == MC_FWD || m->kind == MC_BIDI) {
				mc_put_motion_delta(w, m->dh, p->fcode_f);
				mc_put_motion_delta(w, m->dv, p->fcode_f);
			}
			if (m->kind == MC_BACK || m->kind == MC_BIDI) {
				mc_put_motion_delta(w, m->dbh, p->fcode_b);
				mc_put_motion_delta(w, m->dbv, p->fcode_b);
			}
		}
	}
}

/* A decodable all-intra trailer that flushes the last checked reference. */
static void mc_build_trailer(mc_wire *w) {
	mc_picdesc t;
	int i;
	memset(&t, 0, sizeof(t));
	t.ptype = 1;
	t.temp_ref = 9;
	t.fcode_f = t.fcode_b = 1;
	t.slices = 1;
	for (i = 0; i < MC_NMB; i++) {
		int k;
		t.mb[i].kind = MC_INTRA;
		for (k = 0; k < 6; k++) { t.mb[i].dc[k] = 128; }
	}
	mc_build_picture_header(w, &t);
	mc_build_picture_data(w, &t);
	/* Bare picture start code so every real picture sees a following
	   picture start code through the decoder's complete-frame gate. */
	mc_start_code(w, 0x00);
}

static int mc_build_clip(mc_clip *c) {
	mc_wire w;
	int i;
	memset(c->wire, 0, sizeof(c->wire));
	w.b = c->wire;
	w.cap = sizeof(c->wire);
	w.pos = 0;
	w.overflow = 0;

	mc_build_sequence_header(&w);
	for (i = 0; i < c->npics; i++) {
		if (c->pics[i].auto_deltas) { mc_auto_deltas(&c->pics[i]); }
		mc_build_picture_header(&w, &c->pics[i]);
		mc_build_picture_data(&w, &c->pics[i]);
	}
	mc_build_trailer(&w);
	if (w.overflow) {
		fprintf(stderr, "motion_contract: wire overflow\n");
	}
	return w.overflow;
}

/* ------------------------------------------------------------------ */
/* Independent reference model.                                       */
/* ------------------------------------------------------------------ */

static int mc_floor2(int m) {
	int q = m / 2;
	int r = m % 2;
	if (r < 0) { q--; }
	return q;
}

/* Truncation toward zero is exactly C99 signed division; named explicitly
   because the chroma derivation depends on it. */
static int mc_trunc0_2(int m) { return m / 2; }

static int mc_avg2(int a, int b) { return (a + b + 1) >> 1; }
static int mc_avg4(int a, int b, int c, int d) { return (a + b + c + d + 2) >> 2; }

static int mc_get(const unsigned char *pl, int stride, int w, int h,
                  int x, int y, int *oob) {
	if (x < 0 || y < 0 || x >= w || y >= h) {
		*oob = 1;
		return 0;
	}
	return pl[y * stride + x];
}

/* One reference's ONCE-ROUNDED predictor for an bs x bs MB/block.
   mh/mv are in half-pel units of THIS plane's sampling grid. */
static void mc_predict_plane(
	unsigned char *out,
	const unsigned char *src, int sstride, int sw, int sh,
	int ox, int oy, int bs, int mh, int mv, int *oob
) {
	int ah = mc_floor2(mh);
	int av = mc_floor2(mv);
	int oh = (mh - 2 * ah) != 0; /* 0 or 1, including negative odd */
	int ov = (mv - 2 * av) != 0;
	int y, x;

	for (y = 0; y < bs; y++) {
		for (x = 0; x < bs; x++) {
			int sx = ox + x + ah;
			int sy = oy + y + av;
			int v00 = mc_get(src, sstride, sw, sh, sx, sy, oob);
			int v = v00;
			if (oh && ov) {
				int v10 = mc_get(src, sstride, sw, sh, sx + 1, sy, oob);
				int v01 = mc_get(src, sstride, sw, sh, sx, sy + 1, oob);
				int v11 = mc_get(src, sstride, sw, sh, sx + 1, sy + 1, oob);
				v = mc_avg4(v00, v10, v01, v11);
			}
			else if (oh) {
				int v10 = mc_get(src, sstride, sw, sh, sx + 1, sy, oob);
				v = mc_avg2(v00, v10);
			}
			else if (ov) {
				int v01 = mc_get(src, sstride, sw, sh, sx, sy + 1, oob);
				v = mc_avg2(v00, v01);
			}
			out[y * bs + x] = (unsigned char)v;
		}
	}
}

static void mc_fill_flat_block(
	unsigned char *pl, int stride, int bx, int by, int bs, int value
) {
	int y, x;
	for (y = 0; y < bs; y++) {
		for (x = 0; x < bs; x++) {
			pl[(by * bs + y) * stride + bx * bs + x] = (unsigned char)value;
		}
	}
}

static void mc_texture_from_blocks(mc_planes *out, const mc_blocks *b) {
	int by, bx, i;
	memset(out, 0, sizeof(*out));
	for (by = 0; by < MC_MBH * 2; by++) {
		for (bx = 0; bx < MC_MBW * 2; bx++) {
			mc_fill_flat_block(out->y, MC_W, bx, by, 8, b->ydc[by][bx]);
		}
	}
	for (by = 0; by < MC_MBH; by++) {
		for (bx = 0; bx < MC_MBW; bx++) {
			mc_fill_flat_block(out->cb, MC_CW, bx, by, 8, b->cbdc[by][bx]);
			mc_fill_flat_block(out->cr, MC_CW, bx, by, 8, b->crdc[by][bx]);
		}
	}
	(void)i;
}

static void mc_intra_mbs_from_blocks(mc_picdesc *p, const mc_blocks *b) {
	int mby, mbx;
	for (mby = 0; mby < MC_MBH; mby++) {
		for (mbx = 0; mbx < MC_MBW; mbx++) {
			mc_mb *m = &p->mb[mby * MC_MBW + mbx];
			m->kind = MC_INTRA;
			m->dc[0] = b->ydc[mby * 2 + 0][mbx * 2 + 0];
			m->dc[1] = b->ydc[mby * 2 + 0][mbx * 2 + 1];
			m->dc[2] = b->ydc[mby * 2 + 1][mbx * 2 + 0];
			m->dc[3] = b->ydc[mby * 2 + 1][mbx * 2 + 1];
			m->dc[4] = b->cbdc[mby][mbx];
			m->dc[5] = b->crdc[mby][mbx];
		}
	}
}

static void mc_render_intra_mb(mc_planes *d, int mx, int my, const mc_mb *m) {
	/* Y four 8x8 blocks, decoder block order. */
	mc_fill_flat_block(d->y, MC_W, mx * 2 + 0, my * 2 + 0, 8, m->dc[0]);
	mc_fill_flat_block(d->y, MC_W, mx * 2 + 1, my * 2 + 0, 8, m->dc[1]);
	mc_fill_flat_block(d->y, MC_W, mx * 2 + 0, my * 2 + 1, 8, m->dc[2]);
	mc_fill_flat_block(d->y, MC_W, mx * 2 + 1, my * 2 + 1, 8, m->dc[3]);
	mc_fill_flat_block(d->cb, MC_CW, mx, my, 8, m->dc[4]);
	mc_fill_flat_block(d->cr, MC_CW, mx, my, 8, m->dc[5]);
}

static void mc_copy_mb_plane(
	unsigned char *dst, int dstride, int mx, int my, int bs,
	const unsigned char *pred, int round_bidi,
	const unsigned char *back
) {
	int y, x;
	for (y = 0; y < bs; y++) {
		for (x = 0; x < bs; x++) {
			int k = y * bs + x;
			int v = pred[k];
			if (round_bidi) {
				/* final bidirectional average: +1 rounding here,
				   applied to the two already-rounded predictors. */
				v = (pred[k] + back[k] + 1) >> 1;
			}
			dst[(my * bs + y) * dstride + mx * bs + x] = (unsigned char)v;
		}
	}
}

static void mc_render_mb(
	mc_planes *d, int mx, int my, const mc_mb *m,
	const mc_planes *ids, int *oob
) {
	unsigned char pf_y[256], pf_cb[64], pf_cr[64];
	unsigned char pb_y[256], pb_cb[64], pb_cr[64];
	const mc_planes *f = &ids[m->fref];
	const mc_planes *b = &ids[m->bref];
	int bx = mx * 16, by = my * 16;
	int cx = mx * 8, cy = my * 8;
	int use_f = (m->kind == MC_FWD || m->kind == MC_BIDI);
	int use_b = (m->kind == MC_BACK || m->kind == MC_BIDI);

	if (m->kind == MC_INTRA) {
		mc_render_intra_mb(d, mx, my, m);
		return;
	}

	if (use_f) {
		mc_predict_plane(pf_y, f->y, MC_W, MC_W, MC_H, bx, by, 16,
		                  m->fh, m->fv, oob);
		mc_predict_plane(pf_cb, f->cb, MC_CW, MC_CW, MC_CH, cx, cy, 8,
		                  mc_trunc0_2(m->fh), mc_trunc0_2(m->fv), oob);
		mc_predict_plane(pf_cr, f->cr, MC_CW, MC_CW, MC_CH, cx, cy, 8,
		                  mc_trunc0_2(m->fh), mc_trunc0_2(m->fv), oob);
	}
	if (use_b) {
		mc_predict_plane(pb_y, b->y, MC_W, MC_W, MC_H, bx, by, 16,
		                  m->bh, m->bv, oob);
		mc_predict_plane(pb_cb, b->cb, MC_CW, MC_CW, MC_CH, cx, cy, 8,
		                  mc_trunc0_2(m->bh), mc_trunc0_2(m->bv), oob);
		mc_predict_plane(pb_cr, b->cr, MC_CW, MC_CW, MC_CH, cx, cy, 8,
		                  mc_trunc0_2(m->bh), mc_trunc0_2(m->bv), oob);
	}

	if (m->kind == MC_FWD) {
		mc_copy_mb_plane(d->y, MC_W, mx, my, 16, pf_y, 0, 0);
		mc_copy_mb_plane(d->cb, MC_CW, mx, my, 8, pf_cb, 0, 0);
		mc_copy_mb_plane(d->cr, MC_CW, mx, my, 8, pf_cr, 0, 0);
	}
	else if (m->kind == MC_BACK) {
		mc_copy_mb_plane(d->y, MC_W, mx, my, 16, pb_y, 0, 0);
		mc_copy_mb_plane(d->cb, MC_CW, mx, my, 8, pb_cb, 0, 0);
		mc_copy_mb_plane(d->cr, MC_CW, mx, my, 8, pb_cr, 0, 0);
	}
	else { /* MC_BIDI: forward copied, backward averaged with final round */
		mc_copy_mb_plane(d->y, MC_W, mx, my, 16, pf_y, 0, 0);
		mc_copy_mb_plane(d->y, MC_W, mx, my, 16, pf_y, 1, pb_y);
		mc_copy_mb_plane(d->cb, MC_CW, mx, my, 8, pf_cb, 1, pb_cb);
		mc_copy_mb_plane(d->cr, MC_CW, mx, my, 8, pf_cr, 1, pb_cr);
	}
}

static void mc_render_clip(mc_clip *c, int *oob) {
	int i, k;
	*oob = 0;
	for (i = 0; i < c->npics; i++) {
		mc_picdesc *p = &c->pics[i];
		mc_planes *dst = &c->id[p->store_id];
		memset(dst, 0, sizeof(*dst));
		for (k = 0; k < MC_NMB; k++) {
			mc_render_mb(dst, k % MC_MBW, k / MC_MBW,
			             &p->mb[k], c->id, oob);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Decode through pl_mpeg and compare.                                */
/* ------------------------------------------------------------------ */

typedef struct {
	int present;
	double time;
	double want_time;
	int time_ok;
	int dim_ok;
	long y_count, cb_count, cr_count;
	int y_first, cb_first, cr_first;
} mc_framerep;

typedef struct {
	int header_ok;
	int width, height;
	double fps;
	int wire_overflow;
	int sim_oob;
	int ndisplay;
	mc_framerep fr[MC_MAX_PICS];
} mc_report;

static void mc_compare_plane(
	const unsigned char *got,
	const unsigned char *want, unsigned n,
	long *count, int *first
) {
	unsigned i;
	*count = 0;
	*first = -1;
	for (i = 0; i < n; i++) {
		if (got[i] != want[i]) {
			if (*first < 0) { *first = (int)i; }
			(*count)++;
		}
	}
}

static void mc_run_clip(mc_clip *c, mc_report *r) {
	plm_buffer_t *buf;
	plm_video_t *v;
	int j;

	memset(r, 0, sizeof(*r));
	r->ndisplay = c->ndisplay;
	r->wire_overflow = mc_build_clip(c);
	mc_render_clip(c, &r->sim_oob);

	buf = plm_buffer_create_with_memory(c->wire, sizeof(c->wire), 0);
	v = plm_video_create_with_buffer(buf, 1);
	r->header_ok = plm_video_has_header(v);
	r->width = plm_video_get_width(v);
	r->height = plm_video_get_height(v);
	r->fps = plm_video_get_framerate(v);

	if (r->header_ok) {
		for (j = 0; j < c->ndisplay; j++) {
			plm_frame_t *f = plm_video_decode(v);
			mc_framerep *fr = &r->fr[j];
			int pic_idx = c->display[j];
			mc_planes *want = &c->id[c->pics[pic_idx].store_id];
			fr->want_time = (double)j / 24.0;
			if (!f) {
				fr->present = 0;
				fprintf(stderr, "frame %d: plm_video_decode returned NULL\n", j);
				continue;
			}
			fr->present = 1;
			fr->time = f->time;
			fr->time_ok = (f->time >= fr->want_time - 1e-9 &&
			               f->time <= fr->want_time + 1e-9);
			fr->dim_ok =
				(f->y.width == MC_W && f->y.height == MC_H &&
				 f->cb.width == MC_CW && f->cb.height == MC_CH &&
				 f->cr.width == MC_CW && f->cr.height == MC_CH);

			mc_compare_plane(f->y.data, want->y, MC_YN,
			                 &fr->y_count, &fr->y_first);
			mc_compare_plane(f->cb.data, want->cb, MC_CN,
			                 &fr->cb_count, &fr->cb_first);
			mc_compare_plane(f->cr.data, want->cr, MC_CN,
			                 &fr->cr_count, &fr->cr_first);

			if (fr->y_count || fr->cb_count || fr->cr_count ||
			    !fr->time_ok || !fr->dim_ok) {
				fprintf(stderr,
					"frame %d (pic %d tref %d): time %.6f want %.6f "
					"dimok=%d mismatch Y=%ld(first %d) "
					"Cb=%ld(first %d) Cr=%ld(first %d)\n",
					j, pic_idx, c->pics[pic_idx].temp_ref,
					fr->time, fr->want_time, fr->dim_ok,
					fr->y_count, fr->y_first,
					fr->cb_count, fr->cb_first,
					fr->cr_count, fr->cr_first);
				if (fr->y_first >= 0) {
					int i = fr->y_first;
					fprintf(stderr, "  Y[%d] got=%u want=%u\n", i,
						f->y.data[i], want->y[i]);
				}
				if (fr->cb_first >= 0) {
					int i = fr->cb_first;
					fprintf(stderr, "  Cb[%d] got=%u want=%u\n", i,
						f->cb.data[i], want->cb[i]);
				}
				if (fr->cr_first >= 0) {
					int i = fr->cr_first;
					fprintf(stderr, "  Cr[%d] got=%u want=%u\n", i,
						f->cr.data[i], want->cr[i]);
				}
			}
		}
	}
	plm_video_destroy(v);
}

/* Assert the common report shape; returns 1 from the test on failure.
   Used inside each registered case so every case has TASK_CHECKs. */
#define MC_CHECK_REPORT(rp, expected_frames) do { \
		TASK_CHECK((rp)->header_ok); \
		TASK_CHECK((rp)->width == MC_W && (rp)->height == MC_H); \
		TASK_CHECK((rp)->fps > 23.99 && (rp)->fps < 24.01); \
		TASK_CHECK(!(rp)->wire_overflow); \
		TASK_CHECK(!(rp)->sim_oob); \
		TASK_CHECK((rp)->ndisplay == (expected_frames)); \
	} while (0)

#define MC_CHECK_FRAME(rp, i) do { \
		TASK_CHECK((rp)->fr[i].present); \
		TASK_CHECK((rp)->fr[i].time_ok); \
		TASK_CHECK((rp)->fr[i].dim_ok); \
		TASK_CHECK((rp)->fr[i].y_count == 0); \
		TASK_CHECK((rp)->fr[i].cb_count == 0); \
		TASK_CHECK((rp)->fr[i].cr_count == 0); \
	} while (0)

/* ------------------------------------------------------------------ */
/* Scenario helpers                                                    */
/* ------------------------------------------------------------------ */

static mc_picdesc *mc_add_pic(mc_clip *c, int ptype, int tref, int store) {
	mc_picdesc *p;
	if (c->npics >= MC_MAX_PICS) { return 0; }
	p = &c->pics[c->npics++];
	memset(p, 0, sizeof(*p));
	p->ptype = ptype;
	p->temp_ref = tref;
	p->fcode_f = 1;
	p->fcode_b = 1;
	p->slices = 1;
	p->auto_deltas = 1;
	p->store_id = store;
	return p;
}

static mc_mb mc_fwd(int ref, int h, int v) {
	mc_mb m;
	memset(&m, 0, sizeof(m));
	m.kind = MC_FWD; m.fref = ref; m.fh = h; m.fv = v;
	m.dh = h; m.dv = v;
	return m;
}

static mc_mb mc_back(int ref, int h, int v) {
	mc_mb m;
	memset(&m, 0, sizeof(m));
	m.kind = MC_BACK; m.bref = ref; m.bh = h; m.bv = v;
	m.dbh = h; m.dbv = v;
	return m;
}

static mc_mb mc_bidi(int fref, int fh, int fv, int bref, int bh, int bv) {
	mc_mb m;
	memset(&m, 0, sizeof(m));
	m.kind = MC_BIDI;
	m.fref = fref; m.fh = fh; m.fv = fv;
	m.bref = bref; m.bh = bh; m.bv = bv;
	m.dh = fh; m.dv = fv; m.dbh = bh; m.dbv = bv;
	return m;
}

static mc_mb mc_intra(unsigned y0, unsigned y1, unsigned y2, unsigned y3,
                      unsigned cb, unsigned cr) {
	mc_mb m;
	memset(&m, 0, sizeof(m));
	m.kind = MC_INTRA;
	m.dc[0] = (int)y0; m.dc[1] = (int)y1;
	m.dc[2] = (int)y2; m.dc[3] = (int)y3;
	m.dc[4] = (int)cb; m.dc[5] = (int)cr;
	return m;
}

/* Rich deterministic DC texture: adjacent 8x8 blocks differ in value and
   parity so half-pel and rounding paths produce discriminating bytes. */
static void mc_rich_blocks(mc_blocks *b, int salt) {
	int by, bx;
	memset(b, 0, sizeof(*b));
	for (by = 0; by < MC_MBH * 2; by++) {
		for (bx = 0; bx < MC_MBW * 2; bx++) {
			b->ydc[by][bx] =
				18 + ((bx * 37 + by * 53 + salt * 17) % 214);
		}
	}
	for (by = 0; by < MC_MBH; by++) {
		for (bx = 0; bx < MC_MBW; bx++) {
			b->cbdc[by][bx] =
				24 + ((bx * 61 + by * 29 + salt * 13) % 200);
			b->crdc[by][bx] =
				30 + ((bx * 17 + by * 71 + salt * 7) % 190);
		}
	}
}

/* The registered cases. */

/* Stream group ----------------------------------------------------- */

/* Legal sequence header + uneven DC-only intra picture: proves the ES
   constructor and full-plane entry decode independently of motion. */
static int stream_intra_uneven_dc(void) {
	mc_clip clip;
	mc_report rep;
	mc_blocks blocks;
	mc_picdesc *ip;

	memset(&clip, 0, sizeof(clip));
	mc_rich_blocks(&blocks, 3);
	mc_texture_from_blocks(&clip.id[MC_ID_A], &blocks);

	ip = mc_add_pic(&clip, 1, 0, MC_ID_A);
	mc_intra_mbs_from_blocks(ip, &blocks);
	clip.display[0] = 0;
	clip.ndisplay = 1;

	mc_run_clip(&clip, &rep);
	MC_CHECK_REPORT(&rep, 1);
	TASK_CHECK(!rep.wire_overflow);
	MC_CHECK_FRAME(&rep, 0);
	return 0;
}

/* Temporary foundation case: I then an all-zero forward P copy. */
static int stream_p_zero_copy(void) {
	mc_clip clip;
	mc_report rep;
	mc_blocks blocks;
	mc_picdesc *ip, *pp;
	int i;

	memset(&clip, 0, sizeof(clip));
	mc_rich_blocks(&blocks, 5);
	ip = mc_add_pic(&clip, 1, 0, MC_ID_A);
	mc_intra_mbs_from_blocks(ip, &blocks);

	pp = mc_add_pic(&clip, 2, 2, MC_ID_P2);
	for (i = 0; i < MC_NMB; i++) {
		pp->mb[i] = mc_fwd(MC_ID_A, 0, 0);
	}
	clip.display[0] = 0;
	clip.display[1] = 1;
	clip.ndisplay = 2;

	mc_run_clip(&clip, &rep);
	MC_CHECK_REPORT(&rep, 2);
	MC_CHECK_FRAME(&rep, 0);
	MC_CHECK_FRAME(&rep, 1);
	return 0;
}

/* Prediction group ------------------------------------------------- */

/* Luma horizontal/vertical/2D half-pel with positive and negative odd
   components. Every P MB states its final displacement from A. */
static int pred_luma_half_pel_axes(void) {
	static const int vec[MC_NMB][2] = {
		{ 0,  0}, { 1,  0}, {-1,  0},
		{ 0,  3}, { 3,  3}, {-3, -3},
		{ 0, -1}, { 5, -3}, {-5, -5}
	};
	mc_clip clip;
	mc_report rep;
	mc_blocks blocks;
	mc_picdesc *ip, *pp;
	int i;

	memset(&clip, 0, sizeof(clip));
	mc_rich_blocks(&blocks, 9);
	ip = mc_add_pic(&clip, 1, 0, MC_ID_A);
	mc_intra_mbs_from_blocks(ip, &blocks);

	pp = mc_add_pic(&clip, 2, 2, MC_ID_P2);
	for (i = 0; i < MC_NMB; i++) {
		pp->mb[i] = mc_fwd(MC_ID_A, vec[i][0], vec[i][1]);
	}
	clip.display[0] = 0;
	clip.display[1] = 1;
	clip.ndisplay = 2;

	mc_run_clip(&clip, &rep);
	MC_CHECK_REPORT(&rep, 2);
	MC_CHECK_FRAME(&rep, 0);
	MC_CHECK_FRAME(&rep, 1);
	return 0;
}

/* Chroma derivation: luma half-pel component /2 truncated toward zero,
   then chroma's own half-pel sampling. The (-1,0) and (0,-1) MBs are the
   sentinels for a negative floor division (which would yield -1 instead
   of 0 and shift the chroma block into the neighbouring DC block). */
static int pred_chroma_trunc_toward_zero(void) {
	static const int vec[MC_NMB][2] = {
		{ 0,  0}, { 2,  0}, { 0,  0},
		{ 0, -2}, {-1,  0}, {-3,  0},
		{ 0,  0}, { 0, -1}, {-1, -1}
	};
	mc_clip clip;
	mc_report rep;
	mc_blocks blocks;
	mc_picdesc *ip, *pp;
	int by, bx, i;

	memset(&clip, 0, sizeof(clip));
	memset(&blocks, 0, sizeof(blocks));
	/* Flat luma isolates chroma; still legal uneven per-MB DCs. */
	for (by = 0; by < MC_MBH * 2; by++)
		for (bx = 0; bx < MC_MBW * 2; bx++)
			blocks.ydc[by][bx] = 128;

	/* Deliberate high-contrast chroma DC grid. */
	{
		static const int cb[3][3] = {
			{200, 210, 220},
			{220,  40,  60},
			{ 90,  70, 120}
		};
		static const int cr[3][3] = {
			{205, 215, 225},
			{219,  41,  61},
			{ 95,  75, 125}
		};
		for (by = 0; by < 3; by++)
			for (bx = 0; bx < 3; bx++) {
				blocks.cbdc[by][bx] = cb[by][bx];
				blocks.crdc[by][bx] = cr[by][bx];
			}
	}

	ip = mc_add_pic(&clip, 1, 0, MC_ID_A);
	mc_intra_mbs_from_blocks(ip, &blocks);

	pp = mc_add_pic(&clip, 2, 2, MC_ID_P2);
	for (i = 0; i < MC_NMB; i++) {
		pp->mb[i] = mc_fwd(MC_ID_A, vec[i][0], vec[i][1]);
	}
	clip.display[0] = 0;
	clip.display[1] = 1;
	clip.ndisplay = 2;

	mc_run_clip(&clip, &rep);
	MC_CHECK_REPORT(&rep, 2);
	MC_CHECK_FRAME(&rep, 0);
	MC_CHECK_FRAME(&rep, 1);

	/* Explicit hand-computed sentinel spot checks (see the design note):
	   MB4 Cb is the flat centre DC 40 under truncation, not the average
	   with the left DC 220 that floor division would produce. */
	TASK_CHECK(clip.id[MC_ID_P2].cb[(1 * 8) * MC_CW + 1 * 8] == 40);
	TASK_CHECK(clip.id[MC_ID_P2].cb[(1 * 8) * MC_CW + 1 * 8 + 1] == 40);
	return 0;
}

/* Forward-only, backward-only and bidirectional MBs between two different
   all-intra references. MB4 is the +1 final-average rounding sentinel. */
static int pred_bidir_final_rounding(void) {
	mc_clip clip;
	mc_report rep;
	mc_blocks ba, bc;
	mc_picdesc *ia, *ic, *bp;

	memset(&clip, 0, sizeof(clip));
	mc_rich_blocks(&ba, 11);
	mc_rich_blocks(&bc, 23);

	/* Parity pair at the centre MB: A=10, C=11 -> bidi rounds to 11,
	   while a floor final average would give 10. */
	ba.ydc[2][2] = ba.ydc[2][3] = ba.ydc[3][2] = ba.ydc[3][3] = 10;
	bc.ydc[2][2] = bc.ydc[2][3] = bc.ydc[3][2] = bc.ydc[3][3] = 11;
	ba.cbdc[1][1] = ba.crdc[1][1] = 10;
	bc.cbdc[1][1] = bc.crdc[1][1] = 11;

	ia = mc_add_pic(&clip, 1, 0, MC_ID_A);
	mc_intra_mbs_from_blocks(ia, &ba);
	ic = mc_add_pic(&clip, 1, 2, MC_ID_C);
	mc_intra_mbs_from_blocks(ic, &bc);

	bp = mc_add_pic(&clip, 3, 1, MC_ID_B1); /* B, displays first */
	bp->mb[0] = mc_intra(128,128,128,128,128,128);
	bp->mb[1] = mc_fwd(MC_ID_A, 1, 0);
	bp->mb[2] = mc_back(MC_ID_C, -1, 0);
	bp->mb[3] = mc_bidi(MC_ID_A, 1, 1, MC_ID_C, 1, 1);
	bp->mb[4] = mc_bidi(MC_ID_A, 0, 0, MC_ID_C, 0, 0);
	bp->mb[5] = mc_bidi(MC_ID_A, 0, 1, MC_ID_C, 0, -1);
	/* Horizontal-only odd bidi: exercises the other final-average form. */
	bp->mb[6] = mc_bidi(MC_ID_A, 1, 0, MC_ID_C, 1, 0);
	/* Bottom-row MB: vertical components stay negative so the odd
	   footprint never extends past the picture edge. */
	bp->mb[7] = mc_bidi(MC_ID_A, 1, -1, MC_ID_C, -1, -1);
	bp->mb[8] = mc_intra(128,128,128,128,128,128);

	/* Coding order 0(A),1(C),2(B); display order A,B,C. */
	clip.display[0] = 0;
	clip.display[1] = 2;
	clip.display[2] = 1;
	clip.ndisplay = 3;

	mc_run_clip(&clip, &rep);
	MC_CHECK_REPORT(&rep, 3);
	MC_CHECK_FRAME(&rep, 0);
	MC_CHECK_FRAME(&rep, 1);
	MC_CHECK_FRAME(&rep, 2);

	/* Hand check: every luma byte of centre MB4 is 11. */
	TASK_CHECK(clip.id[MC_ID_B1].y[(16) * MC_W + 16] == 11);
	TASK_CHECK(clip.id[MC_ID_B1].y[20 * MC_W + 20] == 11);
	TASK_CHECK(clip.id[MC_ID_B1].cb[8 * MC_CW + 8] == 11);
	return 0;
}

/* Stream group: full I/P/B coding order differing from display order. */

static int stream_ipb_display_order(void) {
	static const int vec[MC_NMB][2] = {
		{ 0,  0}, { 1,  0}, {-1,  0},
		{ 0,  3}, { 3,  3}, {-3, -3},
		{ 0, -1}, { 5, -3}, {-5, -5}
	};
	mc_clip clip;
	mc_report rep;
	mc_blocks blocks;
	mc_picdesc *ip, *p2, *b1, *p4, *b3;
	int i;

	memset(&clip, 0, sizeof(clip));
	mc_rich_blocks(&blocks, 31);

	/* Coding order: I0 A, P2, B1, P4, B3 (+trailer).
	   Display order: I0, B1, P2, B3, P4. */
	ip = mc_add_pic(&clip, 1, 0, MC_ID_A);
	mc_intra_mbs_from_blocks(ip, &blocks);

	p2 = mc_add_pic(&clip, 2, 2, MC_ID_P2);
	for (i = 0; i < MC_NMB; i++) {
		p2->mb[i] = mc_fwd(MC_ID_A, vec[i][0], vec[i][1]);
	}

	b1 = mc_add_pic(&clip, 3, 1, MC_ID_B1);
	b1->mb[0] = mc_intra(128,128,128,128,128,128);
	b1->mb[1] = mc_fwd(MC_ID_A, 1, 0);
	b1->mb[2] = mc_back(MC_ID_P2, -1, 0);
	b1->mb[3] = mc_bidi(MC_ID_A, 1, 1, MC_ID_P2, 1, 1);
	b1->mb[4] = mc_bidi(MC_ID_A, 0, 0, MC_ID_P2, 0, 0);
	b1->mb[5] = mc_bidi(MC_ID_A, 0, 1, MC_ID_P2, 0, -1);
	b1->mb[6] = mc_bidi(MC_ID_A, 1, 0, MC_ID_P2, 1, 0);
	b1->mb[7] = mc_bidi(MC_ID_A, 1, -1, MC_ID_P2, -1, -1);
	b1->mb[8] = mc_intra(128,128,128,128,128,128);

	p4 = mc_add_pic(&clip, 2, 4, MC_ID_P4);
	for (i = 0; i < MC_NMB; i++) {
		p4->mb[i] = mc_fwd(MC_ID_P2, vec[i][0], vec[i][1]);
	}

	b3 = mc_add_pic(&clip, 3, 3, MC_ID_B3);
	for (i = 0; i < MC_NMB; i++) {
		b3->mb[i] = mc_bidi(MC_ID_P2, 0, 0, MC_ID_P4, 0, 0);
	}
	b3->mb[4] = mc_bidi(MC_ID_P2, 1, 1, MC_ID_P4, -1, -1);

	clip.display[0] = 0; /* I0 */
	clip.display[1] = 2; /* B1 */
	clip.display[2] = 1; /* P2 */
	clip.display[3] = 4; /* B3 */
	clip.display[4] = 3; /* P4 */
	clip.ndisplay = 5;

	/* The registration table really is a reordering. */
	TASK_CHECK(clip.display[1] == 2 && clip.display[2] == 1);
	TASK_CHECK(clip.display[3] == 4 && clip.display[4] == 3);

	mc_run_clip(&clip, &rep);
	MC_CHECK_REPORT(&rep, 5);
	MC_CHECK_FRAME(&rep, 0);
	MC_CHECK_FRAME(&rep, 1);
	MC_CHECK_FRAME(&rep, 2);
	MC_CHECK_FRAME(&rep, 3);
	MC_CHECK_FRAME(&rep, 4);
	return 0;
}

/* State group ------------------------------------------------------ */

/* Differential MV accumulation: +3 then +2 lands on final 5. */
static int state_differential_accumulation(void) {
	mc_clip clip;
	mc_report rep;
	mc_blocks blocks;
	mc_picdesc *ip, *pp;
	int i;

	memset(&clip, 0, sizeof(clip));
	mc_rich_blocks(&blocks, 41);
	ip = mc_add_pic(&clip, 1, 0, MC_ID_A);
	mc_intra_mbs_from_blocks(ip, &blocks);

	pp = mc_add_pic(&clip, 2, 2, MC_ID_P2);
	pp->auto_deltas = 0;
	pp->mb[0] = mc_fwd(MC_ID_A, 3, 0);            /* delta +3 -> 3 */
	pp->mb[1] = mc_fwd(MC_ID_A, 5, 2);
	pp->mb[1].dh = 2;                             /* 3 + 2 -> 5 */
	pp->mb[1].dv = 2;                             /* 0 + 2 -> 2 */
	for (i = 2; i < MC_NMB; i++)
		pp->mb[i] = mc_intra(128,128,128,128,128,128);

	clip.display[0] = 0; clip.display[1] = 1; clip.ndisplay = 2;
	mc_run_clip(&clip, &rep);
	MC_CHECK_REPORT(&rep, 2);
	MC_CHECK_FRAME(&rep, 0);
	MC_CHECK_FRAME(&rep, 1);
	return 0;
}

/* f_code=2 (r_size=1): m_code plus residual bits, still accumulated. */
static int state_fcode_residual_bits(void) {
	mc_clip clip;
	mc_report rep;
	mc_blocks blocks;
	mc_picdesc *ip, *pp;
	int i;

	memset(&clip, 0, sizeof(clip));
	mc_rich_blocks(&blocks, 43);
	ip = mc_add_pic(&clip, 1, 0, MC_ID_A);
	mc_intra_mbs_from_blocks(ip, &blocks);

	pp = mc_add_pic(&clip, 2, 2, MC_ID_P2);
	pp->auto_deltas = 0;
	pp->fcode_f = 2;
	/* +5 = m_code 3, residual 0; +2 v = m_code 1, residual 1. */
	pp->mb[0] = mc_fwd(MC_ID_A, 5, 2);
	/* Then +2 h (m_code1,r1) -> 7 and -1 v (m_code -1,r0) -> 1. */
	pp->mb[1] = mc_fwd(MC_ID_A, 7, 1);
	pp->mb[1].dh = 2;
	pp->mb[1].dv = -1;
	for (i = 2; i < MC_NMB; i++)
		pp->mb[i] = mc_intra(128,128,128,128,128,128);

	clip.display[0] = 0; clip.display[1] = 1; clip.ndisplay = 2;
	mc_run_clip(&clip, &rep);
	MC_CHECK_REPORT(&rep, 2);
	MC_CHECK_FRAME(&rep, 1);
	return 0;
}

/* full_pel_forward_vector doubles the decoded integer displacement. */
static int state_full_pel_scaling(void) {
	mc_clip clip;
	mc_report rep;
	mc_blocks blocks;
	mc_picdesc *ip, *pp;
	int i;

	memset(&clip, 0, sizeof(clip));
	mc_rich_blocks(&blocks, 47);
	ip = mc_add_pic(&clip, 1, 0, MC_ID_A);
	mc_intra_mbs_from_blocks(ip, &blocks);

	pp = mc_add_pic(&clip, 2, 2, MC_ID_P2);
	pp->auto_deltas = 0;
	pp->full_f = 1;
	/* Wire codes (2,1) integer pels; decoder doubles to final (4,2)
	   half-luma units, which is what the reference samples. */
	pp->mb[0] = mc_fwd(MC_ID_A, 4, 2);
	pp->mb[0].dh = 2;
	pp->mb[0].dv = 1;
	for (i = 1; i < MC_NMB; i++)
		pp->mb[i] = mc_intra(128,128,128,128,128,128);

	clip.display[0] = 0; clip.display[1] = 1; clip.ndisplay = 2;
	mc_run_clip(&clip, &rep);
	MC_CHECK_REPORT(&rep, 2);
	MC_CHECK_FRAME(&rep, 1);
	return 0;
}

/* Differential wrap-around in both signs (f_code=1 scale). */
static int state_differential_wrap(void) {
	mc_clip clip;
	mc_report rep;
	mc_blocks blocks;
	mc_picdesc *ip, *pp;

	memset(&clip, 0, sizeof(clip));
	mc_rich_blocks(&blocks, 53);
	ip = mc_add_pic(&clip, 1, 0, MC_ID_A);
	mc_intra_mbs_from_blocks(ip, &blocks);

	pp = mc_add_pic(&clip, 2, 2, MC_ID_P2);
	pp->auto_deltas = 0;
	pp->mb[0] = mc_fwd(MC_ID_A, 0, 0);
	/* Positive: 0 +14 =14, then +3 =17 >15 -> 17-32 = -15. */
	pp->mb[1] = mc_fwd(MC_ID_A, 14, 0);  pp->mb[1].dh = 14;
	pp->mb[2] = mc_fwd(MC_ID_A, -15, 0); pp->mb[2].dh = 3;
	/* Intra resets the state before the negative run. */
	pp->mb[3] = mc_intra(128,128,128,128,128,128);
	/* Negative: -14, -16 (inclusive bound), then -1 -> -17 < -16,
	   so -17 +32 = +15. */
	pp->mb[4] = mc_fwd(MC_ID_A, -14, 0); pp->mb[4].dh = -14;
	pp->mb[5] = mc_fwd(MC_ID_A, -16, 0); pp->mb[5].dh = -2;
	pp->mb[6] = mc_fwd(MC_ID_A, 15, 0);  pp->mb[6].dh = -1;
	pp->mb[7] = mc_intra(128,128,128,128,128,128);
	pp->mb[8] = mc_intra(128,128,128,128,128,128);

	clip.display[0] = 0; clip.display[1] = 1; clip.ndisplay = 2;
	mc_run_clip(&clip, &rep);
	MC_CHECK_REPORT(&rep, 2);
	MC_CHECK_FRAME(&rep, 1);
	return 0;
}

/* Consecutive skipped P MBs are colocated because an address gap clears
   the forward vector; they must not inherit MB0's +5. */
static int state_p_skipped_clear_vectors(void) {
	mc_clip clip;
	mc_report rep;
	mc_blocks blocks;
	mc_picdesc *ip, *pp;
	mc_mb skip;
	int i;

	memset(&clip, 0, sizeof(clip));
	mc_rich_blocks(&blocks, 59);
	ip = mc_add_pic(&clip, 1, 0, MC_ID_A);
	mc_intra_mbs_from_blocks(ip, &blocks);

	pp = mc_add_pic(&clip, 2, 2, MC_ID_P2);
	pp->auto_deltas = 0;
	pp->mb[0] = mc_fwd(MC_ID_A, 5, 0);
	skip = mc_fwd(MC_ID_A, 0, 0);
	skip.skip = 1;
	pp->mb[1] = skip; /* skipped */
	pp->mb[2] = skip; /* two consecutive skipped MBs */
	/* Coded MB3: after the gap the decoder state is 0; delta +2 -> 2. */
	pp->mb[3] = mc_fwd(MC_ID_A, 2, 0);
	for (i = 4; i < MC_NMB; i++)
		pp->mb[i] = mc_intra(128,128,128,128,128,128);

	clip.display[0] = 0; clip.display[1] = 1; clip.ndisplay = 2;
	mc_run_clip(&clip, &rep);
	MC_CHECK_REPORT(&rep, 2);
	MC_CHECK_FRAME(&rep, 1);
	return 0;
}

/* A skipped B MB inherits the previous MB's direction and vectors. */
static int state_b_skipped_inherit(void) {
	mc_clip clip;
	mc_report rep;
	mc_blocks ba, bc;
	mc_picdesc *ia, *ic, *bp;
	mc_mb skip;
	int i;

	memset(&clip, 0, sizeof(clip));
	mc_rich_blocks(&ba, 61);
	mc_rich_blocks(&bc, 67);
	ia = mc_add_pic(&clip, 1, 0, MC_ID_A);
	mc_intra_mbs_from_blocks(ia, &ba);
	ic = mc_add_pic(&clip, 1, 2, MC_ID_C);
	mc_intra_mbs_from_blocks(ic, &bc);

	bp = mc_add_pic(&clip, 3, 1, MC_ID_B1);
	bp->auto_deltas = 0;
	bp->mb[0] = mc_bidi(MC_ID_A, 3, 1, MC_ID_C, 2, 1);
	skip = bp->mb[0];
	skip.skip = 1;
	bp->mb[1] = skip; /* skipped: must repeat the bidi vectors */
	/* Next coded MB changes to forward-only; deltas are relative to the
	   inherited forward state (3,1), so (-3,-1) lands on final (0,0),
	   footprint-safe at the right-edge MB. */
	bp->mb[2] = mc_fwd(MC_ID_A, 0, 0);
	bp->mb[2].dh = -3;
	bp->mb[2].dv = -1;
	for (i = 3; i < MC_NMB; i++)
		bp->mb[i] = mc_intra(128,128,128,128,128,128);

	clip.display[0] = 0;
	clip.display[1] = 2;
	clip.display[2] = 1;
	clip.ndisplay = 3;
	mc_run_clip(&clip, &rep);
	MC_CHECK_REPORT(&rep, 3);
	MC_CHECK_FRAME(&rep, 1); /* the B picture */
	return 0;
}

/* A new slice resets motion vectors at its start. */
static int state_slice_start_reset(void) {
	mc_clip clip;
	mc_report rep;
	mc_blocks blocks;
	mc_picdesc *ip, *pp;
	int i;

	memset(&clip, 0, sizeof(clip));
	mc_rich_blocks(&blocks, 71);
	ip = mc_add_pic(&clip, 1, 0, MC_ID_A);
	mc_intra_mbs_from_blocks(ip, &blocks);

	pp = mc_add_pic(&clip, 2, 2, MC_ID_P2);
	pp->auto_deltas = 0;
	pp->slices = MC_MBH; /* one slice per MB row */
	pp->mb[0] = mc_fwd(MC_ID_A, 0, 0);
	pp->mb[1] = mc_fwd(MC_ID_A, 0, 0);
	pp->mb[2] = mc_fwd(MC_ID_A, -4, 0); /* end slice 1 at -4 */
	/* Slice 2 must reset to 0; +5 gives final 5, not -4+5 = 1. */
	pp->mb[3] = mc_fwd(MC_ID_A, 5, 0);
	for (i = 4; i < MC_NMB; i++)
		pp->mb[i] = mc_intra(128,128,128,128,128,128);

	clip.display[0] = 0; clip.display[1] = 1; clip.ndisplay = 2;
	mc_run_clip(&clip, &rep);
	MC_CHECK_REPORT(&rep, 2);
	MC_CHECK_FRAME(&rep, 1);
	return 0;
}

const struct task_case task_cases[] = {
	{ "stream", "stream_intra_uneven_dc", stream_intra_uneven_dc },
	{ "stream", "stream_p_zero_copy", stream_p_zero_copy },
	{ "stream", "stream_ipb_display_order", stream_ipb_display_order },

	{ "prediction", "pred_luma_half_pel_axes", pred_luma_half_pel_axes },
	{ "prediction", "pred_chroma_trunc_toward_zero", pred_chroma_trunc_toward_zero },
	{ "prediction", "pred_bidir_final_rounding", pred_bidir_final_rounding },

	{ "state", "state_differential_accumulation", state_differential_accumulation },
	{ "state", "state_fcode_residual_bits", state_fcode_residual_bits },
	{ "state", "state_full_pel_scaling", state_full_pel_scaling },
	{ "state", "state_differential_wrap", state_differential_wrap },
	{ "state", "state_p_skipped_clear_vectors", state_p_skipped_clear_vectors },
	{ "state", "state_b_skipped_inherit", state_b_skipped_inherit },
	{ "state", "state_slice_start_reset", state_slice_start_reset }
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
