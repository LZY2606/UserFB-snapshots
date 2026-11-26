/*
 * GPAC - karaoke highlight baking into regular tx3g styles
 */

#include <gpac/internal/isomedia_dev.h>
#include <gpac/constants.h>

#ifndef GPAC_DISABLE_ISOM

#ifndef GPAC_DISABLE_ISOM_WRITE

typedef struct
{
	GF_ISOSample *sample;
	u32 duration;
	u32 description_index;
} BakeChunk;

static void bake_chunk_del(BakeChunk *chunk)
{
	if (!chunk) return;
	if (chunk->sample) gf_isom_sample_del(&chunk->sample);
	gf_free(chunk);
}

static void bake_chunks_del(GF_List *chunks)
{
	BakeChunk *chunk;
	if (!chunks) return;
	while (gf_list_count(chunks)) {
		chunk = (BakeChunk *)gf_list_get(chunks, 0);
		gf_list_rem(chunks, 0);
		bake_chunk_del(chunk);
	}
	gf_list_del(chunks);
}

static GF_Err bake_validate_boxes(u8 *data, u32 data_size, u32 modifier_pos)
{
	GF_BitStream *bs;
	GF_Box *box = NULL;
	GF_Err e = GF_OK;

	if (modifier_pos == data_size) return GF_OK;
	if (modifier_pos > data_size) return GF_NON_COMPLIANT_BITSTREAM;

	bs = gf_bs_new(data, data_size, GF_BITSTREAM_READ);
	if (!bs) return GF_OUT_OF_MEM;
	if (modifier_pos)
		gf_bs_skip_bytes(bs, modifier_pos);

	while (gf_bs_available(bs)) {
		u32 box_type;
		e = gf_isom_box_parse(&box, bs);
		if (e) {
			if (box) gf_isom_box_del(box);
			gf_bs_del(bs);
			return e;
		}
		if (!box) {
			gf_bs_del(bs);
			return GF_NON_COMPLIANT_BITSTREAM;
		}
		box_type = box->type;
		switch (box_type) {
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
			gf_isom_box_del(box);
			gf_bs_del(bs);
			return GF_NOT_SUPPORTED;
		}
		gf_isom_box_del(box);
		box = NULL;
	}
	gf_bs_del(bs);
	return GF_OK;
}

static GF_Err bake_decode_codepoints(const u8 *text, u32 len, u32 *count)
{
	u32 i = 0, n = 0;
	Bool utf16_be = GF_FALSE;
	Bool utf16_le = GF_FALSE;

	*count = 0;
	if (!len) return GF_OK;

	if (len >= 2 && text[0] == 0xFE && text[1] == 0xFF) {
		utf16_be = GF_TRUE;
		i = 2;
	} else if (len >= 2 && text[0] == 0xFF && text[1] == 0xFE) {
		utf16_le = GF_TRUE;
		i = 2;
	}

	if (utf16_be || utf16_le) {
		if (len & 1) return GF_NON_COMPLIANT_BITSTREAM;
		while (i < len) {
			u16 unit;
			if (i + 2 > len) return GF_NON_COMPLIANT_BITSTREAM;
			if (utf16_be) unit = ((u16)text[i] << 8) | text[i + 1];
			else unit = (u16)text[i] | ((u16)text[i + 1] << 8);
			i += 2;

			if ((unit >= 0xD800) && (unit <= 0xDBFF)) {
				u16 low;
				u32 cp;
				if (i + 2 > len) return GF_NON_COMPLIANT_BITSTREAM;
				if (utf16_be) low = ((u16)text[i] << 8) | text[i + 1];
				else low = (u16)text[i] | ((u16)text[i + 1] << 8);
				i += 2;
				if ((low < 0xDC00) || (low > 0xDFFF))
					return GF_NON_COMPLIANT_BITSTREAM;
				cp = 0x10000 + (((u32)(unit - 0xD800) << 10) | (low - 0xDC00));
				if (cp > 0x10FFFF) return GF_NON_COMPLIANT_BITSTREAM;
				n++;
			} else if ((unit >= 0xDC00) && (unit <= 0xDFFF)) {
				return GF_NON_COMPLIANT_BITSTREAM;
			} else {
				n++;
			}
		}
		*count = n;
		return GF_OK;
	}

	/* Accept a leading UTF-8 BOM. It is a formatting marker, not a text offset. */
	if ((len >= 3) && (text[0] == 0xEF) && (text[1] == 0xBB) && (text[2] == 0xBF))
		i = 3;

	while (i < len) {
		u8 c = text[i];
		u32 extra, cp, j;
		u8 min_second = 0x80, max_second = 0xBF;

		if (c < 0x80) {
			i++;
			n++;
			continue;
		}
		if ((c >= 0xC2) && (c <= 0xDF)) {
			extra = 1;
			cp = c & 0x1F;
		} else if ((c >= 0xE0) && (c <= 0xEF)) {
			extra = 2;
			cp = c & 0x0F;
			if (c == 0xE0) min_second = 0xA0;
			if (c == 0xED) max_second = 0x9F;
		} else if ((c >= 0xF0) && (c <= 0xF4)) {
			extra = 3;
			cp = c & 0x07;
			if (c == 0xF0) min_second = 0x90;
			if (c == 0xF4) max_second = 0x8F;
		} else {
			return GF_NON_COMPLIANT_BITSTREAM;
		}

		if (i + extra >= len) return GF_NON_COMPLIANT_BITSTREAM;
		for (j = 1; j <= extra; j++) {
			u8 cont = text[i + j];
			u8 lo = (j == 1) ? min_second : 0x80;
			u8 hi = (j == 1) ? max_second : 0xBF;
			if ((cont < lo) || (cont > hi))
				return GF_NON_COMPLIANT_BITSTREAM;
			cp = (cp << 6) | (cont & 0x3F);
		}
		if ((extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) || cp > 0x10FFFF)
			return GF_NON_COMPLIANT_BITSTREAM;
		i += extra + 1;
		n++;
	}
	*count = n;
	return GF_OK;
}

static GF_Err bake_check_range(u16 start, u16 end, u32 char_count)
{
	if ((u32)start > (u32)end || (u32)end > char_count)
		return GF_NON_COMPLIANT_BITSTREAM;
	return GF_OK;
}

static GF_Err bake_add_time(u32 **times, u32 *count, u32 *alloc, u32 value)
{
	if (*count == *alloc) {
		u32 new_alloc = *alloc ? (*alloc * 2) : 8;
		u32 *new_times = (u32 *)gf_realloc(*times, sizeof(u32) * new_alloc);
		if (!new_times) return GF_OUT_OF_MEM;
		*times = new_times;
		*alloc = new_alloc;
	}
	(*times)[(*count)++] = value;
	return GF_OK;
}

static int bake_u32_cmp(const void *a, const void *b)
{
	u32 x = *(const u32 *)a;
	u32 y = *(const u32 *)b;
	return (x > y) - (x < y);
}

static Bool bake_same_style(const GF_StyleRecord *a, const GF_StyleRecord *b)
{
	return a->fontID == b->fontID
		&& a->style_flags == b->style_flags
		&& a->font_size == b->font_size
		&& a->text_color == b->text_color;
}

static Bool bake_is_default_style(const GF_StyleRecord *a, const GF_StyleRecord *def)
{
	return bake_same_style(a, def);
}

static Bool bake_same_style_span(const GF_StyleRecord *a, const GF_StyleRecord *b, u32 count)
{
	u32 i;
	if (!count) return GF_TRUE;
	if (!a || !b) return GF_FALSE;
	for (i = 0; i < count; i++) {
		if (!bake_same_style(&a[i], &b[i]))
			return GF_FALSE;
	}
	return GF_TRUE;
}

static GF_Err bake_validate_ranges_and_times(GF_TextSample *tx, u32 char_count, u32 duration,
                                             u32 **event_times, u32 *event_count, u32 *event_alloc)
{
	GF_Err e;
	u32 i = 0;
	GF_Box *box;

	if (tx->styles) {
		u32 j;
		for (j = 0; j < tx->styles->entry_count; j++) {
			e = bake_check_range(tx->styles->styles[j].startCharOffset,
			                     tx->styles->styles[j].endCharOffset, char_count);
			if (e) return e;
		}
	}

	e = bake_add_time(event_times, event_count, event_alloc, 0);
	if (e) return e;
	e = bake_add_time(event_times, event_count, event_alloc, duration);
	if (e) return e;

	while ((box = (GF_Box *)gf_list_enum(tx->others, &i))) {
		if (box->type == GF_ISOM_BOX_TYPE_HLIT) {
			GF_TextHighlightBox *h = (GF_TextHighlightBox *)box;
			e = bake_check_range(h->startcharoffset, h->endcharoffset, char_count);
			if (e) return e;
		} else if (box->type == GF_ISOM_BOX_TYPE_HREF) {
			GF_TextHyperTextBox *h = (GF_TextHyperTextBox *)box;
			e = bake_check_range(h->startcharoffset, h->endcharoffset, char_count);
			if (e) return e;
		} else if (box->type == GF_ISOM_BOX_TYPE_KROK) {
			GF_TextKaraokeBox *k = (GF_TextKaraokeBox *)box;
			u32 j, segment_start = k->highlight_starttime;
			if (k->highlight_starttime > duration)
				return GF_NON_COMPLIANT_BITSTREAM;
			e = bake_add_time(event_times, event_count, event_alloc, k->highlight_starttime);
			if (e) return e;
			for (j = 0; j < k->nb_entries; j++) {
				KaraokeRecord *r = &k->records[j];
				if (r->highlight_endtime < segment_start || r->highlight_endtime > duration)
					return GF_NON_COMPLIANT_BITSTREAM;
				e = bake_check_range(r->start_charoffset, r->end_charoffset, char_count);
				if (e) return e;
				e = bake_add_time(event_times, event_count, event_alloc, r->highlight_endtime);
				if (e) return e;
				segment_start = r->highlight_endtime;
			}
		}
	}
	return GF_OK;
}

static void bake_apply_range(u8 *highlighted, u16 start, u16 end)
{
	u32 i;
	for (i = start; i < end; i++)
		highlighted[i] = 1;
}

static void bake_collect_highlight(GF_TextSample *tx, u32 time_start, u32 time_end, u8 *highlighted)
{
	u32 i = 0;
	GF_Box *box;

	while ((box = (GF_Box *)gf_list_enum(tx->others, &i))) {
		if (box->type == GF_ISOM_BOX_TYPE_HLIT) {
			GF_TextHighlightBox *h = (GF_TextHighlightBox *)box;
			bake_apply_range(highlighted, h->startcharoffset, h->endcharoffset);
		} else if (box->type == GF_ISOM_BOX_TYPE_KROK) {
			GF_TextKaraokeBox *k = (GF_TextKaraokeBox *)box;
			u32 j;
			u32 segment_start = k->highlight_starttime;
			for (j = 0; j < k->nb_entries; j++) {
				KaraokeRecord *r = &k->records[j];
				u32 segment_end = r->highlight_endtime;
				/* Event boundaries make every positive interval unambiguous. */
				if ((time_start >= segment_start) && (time_start < segment_end)
					&& (time_end <= segment_end)) {
					bake_apply_range(highlighted, r->start_charoffset, r->end_charoffset);
				}
				segment_start = segment_end;
			}
		}
	}
}

static GF_Err bake_build_output_sample(GF_TextSample *tx, const GF_StyleRecord *effective,
                                       u32 char_count, const GF_StyleRecord *default_style,
                                       GF_ISOSample **out_sample)
{
	GF_TextSample *out_tx;
	GF_ISOSample *iso_sample;
	u32 run_start = 0, cp;
	GF_Err e = GF_OK;
	u32 i = 0;
	GF_Box *box;

	out_tx = gf_isom_new_text_sample();
	if (!out_tx) return GF_OUT_OF_MEM;

	if (tx->len) {
		e = gf_isom_text_add_text(out_tx, tx->text, tx->len);
		if (e) goto exit;
	}

	for (cp = 1; cp <= char_count; cp++) {
		if ((cp < char_count) && bake_same_style(&effective[cp - 1], &effective[cp]))
			continue;

		if (!bake_is_default_style(&effective[run_start], default_style)) {
			GF_StyleRecord rec = effective[run_start];
			rec.startCharOffset = run_start;
			rec.endCharOffset = cp;
			e = gf_isom_text_add_style(out_tx, &rec);
			if (e) goto exit;
		}
		run_start = cp;
	}

	if (tx->box) {
		e = gf_isom_text_set_box(out_tx, tx->box->box.top, tx->box->box.left,
		                         tx->box->box.bottom, tx->box->box.right);
		if (e) goto exit;
	}
	if (tx->wrap) {
		e = gf_isom_text_set_wrap(out_tx, tx->wrap->wrap_flag);
		if (e) goto exit;
	}
	if (tx->is_forced) {
		e = gf_isom_text_set_forced(out_tx, GF_TRUE);
		if (e) goto exit;
	}

	while ((box = (GF_Box *)gf_list_enum(tx->others, &i))) {
		if (box->type != GF_ISOM_BOX_TYPE_HREF) continue;
		{
			GF_TextHyperTextBox *h = (GF_TextHyperTextBox *)box;
			e = gf_isom_text_add_hyperlink(out_tx, h->URL, h->URL_hint,
			                              h->startcharoffset, h->endcharoffset);
			if (e) goto exit;
		}
	}

	iso_sample = gf_isom_text_to_sample(out_tx);
	if (!iso_sample) {
		e = GF_OUT_OF_MEM;
		goto exit;
	}
	*out_sample = iso_sample;

exit:
	gf_isom_delete_text_sample(out_tx);
	return e;
}

static GF_Err bake_process_sample(GF_ISOSample *src_sample, u32 duration,
                                  GF_Tx3gSampleEntryBox *desc, u32 desc_index,
                                  GF_List *chunks)
{
	GF_BitStream *bs = NULL;
	GF_TextSample *tx = NULL;
	u32 text_len = 0, modifier_pos, char_count = 0;
	u32 *event_times = NULL, event_count = 0, event_alloc = 0;
	u32 unique_count = 0, j;
	GF_StyleRecord *last_effective = NULL;
	BakeChunk *previous_chunk = NULL;
	GF_Err e;

	if (!src_sample || !desc) return GF_BAD_PARAM;

	if (!src_sample->dataLength) {
		text_len = 0;
		modifier_pos = 0;
	} else {
		if (src_sample->dataLength < 2)
			return GF_NON_COMPLIANT_BITSTREAM;
		text_len = ((u32)src_sample->data[0] << 8) | src_sample->data[1];
		modifier_pos = 2 + text_len;
		if (modifier_pos > src_sample->dataLength)
			return GF_NON_COMPLIANT_BITSTREAM;
	}

	e = bake_validate_boxes(src_sample->data, src_sample->dataLength, modifier_pos);
	if (e) return e;

	e = bake_decode_codepoints(text_len ? src_sample->data + 2 : NULL, text_len, &char_count);
	if (e) return e;

	bs = gf_bs_new(src_sample->data, src_sample->dataLength, GF_BITSTREAM_READ);
	if (!bs) return GF_OUT_OF_MEM;
	tx = gf_isom_parse_text_sample(bs);
	gf_bs_del(bs);
	if (!tx) return GF_OUT_OF_MEM;
	if (tx->len != text_len) {
		e = GF_NON_COMPLIANT_BITSTREAM;
		goto exit;
	}

	e = bake_validate_ranges_and_times(tx, char_count, duration,
	                                    &event_times, &event_count, &event_alloc);
	if (e) goto exit;

	qsort(event_times, event_count, sizeof(u32), bake_u32_cmp);
	for (j = 0; j < event_count; j++) {
		if (!j || event_times[j] != event_times[unique_count - 1])
			event_times[unique_count++] = event_times[j];
	}
	event_count = unique_count;

	for (j = 0; j + 1 < event_count; j++) {
		u32 time_start = event_times[j];
		u32 time_end = event_times[j + 1];
		u8 *highlighted = NULL;
		GF_StyleRecord *effective = NULL;
		Bool any_highlight = GF_FALSE;
		u32 cp;
		GF_ISOSample *out_sample = NULL;
		BakeChunk *chunk;

		if (time_end == time_start) continue;

		if (char_count) {
			highlighted = (u8 *)gf_malloc(char_count);
			if (!highlighted) {
				e = GF_OUT_OF_MEM;
				goto sample_error;
			}
			memset(highlighted, 0, char_count);
		}
		bake_collect_highlight(tx, time_start, time_end, highlighted);

		for (cp = 0; cp < char_count; cp++) {
			if (highlighted[cp]) {
				any_highlight = GF_TRUE;
				break;
			}
		}
		if (any_highlight && !tx->highlight_color) {
			e = GF_NOT_SUPPORTED;
			goto sample_error;
		}

		if (char_count) {
			u32 r;
			effective = (GF_StyleRecord *)gf_malloc(sizeof(GF_StyleRecord) * char_count);
			if (!effective) {
				e = GF_OUT_OF_MEM;
				goto sample_error;
			}
			for (cp = 0; cp < char_count; cp++) {
				effective[cp] = desc->default_style;
				effective[cp].startCharOffset = 0;
				effective[cp].endCharOffset = 0;
			}
			if (tx->styles) {
				for (r = 0; r < tx->styles->entry_count; r++) {
					GF_StyleRecord *rec = &tx->styles->styles[r];
					for (cp = rec->startCharOffset; cp < rec->endCharOffset; cp++) {
						effective[cp] = *rec;
						effective[cp].startCharOffset = 0;
						effective[cp].endCharOffset = 0;
					}
				}
			}
			if (any_highlight) {
				u32 color = tx->highlight_color->hil_color;
				for (cp = 0; cp < char_count; cp++) {
					if (highlighted[cp])
						effective[cp].text_color = color;
				}
			}
		}

		if (previous_chunk
			&& bake_same_style_span(last_effective, effective, char_count)) {
			previous_chunk->duration += time_end - time_start;
			gf_free(effective);
			gf_free(highlighted);
			continue;
		}

		e = bake_build_output_sample(tx, effective, char_count,
		                             &desc->default_style, &out_sample);
		if (e) goto sample_error;

		chunk = (BakeChunk *)gf_malloc(sizeof(BakeChunk));
		if (!chunk) {
			if (out_sample) gf_isom_sample_del(&out_sample);
			e = GF_OUT_OF_MEM;
			goto sample_error;
		}
		chunk->sample = out_sample;
		chunk->duration = time_end - time_start;
		chunk->description_index = desc_index;
		e = gf_list_add(chunks, chunk);
		if (e) {
			bake_chunk_del(chunk);
			goto sample_error;
		}
		previous_chunk = chunk;

		gf_free(last_effective);
		last_effective = effective;
		effective = NULL;

sample_error:
		gf_free(highlighted);
		if (e) {
			gf_free(effective);
			goto exit;
		}
	}

exit:
	gf_free(last_effective);
	gf_free(event_times);
	if (tx) gf_isom_delete_text_sample(tx);
	return e;
}

GF_EXPORT
GF_Err gf_isom_text_bake_karaoke(GF_ISOFile *src, u32 track, GF_ISOFile *dst, u32 *out_track)
{
	GF_TrackBox *src_trak = NULL, *dst_trak = NULL;
	GF_List *chunks = NULL;
	const char *src_name, *dst_name;
	u32 desc_count, i, sample_count, new_track = 0, track_id;
	u32 media_type, media_timescale, src_movie_ts, dst_movie_ts;
	u64 expected_dts = 0, output_dts = 0;
	u32 last_duration = 0;
	GF_Err e;

	if (out_track) *out_track = 0;
	if (!src || !dst || !out_track || !track || src == dst)
		return GF_BAD_PARAM;

	src_name = gf_isom_get_filename(src);
	dst_name = gf_isom_get_filename(dst);
	if (src_name && dst_name && !strcmp(src_name, dst_name))
		return GF_BAD_PARAM;

	e = gf_isom_can_access_movie(dst, GF_ISOM_OPEN_EDIT);
	if (e) return e;

	src_trak = gf_isom_get_track_box(src, track);
	if (!src_trak || !src_trak->Media || !src_trak->Media->information
		|| !src_trak->Media->information->sampleTable
		|| !src_trak->Media->information->sampleTable->SampleDescription
		|| !src_trak->Media->handler || !src_trak->Media->mediaHeader)
		return GF_BAD_PARAM;

	media_type = src_trak->Media->handler->handlerType;
	if ((media_type != GF_ISOM_MEDIA_TEXT) && (media_type != GF_ISOM_MEDIA_SUBT)
		&& (media_type != GF_ISOM_MEDIA_MPEG_SUBT))
		return GF_NOT_SUPPORTED;

	if (gf_isom_is_track_fragmented(src, gf_isom_get_track_id(src, track)))
		return GF_NOT_SUPPORTED;

	desc_count = gf_list_count(src_trak->Media->information->sampleTable->SampleDescription->child_boxes);
	if (!desc_count) return GF_ISOM_INVALID_MEDIA;
	for (i = 0; i < desc_count; i++) {
		GF_Box *entry = (GF_Box *)gf_list_get(src_trak->Media->information->sampleTable->SampleDescription->child_boxes, i);
		GF_SampleEntryBox *se = (GF_SampleEntryBox *)entry;
		GF_DataEntryURLBox *dref_entry;
		if (!entry || entry->type != GF_ISOM_BOX_TYPE_TX3G)
			return GF_NOT_SUPPORTED;
		if (!src_trak->Media->information->dataInformation
			|| !src_trak->Media->information->dataInformation->dref
			|| !se->dataReferenceIndex)
			return GF_NOT_SUPPORTED;
		dref_entry = (GF_DataEntryURLBox *)gf_list_get(src_trak->Media->information->dataInformation->dref->child_boxes,
		                                               se->dataReferenceIndex - 1);
		if (!dref_entry || !(dref_entry->flags & 1))
			return GF_NOT_SUPPORTED;
	}

	chunks = gf_list_new();
	if (!chunks) return GF_OUT_OF_MEM;

	sample_count = gf_isom_get_sample_count(src, track);
	for (i = 1; i <= sample_count; i++) {
		GF_ISOSample *samp;
		u32 desc_index = 0, duration;
		GF_Tx3gSampleEntryBox *desc;

		samp = gf_isom_get_sample(src, track, i, &desc_index);
		if (!samp) {
			e = gf_isom_last_error(src);
			if (!e) e = GF_ISOM_INVALID_FILE;
			goto fail_prepared;
		}
		duration = gf_isom_get_sample_duration(src, track, i);

		if ((i == 1 && samp->DTS != 0)
			|| (i > 1 && samp->DTS != expected_dts)
			|| samp->CTS_Offset != 0) {
			gf_isom_sample_del(&samp);
			e = GF_NOT_SUPPORTED;
			goto fail_prepared;
		}
		expected_dts = samp->DTS + duration;

		if (!desc_index || desc_index > desc_count) {
			gf_isom_sample_del(&samp);
			e = GF_ISOM_INVALID_MEDIA;
			goto fail_prepared;
		}
		desc = (GF_Tx3gSampleEntryBox *)gf_list_get(src_trak->Media->information->sampleTable->SampleDescription->child_boxes,
		                                            desc_index - 1);
		if (!desc || ((GF_Box *)desc)->type != GF_ISOM_BOX_TYPE_TX3G) {
			gf_isom_sample_del(&samp);
			e = GF_NOT_SUPPORTED;
			goto fail_prepared;
		}

		e = bake_process_sample(samp, duration, desc, desc_index, chunks);
		gf_isom_sample_del(&samp);
		if (e) goto fail_prepared;
	}

	media_timescale = src_trak->Media->mediaHeader->timeScale;
	track_id = 1;
	while (gf_isom_get_track_by_id(dst, track_id))
		track_id++;
	new_track = gf_isom_new_track(dst, track_id, media_type, media_timescale);
	if (!new_track) {
		e = gf_isom_last_error(dst);
		if (!e) e = GF_OUT_OF_MEM;
		goto fail_prepared;
	}

	for (i = 1; i <= desc_count; i++) {
		u32 copied_index = 0;
		e = gf_isom_clone_sample_description(dst, new_track, src, track, i, NULL, NULL, &copied_index);
		if (e) goto fail_track;
		if (copied_index != i) {
			e = GF_ISOM_INVALID_MEDIA;
			goto fail_track;
		}
	}

	for (i = 0; i < gf_list_count(chunks); i++) {
		BakeChunk *chunk = (BakeChunk *)gf_list_get(chunks, i);
		chunk->sample->DTS = output_dts;
		chunk->sample->CTS_Offset = 0;
		e = gf_isom_add_sample(dst, new_track, chunk->description_index, chunk->sample);
		if (e) goto fail_track;
		output_dts += chunk->duration;
		last_duration = chunk->duration;
	}
	if (gf_list_count(chunks)) {
		e = gf_isom_set_last_sample_duration(dst, new_track, last_duration);
		if (e) goto fail_track;
	}

	src_movie_ts = gf_isom_get_timescale(src);
	dst_movie_ts = gf_isom_get_timescale(dst);
	for (i = 1; i <= gf_isom_get_edits_count(src, track); i++) {
		u64 edit_time, edit_duration, media_time;
		GF_ISOEditType edit_mode;
		u64 out_time, out_duration;

		e = gf_isom_get_edit(src, track, i, &edit_time, &edit_duration, &media_time, &edit_mode);
		if (e) goto fail_track;
		if (src_movie_ts && dst_movie_ts && src_movie_ts != dst_movie_ts) {
			out_time = gf_timestamp_rescale(edit_time, src_movie_ts, dst_movie_ts);
			out_duration = gf_timestamp_rescale(edit_duration, src_movie_ts, dst_movie_ts);
		} else {
			out_time = edit_time;
			out_duration = edit_duration;
		}
		e = gf_isom_set_edit(dst, new_track, out_time, out_duration, media_time, edit_mode);
		if (e == GF_EOS) e = GF_OK;
		if (e) goto fail_track;
	}

	dst_trak = gf_isom_get_track_box(dst, new_track);
	if (dst_trak && dst_trak->Header && src_trak->Header) {
		dst_trak->Header->width = src_trak->Header->width;
		dst_trak->Header->height = src_trak->Header->height;
		dst_trak->Header->layer = src_trak->Header->layer;
		dst_trak->Header->alternate_group = src_trak->Header->alternate_group;
	}

	*out_track = new_track;
	bake_chunks_del(chunks);
	return GF_OK;

fail_track:
	if (new_track)
		gf_isom_remove_track(dst, new_track);
fail_prepared:
	*out_track = 0;
	bake_chunks_del(chunks);
	return e;
}

#else

GF_EXPORT
GF_Err gf_isom_text_bake_karaoke(GF_ISOFile *src, u32 track, GF_ISOFile *dst, u32 *out_track)
{
	if (out_track) *out_track = 0;
	(void)src;
	(void)track;
	(void)dst;
	return GF_NOT_SUPPORTED;
}

#endif /* GPAC_DISABLE_ISOM_WRITE */

#endif /* GPAC_DISABLE_ISOM */
