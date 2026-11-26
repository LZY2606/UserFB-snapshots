#include "task_support/check.h"

#include <gpac/internal/isomedia_dev.h>
#include <gpac/isomedia.h>
#include <stdio.h>
#include <string.h>

#define PATH_EVENT      ".task-build/karaoke_event.mp4"
#define PATH_EVENT_OUT  ".task-build/karaoke_event_baked.mp4"
#define PATH_OVERLAP    ".task-build/karaoke_overlap.mp4"
#define PATH_OVERLAP_OUT ".task-build/karaoke_overlap_baked.mp4"
#define PATH_STYLE      ".task-build/karaoke_style.mp4"
#define PATH_STYLE_OUT  ".task-build/karaoke_style_baked.mp4"
#define PATH_UTF16      ".task-build/karaoke_utf16.mp4"
#define PATH_UTF16_OUT  ".task-build/karaoke_utf16_baked.mp4"
#define PATH_MOVIE      ".task-build/karaoke_movie.mp4"
#define PATH_MOVIE_OUT  ".task-build/karaoke_movie_baked.mp4"
#define PATH_BAD        ".task-build/karaoke_bad.mp4"
#define PATH_FAIL_DST   ".task-build/karaoke_fail_dst.mp4"

static void test_sys_init(void)
{
	gf_sys_init(GF_MemTrackerNone, "n");
}

static void test_sys_close(void)
{
	gf_sys_close();
}

static void remove_file(const char *path)
{
	remove(path);
}

static GF_Err add_text_sample(GF_ISOFile *movie, u32 track, u32 desc_idx,
                              GF_TextSample *tx, u64 dts)
{
	GF_ISOSample *s = gf_isom_text_to_sample(tx);
	GF_Err e;
	if (!s) return GF_OUT_OF_MEM;
	s->DTS = dts;
	s->IsRAP = RAP;
	e = gf_isom_add_sample(movie, track, desc_idx, s);
	gf_isom_sample_del(&s);
	return e;
}

static GF_TextSample *read_text(GF_ISOFile *movie, u32 track, u32 number,
                                u32 *desc_idx, GF_ISOSample **iso_out)
{
	GF_ISOSample *samp = gf_isom_get_sample(movie, track, number, desc_idx);
	GF_BitStream *bs;
	GF_TextSample *tx;
	if (!samp) return NULL;
	bs = gf_bs_new(samp->data, samp->dataLength, GF_BITSTREAM_READ);
	if (!bs) {
		gf_isom_sample_del(&samp);
		return NULL;
	}
	tx = gf_isom_parse_text_sample(bs);
	gf_bs_del(bs);
	if (!tx) {
		gf_isom_sample_del(&samp);
		return NULL;
	}
	if (iso_out) *iso_out = samp;
	else gf_isom_sample_del(&samp);
	return tx;
}

static u32 count_other_type(GF_TextSample *tx, u32 type)
{
	u32 i = 0, count = 0;
	GF_Box *b;
	while ((b = (GF_Box *)gf_list_enum(tx->others, &i))) {
		if (b->type == type) count++;
	}
	return count;
}

static void make_desc(GF_TextSampleDescriptor *desc, GF_FontRecord *font,
                      u32 color, u16 font_id, u8 font_size)
{
	memset(desc, 0, sizeof(*desc));
	memset(font, 0, sizeof(*font));
	desc->tag = GF_ODF_TEXT_CFG_TAG;
	font->fontID = font_id;
	font->fontName = (char *)"Serif";
	desc->font_count = 1;
	desc->fonts = font;
	desc->default_style.startCharOffset = 0;
	desc->default_style.endCharOffset = 0;
	desc->default_style.fontID = font_id;
	desc->default_style.style_flags = 0;
	desc->default_style.font_size = font_size;
	desc->default_style.text_color = color;
}

static int event_intervals_example_ab(void)
{
	GF_ISOFile *m, *out;
	GF_TextSampleDescriptor desc;
	GF_FontRecord font;
	GF_TextSample *tx, *parsed;
	GF_ISOSample *samp;
	u32 tr, idx, out_tr = 0, got_idx;
	GF_Err e;
	static const u64 want_dts[4] = {0, 100, 400, 800};
	static const u32 want_dur[4] = {100, 300, 400, 200};
	u32 i;

	test_sys_init();
	remove_file(PATH_EVENT);
	remove_file(PATH_EVENT_OUT);

	m = gf_isom_open(PATH_EVENT, GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(m);
	tr = gf_isom_new_track(m, 1, GF_ISOM_MEDIA_TEXT, 1000);
	TASK_CHECK(tr == 1);
	make_desc(&desc, &font, 0xFFFFFFFF, 1, 18);
	e = gf_isom_new_text_description(m, tr, &desc, NULL, NULL, &idx);
	TASK_CHECK(e == GF_OK && idx == 1);

	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx);
	TASK_CHECK(gf_isom_text_add_text(tx, "AB", 2) == GF_OK);
	TASK_CHECK(gf_isom_text_set_highlight_color(tx, 0xFFFF0000) == GF_OK);
	TASK_CHECK(gf_isom_text_add_karaoke(tx, 100) == GF_OK);
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 400, 0, 1) == GF_OK);
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 800, 1, 2) == GF_OK);
	TASK_CHECK(add_text_sample(m, tr, idx, tx, 0) == GF_OK);
	gf_isom_delete_text_sample(tx);
	TASK_CHECK(gf_isom_set_last_sample_duration(m, tr, 1000) == GF_OK);
	TASK_CHECK(gf_isom_close(m) == GF_OK);

	m = gf_isom_open(PATH_EVENT, GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(m);
	out = gf_isom_open(PATH_EVENT_OUT, GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(out);
	e = gf_isom_text_bake_karaoke(m, 1, out, &out_tr);
	TASK_CHECK(e == GF_OK);
	TASK_CHECK(out_tr == 1);
	TASK_CHECK(gf_isom_close(out) == GF_OK);
	TASK_CHECK(gf_isom_close(m) == GF_OK);

	out = gf_isom_open(PATH_EVENT_OUT, GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(out);
	TASK_CHECK(gf_isom_get_sample_count(out, 1) == 4);
	for (i = 1; i <= 4; i++) {
		u32 styles;
		parsed = read_text(out, 1, i, &got_idx, &samp);
		TASK_CHECK(parsed && samp);
		TASK_CHECK(got_idx == 1);
		TASK_CHECK(samp->DTS == want_dts[i - 1]);
		TASK_CHECK(gf_isom_get_sample_duration(out, 1, i) == want_dur[i - 1]);
		TASK_CHECK(parsed->len == 2 && !memcmp(parsed->text, "AB", 2));
		TASK_CHECK(!parsed->highlight_color);
		TASK_CHECK(count_other_type(parsed, GF_ISOM_BOX_TYPE_KROK) == 0);
		TASK_CHECK(count_other_type(parsed, GF_ISOM_BOX_TYPE_HLIT) == 0);
		styles = parsed->styles ? parsed->styles->entry_count : 0;
		if (i == 1 || i == 4) TASK_CHECK(styles == 0);
		if (i == 2) {
			TASK_CHECK(styles == 1);
			TASK_CHECK(parsed->styles->styles[0].startCharOffset == 0);
			TASK_CHECK(parsed->styles->styles[0].endCharOffset == 1);
			TASK_CHECK(parsed->styles->styles[0].text_color == 0xFFFF0000);
			TASK_CHECK(parsed->styles->styles[0].fontID == 1);
			TASK_CHECK(parsed->styles->styles[0].font_size == 18);
		}
		if (i == 3) {
			TASK_CHECK(styles == 1);
			TASK_CHECK(parsed->styles->styles[0].startCharOffset == 1);
			TASK_CHECK(parsed->styles->styles[0].endCharOffset == 2);
			TASK_CHECK(parsed->styles->styles[0].text_color == 0xFFFF0000);
		}
		gf_isom_sample_del(&samp);
		gf_isom_delete_text_sample(parsed);
	}
	TASK_CHECK(gf_isom_close(out) == GF_OK);
	test_sys_close();
	return 0;
}

static int event_intervals_overlap(void)
{
	GF_ISOFile *m, *out;
	GF_TextSampleDescriptor desc;
	GF_FontRecord font;
	GF_TextSample *tx, *parsed;
	GF_ISOSample *samp;
	u32 tr, idx, out_tr = 0, got_idx;
	GF_Err e;
	static const u64 want_dts[4] = {0, 100, 400, 600};
	static const u32 want_dur[4] = {100, 300, 200, 400};
	static const u16 want_start[4] = {2, 0, 1, 2};
	static const u16 want_end[4] = {4, 4, 4, 4};
	u32 i;

	test_sys_init();
	remove_file(PATH_OVERLAP);
	remove_file(PATH_OVERLAP_OUT);
	m = gf_isom_open(PATH_OVERLAP, GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(m);
	tr = gf_isom_new_track(m, 1, GF_ISOM_MEDIA_TEXT, 1000);
	TASK_CHECK(tr);
	make_desc(&desc, &font, 0xFFFFFFFF, 1, 18);
	TASK_CHECK(gf_isom_new_text_description(m, tr, &desc, NULL, NULL, &idx) == GF_OK);

	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx);
	TASK_CHECK(gf_isom_text_add_text(tx, "ABCD", 4) == GF_OK);
	TASK_CHECK(gf_isom_text_set_highlight_color(tx, 0xFFFF0000) == GF_OK);
	TASK_CHECK(gf_isom_text_add_highlight(tx, 2, 4) == GF_OK);
	TASK_CHECK(gf_isom_text_add_karaoke(tx, 100) == GF_OK);
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 400, 0, 2) == GF_OK);
	TASK_CHECK(gf_isom_text_add_karaoke(tx, 200) == GF_OK);
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 600, 1, 3) == GF_OK);
	TASK_CHECK(add_text_sample(m, tr, idx, tx, 0) == GF_OK);
	gf_isom_delete_text_sample(tx);
	TASK_CHECK(gf_isom_set_last_sample_duration(m, tr, 1000) == GF_OK);
	TASK_CHECK(gf_isom_close(m) == GF_OK);

	m = gf_isom_open(PATH_OVERLAP, GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(m);
	out = gf_isom_open(PATH_OVERLAP_OUT, GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(out);
	e = gf_isom_text_bake_karaoke(m, 1, out, &out_tr);
	TASK_CHECK(e == GF_OK && out_tr == 1);
	TASK_CHECK(gf_isom_close(out) == GF_OK);
	TASK_CHECK(gf_isom_close(m) == GF_OK);

	out = gf_isom_open(PATH_OVERLAP_OUT, GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(out);
	TASK_CHECK(gf_isom_get_sample_count(out, 1) == 4);
	for (i = 1; i <= 4; i++) {
		parsed = read_text(out, 1, i, &got_idx, &samp);
		TASK_CHECK(parsed && samp);
		TASK_CHECK(samp->DTS == want_dts[i - 1]);
		TASK_CHECK(gf_isom_get_sample_duration(out, 1, i) == want_dur[i - 1]);
		TASK_CHECK(parsed->styles && parsed->styles->entry_count == 1);
		TASK_CHECK(parsed->styles->styles[0].startCharOffset == want_start[i - 1]);
		TASK_CHECK(parsed->styles->styles[0].endCharOffset == want_end[i - 1]);
		TASK_CHECK(parsed->styles->styles[0].text_color == 0xFFFF0000);
		gf_isom_sample_del(&samp);
		gf_isom_delete_text_sample(parsed);
	}
	TASK_CHECK(gf_isom_close(out) == GF_OK);
	test_sys_close();
	return 0;
}

static int event_equal_times_empty_ranges(void)
{
	GF_ISOFile *m, *out;
	GF_TextSampleDescriptor desc;
	GF_FontRecord font;
	GF_TextSample *tx, *parsed;
	GF_ISOSample *samp;
	u32 tr, idx, out_tr = 0, got_idx;

	test_sys_init();
	remove_file(".task-build/karaoke_equal.mp4");
	remove_file(".task-build/karaoke_equal_out.mp4");
	m = gf_isom_open(".task-build/karaoke_equal.mp4", GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(m);
	tr = gf_isom_new_track(m, 1, GF_ISOM_MEDIA_TEXT, 1000);
	TASK_CHECK(tr);
	make_desc(&desc, &font, 0xFFFFFFFF, 1, 18);
	TASK_CHECK(gf_isom_new_text_description(m, tr, &desc, NULL, NULL, &idx) == GF_OK);
	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx);
	TASK_CHECK(gf_isom_text_add_text(tx, "AB", 2) == GF_OK);
	TASK_CHECK(gf_isom_text_set_highlight_color(tx, 0xFFFF0000) == GF_OK);
	TASK_CHECK(gf_isom_text_add_karaoke(tx, 10) == GF_OK);
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 10, 0, 1) == GF_OK);
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 50, 1, 1) == GF_OK);
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 50, 1, 2) == GF_OK);
	TASK_CHECK(add_text_sample(m, tr, idx, tx, 0) == GF_OK);
	gf_isom_delete_text_sample(tx);
	TASK_CHECK(gf_isom_set_last_sample_duration(m, tr, 100) == GF_OK);
	TASK_CHECK(gf_isom_close(m) == GF_OK);

	m = gf_isom_open(".task-build/karaoke_equal.mp4", GF_ISOM_OPEN_READ, NULL);
	out = gf_isom_open(".task-build/karaoke_equal_out.mp4", GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(m && out);
	TASK_CHECK(gf_isom_text_bake_karaoke(m, 1, out, &out_tr) == GF_OK);
	TASK_CHECK(out_tr == 1);
	TASK_CHECK(gf_isom_close(out) == GF_OK);
	TASK_CHECK(gf_isom_close(m) == GF_OK);
	out = gf_isom_open(".task-build/karaoke_equal_out.mp4", GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(out);
	TASK_CHECK(gf_isom_get_sample_count(out, 1) == 1);
	parsed = read_text(out, 1, 1, &got_idx, &samp);
	TASK_CHECK(parsed && samp);
	TASK_CHECK(samp->DTS == 0);
	TASK_CHECK(gf_isom_get_sample_duration(out, 1, 1) == 100);
	TASK_CHECK(!parsed->styles || parsed->styles->entry_count == 0);
	gf_isom_sample_del(&samp);
	gf_isom_delete_text_sample(parsed);
	TASK_CHECK(gf_isom_close(out) == GF_OK);
	test_sys_close();
	return 0;
}

static int style_resolution_overlap(void)
{
	GF_ISOFile *m, *out;
	GF_TextSampleDescriptor desc;
	GF_FontRecord font;
	GF_TextSample *tx, *parsed;
	GF_ISOSample *samp;
	GF_StyleRecord rec;
	u32 tr, idx, out_tr = 0, got_idx;

	test_sys_init();
	remove_file(PATH_STYLE);
	remove_file(PATH_STYLE_OUT);
	m = gf_isom_open(PATH_STYLE, GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(m);
	tr = gf_isom_new_track(m, 1, GF_ISOM_MEDIA_TEXT, 1000);
	TASK_CHECK(tr);
	make_desc(&desc, &font, 0xFFFFFFFF, 1, 18);
	TASK_CHECK(gf_isom_new_text_description(m, tr, &desc, NULL, NULL, &idx) == GF_OK);

	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx);
	TASK_CHECK(gf_isom_text_add_text(tx, "ABCDE", 5) == GF_OK);

	memset(&rec, 0, sizeof(rec));
	rec.startCharOffset = 2;
	rec.endCharOffset = 5;
	rec.fontID = 3;
	rec.style_flags = GF_TXT_STYLE_ITALIC;
	rec.font_size = 22;
	rec.text_color = 0xFF00FF00;
	TASK_CHECK(gf_isom_text_add_style(tx, &rec) == GF_OK);

	rec.startCharOffset = 0;
	rec.endCharOffset = 3;
	rec.fontID = 2;
	rec.style_flags = GF_TXT_STYLE_BOLD | GF_TXT_STYLE_UNDERLINED;
	rec.font_size = 20;
	rec.text_color = 0xFF0000FF;
	TASK_CHECK(gf_isom_text_add_style(tx, &rec) == GF_OK);

	TASK_CHECK(gf_isom_text_add_highlight(tx, 1, 4) == GF_OK);
	TASK_CHECK(gf_isom_text_set_highlight_color(tx, 0xFFFF0000) == GF_OK);
	TASK_CHECK(add_text_sample(m, tr, idx, tx, 0) == GF_OK);
	gf_isom_delete_text_sample(tx);
	TASK_CHECK(gf_isom_set_last_sample_duration(m, tr, 1000) == GF_OK);
	TASK_CHECK(gf_isom_close(m) == GF_OK);

	m = gf_isom_open(PATH_STYLE, GF_ISOM_OPEN_READ, NULL);
	out = gf_isom_open(PATH_STYLE_OUT, GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(m && out);
	TASK_CHECK(gf_isom_text_bake_karaoke(m, 1, out, &out_tr) == GF_OK);
	TASK_CHECK(gf_isom_close(out) == GF_OK);
	TASK_CHECK(gf_isom_close(m) == GF_OK);

	out = gf_isom_open(PATH_STYLE_OUT, GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(out);
	TASK_CHECK(gf_isom_get_sample_count(out, 1) == 1);
	parsed = read_text(out, 1, 1, &got_idx, &samp);
	TASK_CHECK(parsed && samp && parsed->styles);
	TASK_CHECK(parsed->styles->entry_count == 4);
	rec = parsed->styles->styles[0];
	TASK_CHECK(rec.startCharOffset == 0 && rec.endCharOffset == 1);
	TASK_CHECK(rec.fontID == 2 && rec.font_size == 20);
	TASK_CHECK(rec.style_flags == (GF_TXT_STYLE_BOLD | GF_TXT_STYLE_UNDERLINED));
	TASK_CHECK(rec.text_color == 0xFF0000FF);
	rec = parsed->styles->styles[1];
	TASK_CHECK(rec.startCharOffset == 1 && rec.endCharOffset == 3);
	TASK_CHECK(rec.fontID == 2 && rec.font_size == 20);
	TASK_CHECK(rec.style_flags == (GF_TXT_STYLE_BOLD | GF_TXT_STYLE_UNDERLINED));
	TASK_CHECK(rec.text_color == 0xFFFF0000);
	rec = parsed->styles->styles[2];
	TASK_CHECK(rec.startCharOffset == 3 && rec.endCharOffset == 4);
	TASK_CHECK(rec.fontID == 3 && rec.font_size == 22);
	TASK_CHECK(rec.style_flags == GF_TXT_STYLE_ITALIC);
	TASK_CHECK(rec.text_color == 0xFFFF0000);
	rec = parsed->styles->styles[3];
	TASK_CHECK(rec.startCharOffset == 4 && rec.endCharOffset == 5);
	TASK_CHECK(rec.fontID == 3 && rec.font_size == 22);
	TASK_CHECK(rec.style_flags == GF_TXT_STYLE_ITALIC);
	TASK_CHECK(rec.text_color == 0xFF00FF00);
	gf_isom_sample_del(&samp);
	gf_isom_delete_text_sample(parsed);
	TASK_CHECK(gf_isom_close(out) == GF_OK);
	test_sys_close();
	return 0;
}

static int style_utf16_one(int little_endian)
{
	u8 utf16_ab[] = {0xFE, 0xFF, 0x00, 'A', 0x00, 'B'};
	GF_ISOFile *m, *out;
	GF_TextSampleDescriptor desc;
	GF_FontRecord font;
	GF_TextSample *tx, *parsed;
	GF_ISOSample *samp;
	u32 tr, idx, out_tr = 0, got_idx;

	if (little_endian) {
		utf16_ab[0] = 0xFF;
		utf16_ab[1] = 0xFE;
		utf16_ab[2] = 'A';
		utf16_ab[3] = 0x00;
		utf16_ab[4] = 'B';
		utf16_ab[5] = 0x00;
	}

	remove_file(PATH_UTF16);
	remove_file(PATH_UTF16_OUT);
	m = gf_isom_open(PATH_UTF16, GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(m);
	tr = gf_isom_new_track(m, 1, GF_ISOM_MEDIA_TEXT, 1000);
	TASK_CHECK(tr);
	make_desc(&desc, &font, 0xFFFFFFFF, 1, 18);
	TASK_CHECK(gf_isom_new_text_description(m, tr, &desc, NULL, NULL, &idx) == GF_OK);
	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx);
	TASK_CHECK(gf_isom_text_add_text(tx, (char *)utf16_ab, sizeof(utf16_ab)) == GF_OK);
	TASK_CHECK(gf_isom_text_set_highlight_color(tx, 0xFFFF0000) == GF_OK);
	TASK_CHECK(gf_isom_text_add_karaoke(tx, 0) == GF_OK);
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 500, 0, 1) == GF_OK);
	TASK_CHECK(add_text_sample(m, tr, idx, tx, 0) == GF_OK);
	gf_isom_delete_text_sample(tx);
	TASK_CHECK(gf_isom_set_last_sample_duration(m, tr, 1000) == GF_OK);
	TASK_CHECK(gf_isom_close(m) == GF_OK);

	m = gf_isom_open(PATH_UTF16, GF_ISOM_OPEN_READ, NULL);
	out = gf_isom_open(PATH_UTF16_OUT, GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(m && out);
	TASK_CHECK(gf_isom_text_bake_karaoke(m, 1, out, &out_tr) == GF_OK);
	TASK_CHECK(gf_isom_close(out) == GF_OK);
	TASK_CHECK(gf_isom_close(m) == GF_OK);
	out = gf_isom_open(PATH_UTF16_OUT, GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(out);
	TASK_CHECK(gf_isom_get_sample_count(out, 1) == 2);
	parsed = read_text(out, 1, 1, &got_idx, &samp);
	TASK_CHECK(parsed && samp && parsed->styles && parsed->styles->entry_count == 1);
	TASK_CHECK(parsed->styles->styles[0].startCharOffset == 0);
	TASK_CHECK(parsed->styles->styles[0].endCharOffset == 1);
	gf_isom_sample_del(&samp);
	gf_isom_delete_text_sample(parsed);
	parsed = read_text(out, 1, 2, &got_idx, &samp);
	TASK_CHECK(parsed && samp && (!parsed->styles || !parsed->styles->entry_count));
	gf_isom_sample_del(&samp);
	gf_isom_delete_text_sample(parsed);
	TASK_CHECK(gf_isom_close(out) == GF_OK);
	return 0;
}

static int style_resolution_utf16_bom(void)
{
	int rc;
	test_sys_init();
	rc = style_utf16_one(0);
	if (!rc) rc = style_utf16_one(1);
	test_sys_close();
	return rc;
}

static int movie_roundtrip_descriptions_edits_clear(void)
{
	GF_ISOFile *m, *out, *check;
	GF_TextSampleDescriptor d1, d2, *rd = NULL;
	GF_FontRecord f1, f2;
	GF_TextSample *tx, *parsed;
	GF_ISOSample *samp;
	u32 tr, idx1, idx2, out_tr = 0, got_idx;
	u64 src_duration, dst_duration;
	u32 src_edits, dst_edits;

	test_sys_init();
	remove_file(PATH_MOVIE);
	remove_file(PATH_MOVIE_OUT);
	m = gf_isom_open(PATH_MOVIE, GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(m);
	tr = gf_isom_new_track(m, 1, GF_ISOM_MEDIA_TEXT, 1000);
	TASK_CHECK(tr);
	make_desc(&d1, &f1, 0xFFFFFFFF, 1, 18);
	TASK_CHECK(gf_isom_new_text_description(m, tr, &d1, NULL, NULL, &idx1) == GF_OK);

	memset(&d2, 0, sizeof(d2));
	memset(&f2, 0, sizeof(f2));
	d2.tag = GF_ODF_TEXT_CFG_TAG;
	f2.fontID = 2;
	f2.fontName = (char *)"Sans";
	d2.font_count = 1;
	d2.fonts = &f2;
	d2.default_style.fontID = 2;
	d2.default_style.font_size = 24;
	d2.default_style.text_color = 0xFFFFFF00;
	TASK_CHECK(gf_isom_new_text_description(m, tr, &d2, NULL, NULL, &idx2) == GF_OK);
	TASK_CHECK(idx2 == 2);

	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx);
	TASK_CHECK(gf_isom_text_add_text(tx, "AB", 2) == GF_OK);
	TASK_CHECK(gf_isom_text_set_highlight_color(tx, 0xFFFF0000) == GF_OK);
	TASK_CHECK(gf_isom_text_add_karaoke(tx, 100) == GF_OK);
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 400, 0, 1) == GF_OK);
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 800, 1, 2) == GF_OK);
	TASK_CHECK(gf_isom_text_set_box(tx, 1, 2, 3, 4) == GF_OK);
	TASK_CHECK(gf_isom_text_set_wrap(tx, 1) == GF_OK);
	TASK_CHECK(gf_isom_text_set_forced(tx, GF_TRUE) == GF_OK);
	TASK_CHECK(gf_isom_text_add_hyperlink(tx, "http://example.test", "hint", 0, 2) == GF_OK);
	TASK_CHECK(add_text_sample(m, tr, idx1, tx, 0) == GF_OK);
	gf_isom_delete_text_sample(tx);

	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx);
	TASK_CHECK(gf_isom_text_add_text(tx, "XY", 2) == GF_OK);
	TASK_CHECK(gf_isom_text_add_highlight(tx, 0, 2) == GF_OK);
	TASK_CHECK(gf_isom_text_set_highlight_color(tx, 0xFF0000FF) == GF_OK);
	TASK_CHECK(add_text_sample(m, tr, idx2, tx, 1000) == GF_OK);
	gf_isom_delete_text_sample(tx);

	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx);
	/* Empty text is a clear-screen sample. */
	TASK_CHECK(add_text_sample(m, tr, idx1, tx, 1500) == GF_OK);
	gf_isom_delete_text_sample(tx);
	TASK_CHECK(gf_isom_set_last_sample_duration(m, tr, 250) == GF_OK);

	TASK_CHECK(gf_isom_set_edit(m, tr, 0, 600, 100, GF_ISOM_EDIT_NORMAL) == GF_OK);
	TASK_CHECK(gf_isom_close(m) == GF_OK);

	m = gf_isom_open(PATH_MOVIE, GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(m);
	src_duration = gf_isom_get_duration(m);
	src_edits = gf_isom_get_edits_count(m, 1);
	TASK_CHECK(src_edits == 1);

	out = gf_isom_open(PATH_MOVIE_OUT, GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(out);
	TASK_CHECK(gf_isom_text_bake_karaoke(m, 1, out, &out_tr) == GF_OK);
	TASK_CHECK(out_tr == 1);
	TASK_CHECK(gf_isom_get_track_count(out) == 1);
	TASK_CHECK(gf_isom_close(out) == GF_OK);
	TASK_CHECK(gf_isom_close(m) == GF_OK);

	check = gf_isom_open(PATH_MOVIE_OUT, GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(check);
	TASK_CHECK(gf_isom_get_media_timescale(check, 1) == 1000);
	TASK_CHECK(gf_isom_get_media_duration(check, 1) == 1750);
	dst_duration = gf_isom_get_duration(check);
	TASK_CHECK(dst_duration == src_duration);
	TASK_CHECK(gf_isom_get_edits_count(check, 1) == src_edits);
	{
		u64 et, ed, mt, et2, ed2, mt2;
		GF_ISOEditType mode, mode2;
		m = gf_isom_open(PATH_MOVIE, GF_ISOM_OPEN_READ, NULL);
		TASK_CHECK(m);
		TASK_CHECK(gf_isom_get_edit(m, 1, 1, &et, &ed, &mt, &mode) == GF_OK);
		TASK_CHECK(gf_isom_get_edit(check, 1, 1, &et2, &ed2, &mt2, &mode2) == GF_OK);
		TASK_CHECK(et == et2 && ed == ed2 && mt == mt2 && mode == mode2);
		TASK_CHECK(gf_isom_close(m) == GF_OK);
	}

	TASK_CHECK(gf_isom_get_sample_count(check, 1) == 6);
	parsed = read_text(check, 1, 1, &got_idx, &samp);
	TASK_CHECK(parsed && samp && got_idx == 1);
	TASK_CHECK(parsed->box && parsed->box->box.top == 1 && parsed->box->box.left == 2);
	TASK_CHECK(parsed->wrap && parsed->wrap->wrap_flag == 1);
	TASK_CHECK(parsed->is_forced);
	TASK_CHECK(count_other_type(parsed, GF_ISOM_BOX_TYPE_HREF) == 1);
	TASK_CHECK(count_other_type(parsed, GF_ISOM_BOX_TYPE_KROK) == 0);
	TASK_CHECK(!parsed->highlight_color);
	gf_isom_sample_del(&samp);
	gf_isom_delete_text_sample(parsed);

	parsed = read_text(check, 1, 4, &got_idx, &samp);
	TASK_CHECK(parsed && got_idx == 1);
	TASK_CHECK(parsed->box && parsed->wrap && parsed->is_forced);
	TASK_CHECK(count_other_type(parsed, GF_ISOM_BOX_TYPE_HREF) == 1);
	gf_isom_sample_del(&samp);
	gf_isom_delete_text_sample(parsed);

	parsed = read_text(check, 1, 5, &got_idx, &samp);
	TASK_CHECK(parsed && got_idx == 2 && parsed->styles && parsed->styles->entry_count == 1);
	TASK_CHECK(parsed->styles->styles[0].startCharOffset == 0);
	TASK_CHECK(parsed->styles->styles[0].endCharOffset == 2);
	TASK_CHECK(parsed->styles->styles[0].text_color == 0xFF0000FF);
	gf_isom_sample_del(&samp);
	gf_isom_delete_text_sample(parsed);

	parsed = read_text(check, 1, 6, &got_idx, &samp);
	TASK_CHECK(parsed && got_idx == 1 && parsed->len == 0);
	TASK_CHECK(!parsed->styles || parsed->styles->entry_count == 0);
	gf_isom_sample_del(&samp);
	gf_isom_delete_text_sample(parsed);

	TASK_CHECK(gf_isom_get_text_description(check, 1, 2, &rd) == GF_OK && rd);
	TASK_CHECK(rd->default_style.fontID == 2 && rd->default_style.font_size == 24);
	TASK_CHECK(rd->default_style.text_color == 0xFFFFFF00);
	gf_odf_desc_del((GF_Descriptor *)rd);
	TASK_CHECK(gf_isom_close(check) == GF_OK);
	test_sys_close();
	return 0;
}

static int movie_roundtrip_failure_reuse(void)
{
	GF_ISOFile *bad, *dst, *check;
	GF_TextSampleDescriptor desc;
	GF_FontRecord font;
	GF_TextSample *tx, *parsed;
	GF_ISOSample *samp;
	u32 tr, idx, out_tr = 1234, got_idx;
	u32 tracks_before;

	test_sys_init();
	remove_file(PATH_BAD);
	remove_file(PATH_FAIL_DST);

	bad = gf_isom_open(PATH_BAD, GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(bad);
	tr = gf_isom_new_track(bad, 1, GF_ISOM_MEDIA_TEXT, 1000);
	TASK_CHECK(tr);
	make_desc(&desc, &font, 0xFFFFFFFF, 1, 18);
	TASK_CHECK(gf_isom_new_text_description(bad, tr, &desc, NULL, NULL, &idx) == GF_OK);
	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx);
	TASK_CHECK(gf_isom_text_add_text(tx, "A", 1) == GF_OK);
	TASK_CHECK(gf_isom_text_add_highlight(tx, 0, 1) == GF_OK);
	/* No hclr: baking a non-empty visible highlight must fail. */
	TASK_CHECK(add_text_sample(bad, tr, idx, tx, 0) == GF_OK);
	gf_isom_delete_text_sample(tx);
	TASK_CHECK(gf_isom_set_last_sample_duration(bad, tr, 1000) == GF_OK);
	TASK_CHECK(gf_isom_close(bad) == GF_OK);

	dst = gf_isom_open(PATH_FAIL_DST, GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(dst);
	tr = gf_isom_new_track(dst, 7, GF_ISOM_MEDIA_TEXT, 1000);
	TASK_CHECK(tr == 1);
	TASK_CHECK(gf_isom_new_text_description(dst, tr, &desc, NULL, NULL, &idx) == GF_OK);
	tx = gf_isom_new_text_sample();
	TASK_CHECK(gf_isom_text_add_text(tx, "keep", 4) == GF_OK);
	TASK_CHECK(add_text_sample(dst, tr, idx, tx, 0) == GF_OK);
	gf_isom_delete_text_sample(tx);
	TASK_CHECK(gf_isom_set_last_sample_duration(dst, tr, 500) == GF_OK);

	bad = gf_isom_open(PATH_BAD, GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(bad);
	tracks_before = gf_isom_get_track_count(dst);
	TASK_CHECK(tracks_before == 1);

	TASK_CHECK(gf_isom_text_bake_karaoke(dst, 1, dst, &out_tr) == GF_BAD_PARAM);
	TASK_CHECK(out_tr == 0);
	out_tr = 1234;
	TASK_CHECK(gf_isom_text_bake_karaoke(bad, 1, dst, &out_tr) == GF_NOT_SUPPORTED);
	TASK_CHECK(out_tr == 0);
	TASK_CHECK(gf_isom_get_track_count(dst) == tracks_before);

	/* Existing destination track remains writable after the failed conversion. */
	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx);
	TASK_CHECK(gf_isom_text_add_text(tx, "more", 4) == GF_OK);
	TASK_CHECK(add_text_sample(dst, 1, 1, tx, 500) == GF_OK);
	gf_isom_delete_text_sample(tx);
	TASK_CHECK(gf_isom_set_last_sample_duration(dst, 1, 700) == GF_OK);
	TASK_CHECK(gf_isom_close(dst) == GF_OK);

	/* Source remains readable after the failed conversion. */
	TASK_CHECK(gf_isom_get_sample_count(bad, 1) == 1);
	parsed = read_text(bad, 1, 1, &got_idx, &samp);
	TASK_CHECK(parsed && !memcmp(parsed->text, "A", 1));
	gf_isom_sample_del(&samp);
	gf_isom_delete_text_sample(parsed);
	TASK_CHECK(gf_isom_close(bad) == GF_OK);

	check = gf_isom_open(PATH_FAIL_DST, GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(check);
	TASK_CHECK(gf_isom_get_track_count(check) == 1);
	TASK_CHECK(gf_isom_get_sample_count(check, 1) == 2);
	parsed = read_text(check, 1, 2, &got_idx, &samp);
	TASK_CHECK(parsed && parsed->len == 4 && !memcmp(parsed->text, "more", 4));
	TASK_CHECK(gf_isom_get_sample_duration(check, 1, 2) == 700);
	gf_isom_sample_del(&samp);
	gf_isom_delete_text_sample(parsed);
	TASK_CHECK(gf_isom_close(check) == GF_OK);
	test_sys_close();
	return 0;
}

static int movie_roundtrip_bad_ranges(void)
{
	static const struct {
		const char *name;
		u16 ks, ke, rs, re;
		u32 dur;
		Bool bad_time;
	} cases[] = {
		{ "char_end", 0, 100, 0, 9, 1000, 0 },
		{ "char_reverse", 0, 100, 2, 1, 1000, 0 },
		{ "time_reverse", 300, 100, 0, 1, 1000, 1 },
		{ "time_overrun", 0, 1200, 0, 1, 1000, 1 },
	};
	u32 ci;

	for (ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
		char path[128], outpath[128];
		GF_ISOFile *m, *dst;
		GF_TextSampleDescriptor desc;
		GF_FontRecord font;
		GF_TextSample *tx;
		u32 tr, idx, out_tr = 777;

		test_sys_init();
		snprintf(path, sizeof(path), ".task-build/bad_%u.mp4", ci);
		snprintf(outpath, sizeof(outpath), ".task-build/bad_%u_out.mp4", ci);
		remove_file(path);
		remove_file(outpath);
		m = gf_isom_open(path, GF_ISOM_WRITE_EDIT, NULL);
		TASK_CHECK(m);
		tr = gf_isom_new_track(m, 1, GF_ISOM_MEDIA_TEXT, 1000);
		TASK_CHECK(tr);
		make_desc(&desc, &font, 0xFFFFFFFF, 1, 18);
		TASK_CHECK(gf_isom_new_text_description(m, tr, &desc, NULL, NULL, &idx) == GF_OK);
		tx = gf_isom_new_text_sample();
		TASK_CHECK(tx);
		TASK_CHECK(gf_isom_text_add_text(tx, "ABCDE", 5) == GF_OK);
		TASK_CHECK(gf_isom_text_set_highlight_color(tx, 0xFFFF0000) == GF_OK);
		TASK_CHECK(gf_isom_text_add_karaoke(tx, cases[ci].ks) == GF_OK);
		TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, cases[ci].ke, cases[ci].rs, cases[ci].re) == GF_OK);
		TASK_CHECK(add_text_sample(m, tr, idx, tx, 0) == GF_OK);
		gf_isom_delete_text_sample(tx);
		TASK_CHECK(gf_isom_set_last_sample_duration(m, tr, cases[ci].dur) == GF_OK);
		TASK_CHECK(gf_isom_close(m) == GF_OK);

		m = gf_isom_open(path, GF_ISOM_OPEN_READ, NULL);
		dst = gf_isom_open(outpath, GF_ISOM_WRITE_EDIT, NULL);
		TASK_CHECK(m && dst);
		TASK_CHECK(gf_isom_text_bake_karaoke(m, 1, dst, &out_tr) != GF_OK);
		TASK_CHECK(out_tr == 0);
		TASK_CHECK(gf_isom_get_track_count(dst) == 0);
		TASK_CHECK(gf_isom_close(dst) == GF_OK);
		TASK_CHECK(gf_isom_close(m) == GF_OK);
		test_sys_close();
	}
	return 0;
}

static int movie_roundtrip_bad_encoding_and_boxes(void)
{
	u32 kind;
	for (kind = 0; kind < 3; kind++) {
		char path[128], outpath[128];
		GF_ISOFile *m, *dst;
		GF_TextSampleDescriptor desc;
		GF_FontRecord font;
		GF_TextSample *tx;
		u32 tr, idx, out_tr = 777;
		GF_Err e;

		test_sys_init();
		snprintf(path, sizeof(path), ".task-build/encoding_%u.mp4", kind);
		snprintf(outpath, sizeof(outpath), ".task-build/encoding_%u_out.mp4", kind);
		remove_file(path);
		remove_file(outpath);

		m = gf_isom_open(path, GF_ISOM_WRITE_EDIT, NULL);
		TASK_CHECK(m);
		tr = gf_isom_new_track(m, 1, GF_ISOM_MEDIA_TEXT, 1000);
		TASK_CHECK(tr);
		make_desc(&desc, &font, 0xFFFFFFFF, 1, 18);
		TASK_CHECK(gf_isom_new_text_description(m, tr, &desc, NULL, NULL, &idx) == GF_OK);
		tx = gf_isom_new_text_sample();
		TASK_CHECK(tx);
		if (kind == 0) {
			TASK_CHECK(gf_isom_text_add_text(tx, "AB\xff", 3) == GF_OK);
			TASK_CHECK(gf_isom_text_set_highlight_color(tx, 0xFFFF0000) == GF_OK);
			TASK_CHECK(gf_isom_text_add_karaoke(tx, 0) == GF_OK);
			TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 500, 0, 1) == GF_OK);
		} else {
			TASK_CHECK(gf_isom_text_add_text(tx, "AB", 2) == GF_OK);
			TASK_CHECK(gf_isom_text_set_highlight_color(tx, 0xFFFF0000) == GF_OK);
			if (kind == 1)
				TASK_CHECK(gf_isom_text_add_blink(tx, 0, 1) == GF_OK);
			else
				TASK_CHECK(gf_isom_text_set_scroll_delay(tx, 10) == GF_OK);
		}
		TASK_CHECK(add_text_sample(m, tr, idx, tx, 0) == GF_OK);
		gf_isom_delete_text_sample(tx);
		TASK_CHECK(gf_isom_set_last_sample_duration(m, tr, 1000) == GF_OK);
		TASK_CHECK(gf_isom_close(m) == GF_OK);

		m = gf_isom_open(path, GF_ISOM_OPEN_READ, NULL);
		dst = gf_isom_open(outpath, GF_ISOM_WRITE_EDIT, NULL);
		TASK_CHECK(m && dst);
		e = gf_isom_text_bake_karaoke(m, 1, dst, &out_tr);
		if (kind == 0) TASK_CHECK(e == GF_NON_COMPLIANT_BITSTREAM);
		else TASK_CHECK(e == GF_NOT_SUPPORTED);
		TASK_CHECK(out_tr == 0);
		TASK_CHECK(gf_isom_get_track_count(dst) == 0);
		TASK_CHECK(gf_isom_close(dst) == GF_OK);
		TASK_CHECK(gf_isom_close(m) == GF_OK);
		test_sys_close();
	}
	return 0;
}

const struct task_case task_cases[] = {
	{ "event_intervals", "example_ab", event_intervals_example_ab },
	{ "event_intervals", "overlap_union", event_intervals_overlap },
	{ "event_intervals", "equal_times_empty_ranges", event_equal_times_empty_ranges },
	{ "style_resolution", "overlapping_records", style_resolution_overlap },
	{ "style_resolution", "utf16_bom", style_resolution_utf16_bom },
	{ "movie_roundtrip", "descriptions_edits_clear", movie_roundtrip_descriptions_edits_clear },
	{ "movie_roundtrip", "failure_and_reuse", movie_roundtrip_failure_reuse },
	{ "movie_roundtrip", "bad_ranges_and_times", movie_roundtrip_bad_ranges },
	{ "movie_roundtrip", "bad_encoding_and_boxes", movie_roundtrip_bad_encoding_and_boxes },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
