/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2000-2025
 *					All rights reserved
 *
 *  This file is part of GPAC / ISO Media File Format sub-project
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <gpac/internal/isomedia_dev.h>
#include <gpac/constants.h>

#if !defined(GPAC_DISABLE_ISOM) && !defined(GPAC_DISABLE_ISOM_WRITE)

/*a highlight segment: active over [t0, t1) relative to the sample start, covering chars [c0, c1)*/
typedef struct
{
	u64 t0, t1;
	u32 c0, c1;
} BakeHiSeg;

static GF_Err bake_push_seg(BakeHiSeg **segs, u32 *nb, u32 *alloc, u64 t0, u64 t1, u32 c0, u32 c1)
{
	if (*nb == *alloc) {
		BakeHiSeg *ns;
		*alloc = *alloc ? 2*(*alloc) : 8;
		ns = (BakeHiSeg *)gf_realloc(*segs, sizeof(BakeHiSeg) * (*alloc));
		if (!ns) return GF_OUT_OF_MEM;
		*segs = ns;
	}
	(*segs)[*nb].t0 = t0;
	(*segs)[*nb].t1 = t1;
	(*segs)[*nb].c0 = c0;
	(*segs)[*nb].c1 = c1;
	(*nb)++;
	return GF_OK;
}

static GF_Err bake_push_u64(u64 **vals, u32 *nb, u32 *alloc, u64 v)
{
	if (*nb == *alloc) {
		u64 *nv;
		*alloc = *alloc ? 2*(*alloc) : 16;
		nv = (u64 *)gf_realloc(*vals, sizeof(u64) * (*alloc));
		if (!nv) return GF_OUT_OF_MEM;
		*vals = nv;
	}
	(*vals)[*nb] = v;
	(*nb)++;
	return GF_OK;
}

static GF_Err bake_push_u32(u32 **vals, u32 *nb, u32 *alloc, u32 v)
{
	if (*nb == *alloc) {
		u32 *nv;
		*alloc = *alloc ? 2*(*alloc) : 16;
		nv = (u32 *)gf_realloc(*vals, sizeof(u32) * (*alloc));
		if (!nv) return GF_OUT_OF_MEM;
		*vals = nv;
	}
	(*vals)[*nb] = v;
	(*nb)++;
	return GF_OK;
}

static GF_Err bake_push_style(GF_StyleRecord **recs, u32 *nb, u32 *alloc, GF_StyleRecord *r)
{
	if (*nb == *alloc) {
		GF_StyleRecord *nr;
		*alloc = *alloc ? 2*(*alloc) : 8;
		nr = (GF_StyleRecord *)gf_realloc(*recs, sizeof(GF_StyleRecord) * (*alloc));
		if (!nr) return GF_OUT_OF_MEM;
		*recs = nr;
	}
	(*recs)[*nb] = *r;
	(*nb)++;
	return GF_OK;
}

static void bake_sort_u64(u64 *vals, u32 nb)
{
	u32 i, j;
	for (i=1; i<nb; i++) {
		u64 v = vals[i];
		for (j=i; j && vals[j-1]>v; j--) vals[j] = vals[j-1];
		vals[j] = v;
	}
}

static void bake_sort_u32(u32 *vals, u32 nb)
{
	u32 i, j;
	for (i=1; i<nb; i++) {
		u32 v = vals[i];
		for (j=i; j && vals[j-1]>v; j--) vals[j] = vals[j-1];
		vals[j] = v;
	}
}

/*counts Unicode code points, validating UTF-8 or BOM-marked UTF-16 (both endiannesses)*/
static GF_Err bake_text_char_count(const char *text, u32 len, u32 *nb_chars)
{
	const u8 *p = (const u8 *)text;
	u32 i, count = 0;
	*nb_chars = 0;
	if (!len) return GF_OK;

	/*UTF-16 signaled by a BOM*/
	if ((len>=2) && ( ((p[0]==0xFE) && (p[1]==0xFF)) || ((p[0]==0xFF) && (p[1]==0xFE)) )) {
		Bool be = (p[0]==0xFE) ? GF_TRUE : GF_FALSE;
		if ((len-2) % 2) return GF_CORRUPTED_DATA;
		i = 2;
		while (i<len) {
			u16 w = be ? (u16)((p[i]<<8)|p[i+1]) : (u16)((p[i+1]<<8)|p[i]);
			if ((w>=0xD800) && (w<=0xDBFF)) {
				u16 w2;
				if (i+4>len) return GF_CORRUPTED_DATA;
				w2 = be ? (u16)((p[i+2]<<8)|p[i+3]) : (u16)((p[i+3]<<8)|p[i+2]);
				if ((w2<0xDC00) || (w2>0xDFFF)) return GF_CORRUPTED_DATA;
				i += 4;
			} else if ((w>=0xDC00) && (w<=0xDFFF)) {
				return GF_CORRUPTED_DATA;
			} else {
				i += 2;
			}
			count++;
		}
		*nb_chars = count;
		return GF_OK;
	}

	/*UTF-8*/
	i = 0;
	while (i<len) {
		u8 c = p[i];
		u32 j, clen, cp;
		if (c<0x80) {
			i++;
			count++;
			continue;
		}
		if ((c>=0xC2) && (c<=0xDF)) { clen=2; cp=c&0x1F; }
		else if ((c>=0xE0) && (c<=0xEF)) { clen=3; cp=c&0x0F; }
		else if ((c>=0xF0) && (c<=0xF4)) { clen=4; cp=c&0x07; }
		else return GF_CORRUPTED_DATA;
		if (i+clen>len) return GF_CORRUPTED_DATA;
		for (j=1; j<clen; j++) {
			if ((p[i+j]&0xC0)!=0x80) return GF_CORRUPTED_DATA;
			cp = (cp<<6) | (p[i+j]&0x3F);
		}
		if ((clen==3) && (cp<0x800)) return GF_CORRUPTED_DATA;
		if ((clen==4) && (cp<0x10000)) return GF_CORRUPTED_DATA;
		if ((cp>=0xD800) && (cp<=0xDFFF)) return GF_CORRUPTED_DATA;
		if (cp>0x10FFFF) return GF_CORRUPTED_DATA;
		i += clen;
		count++;
	}
	*nb_chars = count;
	return GF_OK;
}

/*checks the serialized modifiers of a tx3g sample: only styl, krok, hlit, hclr, tbox, twrp,
frcd and href are allowed, anything else is not supported*/
static GF_Err bake_check_modifiers(const u8 *data, u32 len)
{
	u32 pos, tlen;
	if (!len) return GF_OK;
	if (len<2) return GF_CORRUPTED_DATA;
	tlen = (data[0]<<8) | data[1];
	if (tlen > len-2) return GF_CORRUPTED_DATA;
	pos = 2 + tlen;
	while (pos<len) {
		u32 size, type;
		if (len-pos<8) return GF_CORRUPTED_DATA;
		size = (data[pos]<<24) | (data[pos+1]<<16) | (data[pos+2]<<8) | data[pos+3];
		type = GF_4CC(data[pos+4], data[pos+5], data[pos+6], data[pos+7]);
		if ((size<8) || (size>len-pos)) return GF_CORRUPTED_DATA;
		switch (type) {
		case GF_ISOM_BOX_TYPE_STYL:
		case GF_ISOM_BOX_TYPE_KROK:
		case GF_ISOM_BOX_TYPE_HLIT:
		case GF_ISOM_BOX_TYPE_HCLR:
		case GF_ISOM_BOX_TYPE_TBOX:
		case GF_ISOM_BOX_TYPE_TWRP:
		case GF_ISOM_BOX_TYPE_HREF:
		case GF_QT_BOX_TYPE_FRCD:
			break;
		default:
			return GF_NOT_SUPPORTED;
		}
		pos += size;
	}
	return GF_OK;
}

static Bool bake_same_style(GF_StyleRecord *a, GF_StyleRecord *b)
{
	return (a->fontID==b->fontID)
	       && (a->style_flags==b->style_flags)
	       && (a->font_size==b->font_size)
	       && (a->text_color==b->text_color);
}

/*resolves the effective styles over the whole text: default style from the sample description,
source styl records applied in order, then the highlight color forced on the highlighted ranges.
Outputs an ordered, non-overlapping, merged list of style records for the ranges differing
from the default style*/
static GF_Err bake_compute_styles(GF_StyleRecord *def_style, GF_TextStyleBox *styles, BakeHiSeg *hi, u32 nb_hi, u32 hclr, u32 nb_chars, GF_StyleRecord **out_recs, u32 *nb_recs)
{
	GF_Err e = GF_OK;
	u32 *bounds = NULL;
	u32 nb_b = 0, alloc_b = 0;
	GF_StyleRecord *recs = NULL;
	u32 nb_r = 0, alloc_r = 0;
	u32 i, j, out_nb;

	*out_recs = NULL;
	*nb_recs = 0;

	e = bake_push_u32(&bounds, &nb_b, &alloc_b, 0);
	if (!e) e = bake_push_u32(&bounds, &nb_b, &alloc_b, nb_chars);
	if (e) goto exit;

	if (styles) {
		for (i=0; i<styles->entry_count; i++) {
			GF_StyleRecord *r = &styles->styles[i];
			if ((r->startCharOffset > r->endCharOffset) || (r->endCharOffset > nb_chars)) {
				e = GF_CORRUPTED_DATA;
				goto exit;
			}
			e = bake_push_u32(&bounds, &nb_b, &alloc_b, r->startCharOffset);
			if (!e) e = bake_push_u32(&bounds, &nb_b, &alloc_b, r->endCharOffset);
			if (e) goto exit;
		}
	}
	for (i=0; i<nb_hi; i++) {
		e = bake_push_u32(&bounds, &nb_b, &alloc_b, hi[i].c0);
		if (!e) e = bake_push_u32(&bounds, &nb_b, &alloc_b, hi[i].c1);
		if (e) goto exit;
	}

	bake_sort_u32(bounds, nb_b);

	for (i=0; i+1<nb_b; i++) {
		u32 b0 = bounds[i], b1 = bounds[i+1];
		GF_StyleRecord st;
		Bool in_hi = GF_FALSE;
		if (b0==b1) continue;
		st = *def_style;
		/*apply styl records in order*/
		if (styles) {
			for (j=0; j<styles->entry_count; j++) {
				GF_StyleRecord *r = &styles->styles[j];
				if ((r->startCharOffset<=b0) && (b0<r->endCharOffset)) {
					st.fontID = r->fontID;
					st.style_flags = r->style_flags;
					st.font_size = r->font_size;
					st.text_color = r->text_color;
				}
			}
		}
		/*then only override the color on highlighted ranges*/
		for (j=0; j<nb_hi; j++) {
			if ((hi[j].c0<=b0) && (b0<hi[j].c1)) {
				in_hi = GF_TRUE;
				break;
			}
		}
		if (in_hi) st.text_color = hclr;

		/*merge with the previous range if the complete style is the same*/
		if (nb_r && (recs[nb_r-1].endCharOffset==b0) && bake_same_style(&recs[nb_r-1], &st)) {
			recs[nb_r-1].endCharOffset = (u16)b1;
		} else {
			if (b1>0xFFFF) {
				e = GF_NOT_SUPPORTED;
				goto exit;
			}
			st.startCharOffset = (u16)b0;
			st.endCharOffset = (u16)b1;
			e = bake_push_style(&recs, &nb_r, &alloc_r, &st);
			if (e) goto exit;
		}
	}

	/*only keep ranges differing from the default style - the list stays ordered and
	non-overlapping*/
	out_nb = 0;
	for (i=0; i<nb_r; i++) {
		if (bake_same_style(&recs[i], def_style)) continue;
		recs[out_nb] = recs[i];
		out_nb++;
	}
	*out_recs = recs;
	*nb_recs = out_nb;
	recs = NULL;

exit:
	gf_free(bounds);
	if (recs) gf_free(recs);
	return e;
}

/*output writer: merges adjacent segments with identical payloads (hence identical effective
styles) into a single sample, never across source sample boundaries*/
typedef struct
{
	GF_ISOFile *dst;
	u32 track;
	u64 next_dts;
	GF_ISOSample *pending;
	u32 pending_dur;
	u32 pending_desc;
} BakeWriter;

static GF_Err bake_writer_flush(BakeWriter *w)
{
	GF_Err e;
	if (!w->pending) return GF_OK;
	w->pending->DTS = w->next_dts;
	w->pending->CTS_Offset = 0;
	e = gf_isom_add_sample(w->dst, w->track, w->pending_desc, w->pending);
	if (!e) e = gf_isom_set_last_sample_duration(w->dst, w->track, w->pending_dur);
	if (!e) w->next_dts += w->pending_dur;
	gf_isom_sample_del(&w->pending);
	w->pending_dur = 0;
	return e;
}

static GF_Err bake_writer_push(BakeWriter *w, GF_ISOSample *samp, u32 dur, u32 desc_idx)
{
	GF_Err e;
	/*zero-length segments are not output*/
	if (!dur) {
		gf_isom_sample_del(&samp);
		return GF_OK;
	}
	if (w->pending
	        && (w->pending->dataLength==samp->dataLength)
	        && !memcmp(w->pending->data, samp->data, samp->dataLength)) {
		w->pending_dur += dur;
		gf_isom_sample_del(&samp);
		return GF_OK;
	}
	e = bake_writer_flush(w);
	if (e) {
		gf_isom_sample_del(&samp);
		return e;
	}
	w->pending = samp;
	w->pending_dur = dur;
	w->pending_desc = desc_idx;
	return GF_OK;
}

static GF_Err bake_convert_sample(BakeWriter *w, GF_ISOSample *src_samp, u32 desc_idx, GF_StyleRecord *def_style, u32 media_dur)
{
	GF_Err e;
	GF_TextSample *ts = NULL;
	GF_BitStream *bs;
	GF_Box *a;
	BakeHiSeg *segs = NULL;
	u32 nb_segs = 0, alloc_segs = 0;
	u64 *events = NULL;
	u32 nb_ev = 0, alloc_ev = 0;
	u32 nb_chars = 0, i;
	u32 hclr = 0;
	Bool has_highlight = GF_FALSE;

	/*reject unsupported sample extensions before parsing (the text sample parser silently
	drops unknown boxes)*/
	e = bake_check_modifiers((u8 *)src_samp->data, src_samp->dataLength);
	if (e) return e;

	bs = gf_bs_new(src_samp->data, src_samp->dataLength, GF_BITSTREAM_READ);
	if (!bs) return GF_OUT_OF_MEM;
	ts = gf_isom_parse_text_sample(bs);
	gf_bs_del(bs);
	if (!ts) return GF_OUT_OF_MEM;

	e = bake_text_char_count(ts->text, ts->len, &nb_chars);
	if (e) goto exit;

	/*collect the highlight segments of ALL karaoke sequences and of the static highlights*/
	i = 0;
	while ((a = (GF_Box*)gf_list_enum(ts->others, &i))) {
		if (a->type==GF_ISOM_BOX_TYPE_KROK) {
			GF_TextKaraokeBox *k = (GF_TextKaraokeBox *)a;
			u64 seg_start = k->highlight_starttime;
			u32 j;
			if (seg_start > media_dur) {
				e = GF_CORRUPTED_DATA;
				goto exit;
			}
			for (j=0; j<k->nb_entries; j++) {
				KaraokeRecord *r = &k->records[j];
				if ((r->highlight_endtime < seg_start) || (r->highlight_endtime > media_dur)) {
					e = GF_CORRUPTED_DATA;
					goto exit;
				}
				if ((r->start_charoffset > r->end_charoffset) || (r->end_charoffset > nb_chars)) {
					e = GF_CORRUPTED_DATA;
					goto exit;
				}
				e = bake_push_seg(&segs, &nb_segs, &alloc_segs, seg_start, r->highlight_endtime, r->start_charoffset, r->end_charoffset);
				if (e) goto exit;
				seg_start = r->highlight_endtime;
			}
		} else if (a->type==GF_ISOM_BOX_TYPE_HLIT) {
			GF_TextHighlightBox *h = (GF_TextHighlightBox *)a;
			if ((h->startcharoffset > h->endcharoffset) || (h->endcharoffset > nb_chars)) {
				e = GF_CORRUPTED_DATA;
				goto exit;
			}
			/*static highlight is effective for the whole sample*/
			e = bake_push_seg(&segs, &nb_segs, &alloc_segs, 0, media_dur, h->startcharoffset, h->endcharoffset);
			if (e) goto exit;
		} else if (a->type==GF_ISOM_BOX_TYPE_HREF) {
			GF_TextHyperTextBox *h = (GF_TextHyperTextBox *)a;
			if ((h->startcharoffset > h->endcharoffset) || (h->endcharoffset > nb_chars)) {
				e = GF_CORRUPTED_DATA;
				goto exit;
			}
		}
	}

	/*a non-empty highlight requires a highlight color*/
	for (i=0; i<nb_segs; i++) {
		if ((segs[i].t1>segs[i].t0) && (segs[i].c1>segs[i].c0)) has_highlight = GF_TRUE;
	}
	if (has_highlight && !ts->highlight_color) {
		e = GF_NOT_SUPPORTED;
		goto exit;
	}
	if (ts->highlight_color) hclr = ts->highlight_color->hil_color;

	/*event points: sample bounds plus all segment bounds - at a given time, expiring ranges
	are removed before new ones are added (half-open intervals)*/
	e = bake_push_u64(&events, &nb_ev, &alloc_ev, 0);
	if (!e) e = bake_push_u64(&events, &nb_ev, &alloc_ev, media_dur);
	if (e) goto exit;
	for (i=0; i<nb_segs; i++) {
		e = bake_push_u64(&events, &nb_ev, &alloc_ev, segs[i].t0);
		if (!e) e = bake_push_u64(&events, &nb_ev, &alloc_ev, segs[i].t1);
		if (e) goto exit;
	}
	bake_sort_u64(events, nb_ev);

	for (i=0; i+1<nb_ev; i++) {
		u64 t0 = events[i], t1 = events[i+1];
		BakeHiSeg *act = NULL;
		u32 nb_act = 0, alloc_act = 0;
		GF_StyleRecord *recs = NULL;
		u32 nb_recs = 0, j;
		GF_TextSample *out = NULL;
		GF_ISOSample *os = NULL;

		if (t0==t1) continue;

		/*union of the highlight ranges active on this interval*/
		for (j=0; j<nb_segs; j++) {
			if ((segs[j].t0<=t0) && (t0<segs[j].t1) && (segs[j].c1>segs[j].c0)) {
				e = bake_push_seg(&act, &nb_act, &alloc_act, 0, 0, segs[j].c0, segs[j].c1);
				if (e) {
					gf_free(act);
					goto exit;
				}
			}
		}

		e = bake_compute_styles(def_style, ts->styles, act, nb_act, hclr, nb_chars, &recs, &nb_recs);
		gf_free(act);
		if (e) goto exit;

		/*rebuild the sample: full original text, computed styles, tbox/twrp/frcd/href
		preserved, krok/hlit/hclr dropped*/
		out = gf_isom_new_text_sample();
		if (!out) {
			gf_free(recs);
			e = GF_OUT_OF_MEM;
			goto exit;
		}
		e = gf_isom_text_add_text(out, ts->text, ts->len);
		for (j=0; !e && (j<nb_recs); j++)
			e = gf_isom_text_add_style(out, &recs[j]);
		if (!e && ts->box)
			e = gf_isom_text_set_box(out, ts->box->box.top, ts->box->box.left, ts->box->box.bottom, ts->box->box.right);
		if (!e && ts->wrap)
			e = gf_isom_text_set_wrap(out, ts->wrap->wrap_flag);
		if (!e && ts->is_forced)
			e = gf_isom_text_set_forced(out, GF_TRUE);
		j = 0;
		while (!e && (a = (GF_Box*)gf_list_enum(ts->others, &j))) {
			if (a->type==GF_ISOM_BOX_TYPE_HREF) {
				GF_TextHyperTextBox *h = (GF_TextHyperTextBox *)a;
				e = gf_isom_text_add_hyperlink(out, h->URL, h->URL_hint, h->startcharoffset, h->endcharoffset);
			}
		}
		gf_free(recs);
		if (!e) os = gf_isom_text_to_sample(out);
		gf_isom_delete_text_sample(out);
		if (e) goto exit;
		if (!os) {
			e = GF_OUT_OF_MEM;
			goto exit;
		}
		os->IsRAP = src_samp->IsRAP;
		e = bake_writer_push(w, os, (u32)(t1-t0), desc_idx);
		if (e) goto exit;
	}

exit:
	if (ts) gf_isom_delete_text_sample(ts);
	if (segs) gf_free(segs);
	if (events) gf_free(events);
	return e;
}

GF_EXPORT
GF_Err gf_isom_text_bake_karaoke(GF_ISOFile *src, u32 track, GF_ISOFile *dst, u32 *out_track)
{
	GF_Err e;
	GF_TrackBox *trak;
	BakeWriter w;
	GF_StyleRecord *def_styles = NULL;
	u32 i, nb_samples, nb_descs, timescale, media_type, dst_track = 0;
	u64 src_dts;

	if (out_track) *out_track = 0;
	if (!src || !dst || !out_track || !track) return GF_BAD_PARAM;
	if (src==dst) return GF_BAD_PARAM;

	e = gf_isom_can_access_movie(dst, GF_ISOM_OPEN_WRITE);
	if (e) return e;

	trak = gf_isom_get_track_box(src, track);
	if (!trak || !trak->Media || !trak->Media->handler) return GF_BAD_PARAM;

	switch (trak->Media->handler->handlerType) {
	case GF_ISOM_MEDIA_TEXT:
	case GF_ISOM_MEDIA_SUBT:
	case GF_ISOM_MEDIA_MPEG_SUBT:
		break;
	default:
		return GF_BAD_PARAM;
	}
	media_type = trak->Media->handler->handlerType;

	/*only self-contained, non-fragmented sources*/
	if (gf_isom_is_fragmented(src)) return GF_NOT_SUPPORTED;

	timescale = gf_isom_get_media_timescale(src, track);
	if (!timescale) return GF_ISOM_INVALID_MEDIA;
	nb_samples = gf_isom_get_sample_count(src, track);
	nb_descs = gf_isom_get_sample_description_count(src, track);
	if (!nb_descs) return GF_ISOM_INVALID_MEDIA;
	for (i=1; i<=nb_descs; i++) {
		if (!gf_isom_is_self_contained(src, track, i)) return GF_NOT_SUPPORTED;
	}

	/*only tx3g sample entries - cache the default styles while checking*/
	def_styles = (GF_StyleRecord *)gf_malloc(sizeof(GF_StyleRecord) * nb_descs);
	if (!def_styles) return GF_OUT_OF_MEM;
	for (i=1; i<=nb_descs; i++) {
		GF_Tx3gSampleEntryBox *ent = (GF_Tx3gSampleEntryBox *)gf_list_get(trak->Media->information->sampleTable->SampleDescription->child_boxes, i-1);
		if (!ent || (ent->type != GF_ISOM_BOX_TYPE_TX3G)) {
			e = GF_NOT_SUPPORTED;
			goto exit;
		}
		def_styles[i-1] = ent->default_style;
	}

	dst_track = gf_isom_new_track(dst, 0, media_type, timescale);
	if (!dst_track) {
		e = gf_isom_last_error(dst);
		if (!e) e = GF_OUT_OF_MEM;
		goto exit;
	}

	/*copy the sample descriptions in order, preserving description indexes*/
	for (i=1; i<=nb_descs; i++) {
		GF_TextSampleDescriptor *desc = NULL;
		u32 new_idx = 0;
		e = gf_isom_get_text_description(src, track, i, &desc);
		if (e) goto exit;
		e = gf_isom_new_text_description(dst, dst_track, desc, NULL, NULL, &new_idx);
		gf_odf_desc_del((GF_Descriptor *)desc);
		if (e) goto exit;
		if (new_idx != i) {
			e = GF_ISOM_INVALID_MEDIA;
			goto exit;
		}
	}

	memset(&w, 0, sizeof(BakeWriter));
	w.dst = dst;
	w.track = dst_track;

	src_dts = 0;
	for (i=1; i<=nb_samples; i++) {
		GF_ISOSample *samp;
		u32 desc_idx = 0, dur;
		samp = gf_isom_get_sample(src, track, i, &desc_idx);
		if (!samp) {
			e = gf_isom_last_error(src);
			if (!e) e = GF_ISOM_INVALID_MEDIA;
			goto exit;
		}
		dur = gf_isom_get_sample_duration(src, track, i);
		/*only no CTS offsets and continuous DTS starting at 0*/
		if (samp->CTS_Offset || (samp->DTS != src_dts)) {
			gf_isom_sample_del(&samp);
			e = GF_NOT_SUPPORTED;
			goto exit;
		}
		src_dts += dur;
		if (!desc_idx || (desc_idx > nb_descs)) {
			gf_isom_sample_del(&samp);
			e = GF_ISOM_INVALID_MEDIA;
			goto exit;
		}
		e = bake_convert_sample(&w, samp, desc_idx, &def_styles[desc_idx-1], dur);
		gf_isom_sample_del(&samp);
		if (e) goto exit;
		/*never merge samples across source sample boundaries*/
		e = bake_writer_flush(&w);
		if (e) goto exit;
	}

	/*copy the edit list, rescaling movie-timescale values if needed*/
	{
		u32 nb_edits = gf_isom_get_edits_count(src, track);
		u32 src_ts = gf_isom_get_timescale(src);
		u32 dst_ts = gf_isom_get_timescale(dst);
		for (i=1; i<=nb_edits; i++) {
			u64 edit_time, seg_dur, media_time;
			GF_ISOEditType mode;
			e = gf_isom_get_edit(src, track, i, &edit_time, &seg_dur, &media_time, &mode);
			if (e) goto exit;
			if (src_ts && dst_ts && (src_ts != dst_ts)) {
				edit_time = gf_timestamp_rescale(edit_time, src_ts, dst_ts);
				seg_dur = gf_timestamp_rescale(seg_dur, src_ts, dst_ts);
			}
			e = gf_isom_set_edit(dst, dst_track, edit_time, seg_dur, media_time, mode);
			if (e) goto exit;
		}
	}

	*out_track = dst_track;
	e = GF_OK;

exit:
	if (def_styles) gf_free(def_styles);
	if (e && dst_track) {
		/*do not leave a half-written track behind*/
		gf_isom_remove_track(dst, dst_track);
		*out_track = 0;
	}
	return e;
}

#endif /*!GPAC_DISABLE_ISOM && !GPAC_DISABLE_ISOM_WRITE*/
