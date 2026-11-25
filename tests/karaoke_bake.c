/*
 * Regression tests for gf_isom_text_bake_karaoke - baked karaoke tx3g tracks.
 */
#include "../task_support/check.h"
#include <gpac/internal/isomedia_dev.h>
#include <stdio.h>
#include <string.h>

static int sys_ready = 0;
static void ensure_sys(void)
{
	if (!sys_ready) {
		gf_sys_init(GF_MemTrackerNone, "n");
		sys_ready = 1;
	}
}

/* ---------- source file builder ---------- */

typedef struct {
	GF_ISOFile *m;
	u32 tr;
	u64 dts;
	GF_FontRecord font;
	GF_TextSampleDescriptor desc;
} SrcW;

static int src_open(SrcW *b, const char *path, u32 timescale)
{
	memset(b, 0, sizeof(*b));
	b->m = gf_isom_open(path, GF_ISOM_WRITE_EDIT, NULL);
	if (!b->m) return 1;
	b->tr = gf_isom_new_track(b->m, 1, GF_ISOM_MEDIA_TEXT, timescale);
	if (!b->tr) return 1;
	return 0;
}

static int src_add_desc(SrcW *b, u32 color, u8 size, u8 flags, u32 *idx)
{
	memset(&b->desc, 0, sizeof(b->desc));
	b->desc.tag = GF_ODF_TEXT_CFG_TAG;
	b->font.fontID = 1;
	b->font.fontName = "Serif";
	b->desc.font_count = 1;
	b->desc.fonts = &b->font;
	b->desc.default_style.fontID = 1;
	b->desc.default_style.font_size = size;
	b->desc.default_style.style_flags = flags;
	b->desc.default_style.text_color = color;
	return gf_isom_new_text_description(b->m, b->tr, &b->desc, NULL, NULL, idx) != GF_OK;
}

static int src_sample(SrcW *b, GF_TextSample *tx, u32 desc_idx, u32 dur)
{
	GF_ISOSample *s = gf_isom_text_to_sample(tx);
	if (!s) return 1;
	s->DTS = b->dts;
	s->IsRAP = RAP;
	if (gf_isom_add_sample(b->m, b->tr, desc_idx, s)) {
		gf_isom_sample_del(&s);
		return 1;
	}
	if (gf_isom_set_last_sample_duration(b->m, b->tr, dur)) {
		gf_isom_sample_del(&s);
		return 1;
	}
	b->dts += dur;
	gf_isom_sample_del(&s);
	return 0;
}

/* ---------- readers ---------- */

static GF_TextSample *read_sample(GF_ISOFile *m, u32 tr, u32 num, u32 *desc_idx, u64 *dts, u32 *dur)
{
	GF_ISOSample *s = gf_isom_get_sample(m, tr, num, desc_idx);
	GF_TextSample *tx;
	GF_BitStream *bs;
	if (!s) return NULL;
	if (dts) *dts = s->DTS;
	if (dur) *dur = gf_isom_get_sample_duration(m, tr, num);
	bs = gf_bs_new(s->data, s->dataLength, GF_BITSTREAM_READ);
	tx = gf_isom_parse_text_sample(bs);
	gf_bs_del(bs);
	gf_isom_sample_del(&s);
	return tx;
}

static u32 count_type(GF_TextSample *tx, u32 type)
{
	u32 n = 0, i = 0;
	GF_Box *b;
	while ((b = (GF_Box*)gf_list_enum(tx->others, &i))) {
		if (b->type==type) n++;
	}
	return n;
}

/* ========== event_intervals ========== */

static int case_basic_split(void)
{
	SrcW b;
	GF_TextSample *tx, *p;
	GF_ISOFile *src, *dst;
	GF_Err e;
	u32 idx = 0, out_tr = 0, i, di;
	u64 dts;
	u32 dur;
	static const u64 exp_dts[4] = {0, 100, 400, 800};
	static const u32 exp_dur[4] = {100, 300, 400, 200};
	static const u32 exp_hi_start[4] = {0, 0, 1, 0};
	static const u32 exp_hi_end[4] = {0, 1, 2, 0};

	ensure_sys();
	TASK_CHECK(src_open(&b, ".task-build/kb_basic_src.mp4", 1000)==0);
	TASK_CHECK(src_add_desc(&b, 0xFFFFFFFF, 18, 0, &idx)==0);
	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx!=NULL);
	TASK_CHECK(gf_isom_text_add_text(tx, "AB", 2)==GF_OK);
	TASK_CHECK(gf_isom_text_set_highlight_color(tx, 0xFFFF0000)==GF_OK);
	TASK_CHECK(gf_isom_text_add_karaoke(tx, 100)==GF_OK);
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 400, 0, 1)==GF_OK);
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 800, 1, 2)==GF_OK);
	TASK_CHECK(src_sample(&b, tx, idx, 1000)==0);
	gf_isom_delete_text_sample(tx);
	TASK_CHECK(gf_isom_close(b.m)==GF_OK);

	src = gf_isom_open(".task-build/kb_basic_src.mp4", GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(src!=NULL);
	dst = gf_isom_open(".task-build/kb_basic_dst.mp4", GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(dst!=NULL);
	e = gf_isom_text_bake_karaoke(src, 1, dst, &out_tr);
	TASK_CHECK(e==GF_OK);
	TASK_CHECK(out_tr==1);
	TASK_CHECK(gf_isom_close(src)==GF_OK);
	TASK_CHECK(gf_isom_close(dst)==GF_OK);

	dst = gf_isom_open(".task-build/kb_basic_dst.mp4", GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(dst!=NULL);
	TASK_CHECK(gf_isom_get_sample_count(dst, 1)==4);
	TASK_CHECK(gf_isom_get_media_timescale(dst, 1)==1000);
	TASK_CHECK(gf_isom_get_media_duration(dst, 1)==1000);
	for (i=1; i<=4; i++) {
		p = read_sample(dst, 1, i, &di, &dts, &dur);
		TASK_CHECK(p!=NULL);
		TASK_CHECK(dts==exp_dts[i-1]);
		TASK_CHECK(dur==exp_dur[i-1]);
		TASK_CHECK(di==1);
		TASK_CHECK(p->len==2 && !memcmp(p->text, "AB", 2));
		/*no karaoke/highlight leftovers, readable through the regular text API*/
		TASK_CHECK(count_type(p, GF_ISOM_BOX_TYPE_KROK)==0);
		TASK_CHECK(count_type(p, GF_ISOM_BOX_TYPE_HLIT)==0);
		TASK_CHECK(p->highlight_color==NULL);
		if (exp_hi_end[i-1]) {
			TASK_CHECK(p->styles!=NULL && p->styles->entry_count==1);
			TASK_CHECK(p->styles->styles[0].startCharOffset==exp_hi_start[i-1]);
			TASK_CHECK(p->styles->styles[0].endCharOffset==exp_hi_end[i-1]);
			TASK_CHECK(p->styles->styles[0].text_color==0xFFFF0000);
		} else {
			TASK_CHECK(p->styles==NULL || p->styles->entry_count==0);
		}
		gf_isom_delete_text_sample(p);
	}
	TASK_CHECK(gf_isom_close(dst)==GF_OK);

	/*source track untouched*/
	src = gf_isom_open(".task-build/kb_basic_src.mp4", GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(src!=NULL);
	TASK_CHECK(gf_isom_get_sample_count(src, 1)==1);
	p = read_sample(src, 1, 1, &di, &dts, &dur);
	TASK_CHECK(p!=NULL);
	TASK_CHECK(count_type(p, GF_ISOM_BOX_TYPE_KROK)==1);
	gf_isom_delete_text_sample(p);
	TASK_CHECK(gf_isom_close(src)==GF_OK);
	return 0;
}

static int case_overlap_union(void)
{
	SrcW b;
	GF_TextSample *tx, *p;
	GF_ISOFile *src, *dst;
	u32 idx = 0, out_tr = 0, i, di;
	u64 dts;
	u32 dur;
	static const u64 exp_dts[5] = {0, 100, 200, 500, 600};
	static const u32 exp_dur[5] = {100, 100, 300, 100, 400};
	static const u32 exp_hi_start[5] = {0, 0, 0, 1, 0};
	static const u32 exp_hi_end[5] = {0, 2, 3, 3, 0};

	ensure_sys();
	TASK_CHECK(src_open(&b, ".task-build/kb_union_src.mp4", 1000)==0);
	TASK_CHECK(src_add_desc(&b, 0xFFFFFFFF, 18, 0, &idx)==0);
	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx!=NULL);
	TASK_CHECK(gf_isom_text_add_text(tx, "ABCD", 4)==GF_OK);
	TASK_CHECK(gf_isom_text_set_highlight_color(tx, 0xFFFF0000)==GF_OK);
	/*two karaoke sequences with overlapping times and ranges*/
	TASK_CHECK(gf_isom_text_add_karaoke(tx, 100)==GF_OK);
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 500, 0, 2)==GF_OK);
	TASK_CHECK(gf_isom_text_add_karaoke(tx, 200)==GF_OK);
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 600, 1, 3)==GF_OK);
	TASK_CHECK(src_sample(&b, tx, idx, 1000)==0);
	gf_isom_delete_text_sample(tx);
	TASK_CHECK(gf_isom_close(b.m)==GF_OK);

	src = gf_isom_open(".task-build/kb_union_src.mp4", GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(src!=NULL);
	dst = gf_isom_open(".task-build/kb_union_dst.mp4", GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(dst!=NULL);
	TASK_CHECK(gf_isom_text_bake_karaoke(src, 1, dst, &out_tr)==GF_OK);
	TASK_CHECK(out_tr==1);
	TASK_CHECK(gf_isom_close(src)==GF_OK);
	TASK_CHECK(gf_isom_close(dst)==GF_OK);

	dst = gf_isom_open(".task-build/kb_union_dst.mp4", GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(dst!=NULL);
	TASK_CHECK(gf_isom_get_sample_count(dst, 1)==5);
	for (i=1; i<=5; i++) {
		p = read_sample(dst, 1, i, &di, &dts, &dur);
		TASK_CHECK(p!=NULL);
		TASK_CHECK(dts==exp_dts[i-1]);
		TASK_CHECK(dur==exp_dur[i-1]);
		TASK_CHECK(p->len==4 && !memcmp(p->text, "ABCD", 4));
		if (exp_hi_end[i-1]) {
			TASK_CHECK(p->styles!=NULL && p->styles->entry_count==1);
			TASK_CHECK(p->styles->styles[0].startCharOffset==exp_hi_start[i-1]);
			TASK_CHECK(p->styles->styles[0].endCharOffset==exp_hi_end[i-1]);
			TASK_CHECK(p->styles->styles[0].text_color==0xFFFF0000);
		} else {
			TASK_CHECK(p->styles==NULL || p->styles->entry_count==0);
		}
		gf_isom_delete_text_sample(p);
	}
	TASK_CHECK(gf_isom_close(dst)==GF_OK);
	return 0;
}

static int case_zero_length_segments(void)
{
	SrcW b;
	GF_TextSample *tx, *p;
	GF_ISOFile *src, *dst;
	u32 idx = 0, out_tr = 0, i, di;
	u64 dts;
	u32 dur;
	static const u64 exp_dts[3] = {0, 400, 700};
	static const u32 exp_dur[3] = {400, 300, 300};

	ensure_sys();
	TASK_CHECK(src_open(&b, ".task-build/kb_zero_src.mp4", 1000)==0);
	TASK_CHECK(src_add_desc(&b, 0xFFFFFFFF, 18, 0, &idx)==0);
	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx!=NULL);
	TASK_CHECK(gf_isom_text_add_text(tx, "AB", 2)==GF_OK);
	TASK_CHECK(gf_isom_text_set_highlight_color(tx, 0xFFFF0000)==GF_OK);
	TASK_CHECK(gf_isom_text_add_karaoke(tx, 100)==GF_OK);
	/*zero-length time segment: no visible highlight*/
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 100, 0, 1)==GF_OK);
	/*empty character range: no visible highlight*/
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 400, 0, 0)==GF_OK);
	/*the only effective segment*/
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 700, 0, 1)==GF_OK);
	TASK_CHECK(src_sample(&b, tx, idx, 1000)==0);
	gf_isom_delete_text_sample(tx);
	TASK_CHECK(gf_isom_close(b.m)==GF_OK);

	src = gf_isom_open(".task-build/kb_zero_src.mp4", GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(src!=NULL);
	dst = gf_isom_open(".task-build/kb_zero_dst.mp4", GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(dst!=NULL);
	TASK_CHECK(gf_isom_text_bake_karaoke(src, 1, dst, &out_tr)==GF_OK);
	TASK_CHECK(out_tr==1);
	TASK_CHECK(gf_isom_close(src)==GF_OK);
	TASK_CHECK(gf_isom_close(dst)==GF_OK);

	dst = gf_isom_open(".task-build/kb_zero_dst.mp4", GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(dst!=NULL);
	/*the two leading highlight-free intervals are merged into a single sample*/
	TASK_CHECK(gf_isom_get_sample_count(dst, 1)==3);
	for (i=1; i<=3; i++) {
		p = read_sample(dst, 1, i, &di, &dts, &dur);
		TASK_CHECK(p!=NULL);
		TASK_CHECK(dts==exp_dts[i-1]);
		TASK_CHECK(dur==exp_dur[i-1]);
		if (i==2) {
			TASK_CHECK(p->styles!=NULL && p->styles->entry_count==1);
			TASK_CHECK(p->styles->styles[0].startCharOffset==0);
			TASK_CHECK(p->styles->styles[0].endCharOffset==1);
			TASK_CHECK(p->styles->styles[0].text_color==0xFFFF0000);
		} else {
			TASK_CHECK(p->styles==NULL || p->styles->entry_count==0);
		}
		gf_isom_delete_text_sample(p);
	}
	TASK_CHECK(gf_isom_close(dst)==GF_OK);
	return 0;
}

/* ========== style_resolution ========== */

static int case_styl_override_order(void)
{
	SrcW b;
	GF_TextSample *tx, *p;
	GF_ISOFile *src, *dst;
	GF_StyleRecord st;
	u32 idx = 0, out_tr = 0, di;
	u64 dts;
	u32 dur;

	ensure_sys();
	TASK_CHECK(src_open(&b, ".task-build/kb_styl_src.mp4", 1000)==0);
	TASK_CHECK(src_add_desc(&b, 0xFFFFFFFF, 18, 0, &idx)==0);
	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx!=NULL);
	TASK_CHECK(gf_isom_text_add_text(tx, "ABC", 3)==GF_OK);
	/*first record: [0,2) bold blue size 20*/
	memset(&st, 0, sizeof(st));
	st.startCharOffset = 0;
	st.endCharOffset = 2;
	st.fontID = 1;
	st.style_flags = GF_TXT_STYLE_BOLD;
	st.font_size = 20;
	st.text_color = 0xFF0000FF;
	TASK_CHECK(gf_isom_text_add_style(tx, &st)==GF_OK);
	/*second record: [1,3) italic green size 22 - wins over the first one on [1,2)*/
	st.startCharOffset = 1;
	st.endCharOffset = 3;
	st.style_flags = GF_TXT_STYLE_ITALIC;
	st.font_size = 22;
	st.text_color = 0xFF00FF00;
	TASK_CHECK(gf_isom_text_add_style(tx, &st)==GF_OK);
	TASK_CHECK(gf_isom_text_set_highlight_color(tx, 0xFFFF0000)==GF_OK);
	TASK_CHECK(gf_isom_text_add_karaoke(tx, 0)==GF_OK);
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 500, 1, 2)==GF_OK);
	TASK_CHECK(src_sample(&b, tx, idx, 1000)==0);
	gf_isom_delete_text_sample(tx);
	TASK_CHECK(gf_isom_close(b.m)==GF_OK);

	src = gf_isom_open(".task-build/kb_styl_src.mp4", GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(src!=NULL);
	dst = gf_isom_open(".task-build/kb_styl_dst.mp4", GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(dst!=NULL);
	TASK_CHECK(gf_isom_text_bake_karaoke(src, 1, dst, &out_tr)==GF_OK);
	TASK_CHECK(gf_isom_close(src)==GF_OK);
	TASK_CHECK(gf_isom_close(dst)==GF_OK);

	dst = gf_isom_open(".task-build/kb_styl_dst.mp4", GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(dst!=NULL);
	TASK_CHECK(gf_isom_get_sample_count(dst, 1)==2);

	/*first half: highlight on [1,2) replaces only the color of the winning record*/
	p = read_sample(dst, 1, 1, &di, &dts, &dur);
	TASK_CHECK(p!=NULL);
	TASK_CHECK(dur==500);
	TASK_CHECK(p->styles!=NULL && p->styles->entry_count==3);
	/*[0,1): bold blue size 20*/
	TASK_CHECK(p->styles->styles[0].startCharOffset==0 && p->styles->styles[0].endCharOffset==1);
	TASK_CHECK(p->styles->styles[0].style_flags==GF_TXT_STYLE_BOLD);
	TASK_CHECK(p->styles->styles[0].font_size==20);
	TASK_CHECK(p->styles->styles[0].text_color==0xFF0000FF);
	TASK_CHECK(p->styles->styles[0].fontID==1);
	/*[1,2): italic size 22, color replaced by the highlight color*/
	TASK_CHECK(p->styles->styles[1].startCharOffset==1 && p->styles->styles[1].endCharOffset==2);
	TASK_CHECK(p->styles->styles[1].style_flags==GF_TXT_STYLE_ITALIC);
	TASK_CHECK(p->styles->styles[1].font_size==22);
	TASK_CHECK(p->styles->styles[1].text_color==0xFFFF0000);
	TASK_CHECK(p->styles->styles[1].fontID==1);
	/*[2,3): italic green size 22*/
	TASK_CHECK(p->styles->styles[2].startCharOffset==2 && p->styles->styles[2].endCharOffset==3);
	TASK_CHECK(p->styles->styles[2].style_flags==GF_TXT_STYLE_ITALIC);
	TASK_CHECK(p->styles->styles[2].font_size==22);
	TASK_CHECK(p->styles->styles[2].text_color==0xFF00FF00);
	gf_isom_delete_text_sample(p);

	/*second half: no highlight, adjacent identical records merged*/
	p = read_sample(dst, 1, 2, &di, &dts, &dur);
	TASK_CHECK(p!=NULL);
	TASK_CHECK(dts==500 && dur==500);
	TASK_CHECK(p->styles!=NULL && p->styles->entry_count==2);
	TASK_CHECK(p->styles->styles[0].startCharOffset==0 && p->styles->styles[0].endCharOffset==1);
	TASK_CHECK(p->styles->styles[0].text_color==0xFF0000FF);
	TASK_CHECK(p->styles->styles[1].startCharOffset==1 && p->styles->styles[1].endCharOffset==3);
	TASK_CHECK(p->styles->styles[1].style_flags==GF_TXT_STYLE_ITALIC);
	TASK_CHECK(p->styles->styles[1].text_color==0xFF00FF00);
	gf_isom_delete_text_sample(p);
	TASK_CHECK(gf_isom_close(dst)==GF_OK);
	return 0;
}

static int case_merge_same_style(void)
{
	SrcW b;
	GF_TextSample *tx, *p;
	GF_ISOFile *src, *dst;
	GF_StyleRecord st;
	u32 idx = 0, out_tr = 0, di;
	u64 dts;
	u32 dur;

	ensure_sys();
	TASK_CHECK(src_open(&b, ".task-build/kb_merge_src.mp4", 1000)==0);
	TASK_CHECK(src_add_desc(&b, 0xFFFFFFFF, 18, 0, &idx)==0);
	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx!=NULL);
	TASK_CHECK(gf_isom_text_add_text(tx, "AB", 2)==GF_OK);
	/*two adjacent records with the exact same style*/
	memset(&st, 0, sizeof(st));
	st.startCharOffset = 0;
	st.endCharOffset = 1;
	st.fontID = 1;
	st.style_flags = GF_TXT_STYLE_BOLD;
	st.font_size = 18;
	st.text_color = 0xFF111111;
	TASK_CHECK(gf_isom_text_add_style(tx, &st)==GF_OK);
	st.startCharOffset = 1;
	st.endCharOffset = 2;
	TASK_CHECK(gf_isom_text_add_style(tx, &st)==GF_OK);
	/*highlight color identical to the effective color: no visible change*/
	TASK_CHECK(gf_isom_text_set_highlight_color(tx, 0xFF111111)==GF_OK);
	TASK_CHECK(gf_isom_text_add_karaoke(tx, 200)==GF_OK);
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 600, 0, 1)==GF_OK);
	TASK_CHECK(src_sample(&b, tx, idx, 1000)==0);
	gf_isom_delete_text_sample(tx);
	TASK_CHECK(gf_isom_close(b.m)==GF_OK);

	src = gf_isom_open(".task-build/kb_merge_src.mp4", GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(src!=NULL);
	dst = gf_isom_open(".task-build/kb_merge_dst.mp4", GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(dst!=NULL);
	TASK_CHECK(gf_isom_text_bake_karaoke(src, 1, dst, &out_tr)==GF_OK);
	TASK_CHECK(gf_isom_close(src)==GF_OK);
	TASK_CHECK(gf_isom_close(dst)==GF_OK);

	dst = gf_isom_open(".task-build/kb_merge_dst.mp4", GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(dst!=NULL);
	/*identical effective styles over the whole sample: a single output sample*/
	TASK_CHECK(gf_isom_get_sample_count(dst, 1)==1);
	p = read_sample(dst, 1, 1, &di, &dts, &dur);
	TASK_CHECK(p!=NULL);
	TASK_CHECK(dts==0 && dur==1000);
	/*adjacent ranges with identical complete styles are merged*/
	TASK_CHECK(p->styles!=NULL && p->styles->entry_count==1);
	TASK_CHECK(p->styles->styles[0].startCharOffset==0 && p->styles->styles[0].endCharOffset==2);
	TASK_CHECK(p->styles->styles[0].style_flags==GF_TXT_STYLE_BOLD);
	TASK_CHECK(p->styles->styles[0].text_color==0xFF111111);
	gf_isom_delete_text_sample(p);
	TASK_CHECK(gf_isom_close(dst)==GF_OK);
	return 0;
}

static int case_utf16_text(void)
{
	SrcW b;
	GF_TextSample *tx, *p;
	GF_ISOFile *src, *dst;
	u32 idx = 0, out_tr = 0, di;
	u64 dts;
	u32 dur;
	static const char txt_be[6] = {(char)0xFE, (char)0xFF, 0x00, 0x41, 0x00, 0x42};
	static const char txt_le[6] = {(char)0xFF, (char)0xFE, 0x41, 0x00, 0x42, 0x00};

	ensure_sys();
	TASK_CHECK(src_open(&b, ".task-build/kb_utf16_src.mp4", 1000)==0);
	TASK_CHECK(src_add_desc(&b, 0xFFFFFFFF, 18, 0, &idx)==0);
	/*UTF-16BE with BOM, karaoke on the second code point*/
	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx!=NULL);
	TASK_CHECK(gf_isom_text_add_text(tx, (char*)txt_be, 6)==GF_OK);
	TASK_CHECK(gf_isom_text_set_highlight_color(tx, 0xFFFF0000)==GF_OK);
	TASK_CHECK(gf_isom_text_add_karaoke(tx, 0)==GF_OK);
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 500, 1, 2)==GF_OK);
	TASK_CHECK(src_sample(&b, tx, idx, 500)==0);
	gf_isom_delete_text_sample(tx);
	/*UTF-16LE with BOM, static highlight on the first code point*/
	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx!=NULL);
	TASK_CHECK(gf_isom_text_add_text(tx, (char*)txt_le, 6)==GF_OK);
	TASK_CHECK(gf_isom_text_set_highlight_color(tx, 0xFF00FF00)==GF_OK);
	TASK_CHECK(gf_isom_text_add_highlight(tx, 0, 1)==GF_OK);
	TASK_CHECK(src_sample(&b, tx, idx, 500)==0);
	gf_isom_delete_text_sample(tx);
	TASK_CHECK(gf_isom_close(b.m)==GF_OK);

	src = gf_isom_open(".task-build/kb_utf16_src.mp4", GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(src!=NULL);
	dst = gf_isom_open(".task-build/kb_utf16_dst.mp4", GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(dst!=NULL);
	TASK_CHECK(gf_isom_text_bake_karaoke(src, 1, dst, &out_tr)==GF_OK);
	TASK_CHECK(gf_isom_close(src)==GF_OK);
	TASK_CHECK(gf_isom_close(dst)==GF_OK);

	dst = gf_isom_open(".task-build/kb_utf16_dst.mp4", GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(dst!=NULL);
	TASK_CHECK(gf_isom_get_sample_count(dst, 1)==2);
	/*original text bytes are kept untouched, ranges are in code points*/
	p = read_sample(dst, 1, 1, &di, &dts, &dur);
	TASK_CHECK(p!=NULL);
	TASK_CHECK(dts==0 && dur==500);
	TASK_CHECK(p->len==6 && !memcmp(p->text, txt_be, 6));
	TASK_CHECK(p->styles!=NULL && p->styles->entry_count==1);
	TASK_CHECK(p->styles->styles[0].startCharOffset==1 && p->styles->styles[0].endCharOffset==2);
	TASK_CHECK(p->styles->styles[0].text_color==0xFFFF0000);
	TASK_CHECK(count_type(p, GF_ISOM_BOX_TYPE_KROK)==0);
	gf_isom_delete_text_sample(p);
	p = read_sample(dst, 1, 2, &di, &dts, &dur);
	TASK_CHECK(p!=NULL);
	TASK_CHECK(dts==500 && dur==500);
	TASK_CHECK(p->len==6 && !memcmp(p->text, txt_le, 6));
	TASK_CHECK(p->styles!=NULL && p->styles->entry_count==1);
	TASK_CHECK(p->styles->styles[0].startCharOffset==0 && p->styles->styles[0].endCharOffset==1);
	TASK_CHECK(p->styles->styles[0].text_color==0xFF00FF00);
	TASK_CHECK(count_type(p, GF_ISOM_BOX_TYPE_HLIT)==0);
	TASK_CHECK(p->highlight_color==NULL);
	gf_isom_delete_text_sample(p);
	TASK_CHECK(gf_isom_close(dst)==GF_OK);
	return 0;
}

static int case_missing_hclr(void)
{
	SrcW b;
	GF_TextSample *tx;
	GF_ISOFile *src, *dst;
	u32 idx = 0, out_tr = 0xFFFFFFFF;

	ensure_sys();
	TASK_CHECK(src_open(&b, ".task-build/kb_nohclr_src.mp4", 1000)==0);
	TASK_CHECK(src_add_desc(&b, 0xFFFFFFFF, 18, 0, &idx)==0);
	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx!=NULL);
	TASK_CHECK(gf_isom_text_add_text(tx, "AB", 2)==GF_OK);
	/*non-empty highlight but no highlight color*/
	TASK_CHECK(gf_isom_text_add_karaoke(tx, 100)==GF_OK);
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 400, 0, 1)==GF_OK);
	TASK_CHECK(src_sample(&b, tx, idx, 1000)==0);
	gf_isom_delete_text_sample(tx);
	TASK_CHECK(gf_isom_close(b.m)==GF_OK);

	src = gf_isom_open(".task-build/kb_nohclr_src.mp4", GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(src!=NULL);
	dst = gf_isom_open(".task-build/kb_nohclr_dst.mp4", GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(dst!=NULL);
	TASK_CHECK(gf_isom_text_bake_karaoke(src, 1, dst, &out_tr)==GF_NOT_SUPPORTED);
	TASK_CHECK(out_tr==0);
	/*no half-written track, destination still usable*/
	TASK_CHECK(gf_isom_get_track_count(dst)==0);
	TASK_CHECK(gf_isom_new_track(dst, 5, GF_ISOM_MEDIA_TEXT, 1000)==1);
	TASK_CHECK(gf_isom_close(src)==GF_OK);
	TASK_CHECK(gf_isom_close(dst)==GF_OK);
	return 0;
}

/* ========== movie_roundtrip ========== */

static int case_desc_switch(void)
{
	SrcW b;
	GF_TextSample *tx, *p;
	GF_ISOFile *src, *dst;
	GF_TextSampleDescriptor *rd = NULL;
	u32 idx1 = 0, idx2 = 0, out_tr = 0, i, di;
	u64 dts;
	u32 dur;
	static const u64 exp_dts[5] = {0, 100, 300, 500, 800};
	static const u32 exp_desc[5] = {1, 1, 1, 2, 1};
	static const char *exp_txt[5] = {"AB", "AB", "AB", "CD", "EF"};

	ensure_sys();
	TASK_CHECK(src_open(&b, ".task-build/kb_desc_src.mp4", 1000)==0);
	TASK_CHECK(src_add_desc(&b, 0xFFFFFFFF, 18, 0, &idx1)==0);
	TASK_CHECK(src_add_desc(&b, 0xFFFFFF00, 24, 0, &idx2)==0);
	TASK_CHECK(idx1==1 && idx2==2);
	/*sample 1 on desc 1 with karaoke*/
	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx!=NULL);
	TASK_CHECK(gf_isom_text_add_text(tx, "AB", 2)==GF_OK);
	TASK_CHECK(gf_isom_text_set_highlight_color(tx, 0xFFFF0000)==GF_OK);
	TASK_CHECK(gf_isom_text_add_karaoke(tx, 100)==GF_OK);
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 300, 0, 1)==GF_OK);
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 500, 1, 2)==GF_OK);
	TASK_CHECK(src_sample(&b, tx, idx1, 500)==0);
	gf_isom_delete_text_sample(tx);
	/*sample 2 on desc 2, plain*/
	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx!=NULL);
	TASK_CHECK(gf_isom_text_add_text(tx, "CD", 2)==GF_OK);
	TASK_CHECK(src_sample(&b, tx, idx2, 300)==0);
	gf_isom_delete_text_sample(tx);
	/*sample 3 back on desc 1 with a static highlight*/
	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx!=NULL);
	TASK_CHECK(gf_isom_text_add_text(tx, "EF", 2)==GF_OK);
	TASK_CHECK(gf_isom_text_set_highlight_color(tx, 0xFFFF0000)==GF_OK);
	TASK_CHECK(gf_isom_text_add_highlight(tx, 0, 2)==GF_OK);
	TASK_CHECK(src_sample(&b, tx, idx1, 200)==0);
	gf_isom_delete_text_sample(tx);
	TASK_CHECK(gf_isom_close(b.m)==GF_OK);

	src = gf_isom_open(".task-build/kb_desc_src.mp4", GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(src!=NULL);
	dst = gf_isom_open(".task-build/kb_desc_dst.mp4", GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(dst!=NULL);
	TASK_CHECK(gf_isom_text_bake_karaoke(src, 1, dst, &out_tr)==GF_OK);
	TASK_CHECK(out_tr==1);
	TASK_CHECK(gf_isom_close(src)==GF_OK);
	TASK_CHECK(gf_isom_close(dst)==GF_OK);

	/*reopen the saved file and check descriptions, switches and total duration*/
	dst = gf_isom_open(".task-build/kb_desc_dst.mp4", GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(dst!=NULL);
	TASK_CHECK(gf_isom_get_sample_description_count(dst, 1)==2);
	TASK_CHECK(gf_isom_get_text_description(dst, 1, 2, &rd)==GF_OK);
	TASK_CHECK(rd!=NULL);
	TASK_CHECK(rd->default_style.text_color==0xFFFFFF00);
	TASK_CHECK(rd->default_style.font_size==24);
	TASK_CHECK(rd->font_count==1);
	gf_odf_desc_del((GF_Descriptor*)rd);
	rd = NULL;
	TASK_CHECK(gf_isom_get_text_description(dst, 1, 1, &rd)==GF_OK);
	TASK_CHECK(rd->default_style.text_color==0xFFFFFFFF);
	TASK_CHECK(rd->default_style.font_size==18);
	gf_odf_desc_del((GF_Descriptor*)rd);
	rd = NULL;
	TASK_CHECK(gf_isom_get_media_duration(dst, 1)==1000);
	TASK_CHECK(gf_isom_get_media_timescale(dst, 1)==1000);
	TASK_CHECK(gf_isom_get_sample_count(dst, 1)==5);
	for (i=1; i<=5; i++) {
		p = read_sample(dst, 1, i, &di, &dts, &dur);
		TASK_CHECK(p!=NULL);
		TASK_CHECK(dts==exp_dts[i-1]);
		TASK_CHECK(di==exp_desc[i-1]);
		TASK_CHECK(p->len==2 && !memcmp(p->text, exp_txt[i-1], 2));
		TASK_CHECK(count_type(p, GF_ISOM_BOX_TYPE_KROK)==0);
		TASK_CHECK(count_type(p, GF_ISOM_BOX_TYPE_HLIT)==0);
		gf_isom_delete_text_sample(p);
	}
	TASK_CHECK(gf_isom_close(dst)==GF_OK);
	return 0;
}

static int case_clear_screen_and_edits(void)
{
	SrcW b;
	GF_TextSample *tx, *p;
	GF_ISOFile *src, *dst;
	u32 idx = 0, out_tr = 0, di;
	u64 dts, et, sd, mt;
	u32 dur;
	GF_ISOEditType mode;

	ensure_sys();
	TASK_CHECK(src_open(&b, ".task-build/kb_clear_src.mp4", 1000)==0);
	TASK_CHECK(gf_isom_set_timescale(b.m, 1000)==GF_OK);
	TASK_CHECK(src_add_desc(&b, 0xFFFFFFFF, 18, 0, &idx)==0);
	/*sample 1: text with karaoke*/
	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx!=NULL);
	TASK_CHECK(gf_isom_text_add_text(tx, "AB", 2)==GF_OK);
	TASK_CHECK(gf_isom_text_set_highlight_color(tx, 0xFFFF0000)==GF_OK);
	TASK_CHECK(gf_isom_text_add_karaoke(tx, 100)==GF_OK);
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 400, 0, 1)==GF_OK);
	TASK_CHECK(src_sample(&b, tx, idx, 400)==0);
	gf_isom_delete_text_sample(tx);
	/*sample 2: empty text, clears the screen*/
	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx!=NULL);
	TASK_CHECK(src_sample(&b, tx, idx, 600)==0);
	gf_isom_delete_text_sample(tx);
	/*an empty edit followed by a normal one*/
	TASK_CHECK(gf_isom_set_edit(b.m, b.tr, 0, 500, 0, GF_ISOM_EDIT_EMPTY)==GF_OK);
	TASK_CHECK(gf_isom_set_edit(b.m, b.tr, 500, 1000, 0, GF_ISOM_EDIT_NORMAL)==GF_OK);
	TASK_CHECK(gf_isom_close(b.m)==GF_OK);

	src = gf_isom_open(".task-build/kb_clear_src.mp4", GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(src!=NULL);
	dst = gf_isom_open(".task-build/kb_clear_dst.mp4", GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(dst!=NULL);
	TASK_CHECK(gf_isom_set_timescale(dst, 1000)==GF_OK);
	TASK_CHECK(gf_isom_text_bake_karaoke(src, 1, dst, &out_tr)==GF_OK);
	TASK_CHECK(out_tr==1);
	TASK_CHECK(gf_isom_close(src)==GF_OK);
	TASK_CHECK(gf_isom_close(dst)==GF_OK);

	dst = gf_isom_open(".task-build/kb_clear_dst.mp4", GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(dst!=NULL);
	/*edit list preserved*/
	TASK_CHECK(gf_isom_get_edits_count(dst, 1)==2);
	TASK_CHECK(gf_isom_get_edit(dst, 1, 1, &et, &sd, &mt, &mode)==GF_OK);
	TASK_CHECK(et==0 && sd==500 && mode==GF_ISOM_EDIT_EMPTY);
	TASK_CHECK(gf_isom_get_edit(dst, 1, 2, &et, &sd, &mt, &mode)==GF_OK);
	TASK_CHECK(et==500 && sd==1000 && mt==0 && mode==GF_ISOM_EDIT_NORMAL);
	/*3 samples: the split text and the clear screen*/
	TASK_CHECK(gf_isom_get_sample_count(dst, 1)==3);
	TASK_CHECK(gf_isom_get_media_duration(dst, 1)==1000);
	p = read_sample(dst, 1, 3, &di, &dts, &dur);
	TASK_CHECK(p!=NULL);
	TASK_CHECK(dts==400 && dur==600);
	TASK_CHECK(p->len==0);
	TASK_CHECK(p->styles==NULL || p->styles->entry_count==0);
	gf_isom_delete_text_sample(p);
	p = read_sample(dst, 1, 2, &di, &dts, &dur);
	TASK_CHECK(p!=NULL);
	TASK_CHECK(dts==100 && dur==300);
	TASK_CHECK(p->styles!=NULL && p->styles->entry_count==1);
	TASK_CHECK(p->styles->styles[0].text_color==0xFFFF0000);
	gf_isom_delete_text_sample(p);
	TASK_CHECK(gf_isom_close(dst)==GF_OK);
	return 0;
}

static int make_bad_source(const char *path, u32 kind)
{
	SrcW b;
	GF_TextSample *tx;
	u32 idx = 0;
	if (src_open(&b, path, 1000)) return 1;
	if (src_add_desc(&b, 0xFFFFFFFF, 18, 0, &idx)) return 1;
	tx = gf_isom_new_text_sample();
	if (!tx) return 1;
	if (kind==4) {
		/*broken UTF-8: truncated multibyte sequence*/
		if (gf_isom_text_add_text(tx, "\xC3\x28", 2)) return 1;
	} else {
		if (gf_isom_text_add_text(tx, "AB", 2)) return 1;
	}
	if (gf_isom_text_set_highlight_color(tx, 0xFFFF0000)) return 1;
	switch (kind) {
	case 1: /*karaoke time beyond the sample duration*/
		if (gf_isom_text_add_karaoke(tx, 100)) return 1;
		if (gf_isom_text_set_karaoke_segment(tx, 1500, 0, 1)) return 1;
		break;
	case 2: /*unsupported sample extension (scroll delay)*/
		if (gf_isom_text_set_scroll_delay(tx, 50)) return 1;
		break;
	case 3: /*character range out of bounds*/
		if (gf_isom_text_add_karaoke(tx, 100)) return 1;
		if (gf_isom_text_set_karaoke_segment(tx, 400, 0, 5)) return 1;
		break;
	case 5: /*karaoke times going backwards*/
		if (gf_isom_text_add_karaoke(tx, 100)) return 1;
		if (gf_isom_text_set_karaoke_segment(tx, 50, 0, 1)) return 1;
		break;
	default:
		break;
	}
	if (src_sample(&b, tx, idx, 1000)) return 1;
	gf_isom_delete_text_sample(tx);
	return gf_isom_close(b.m) != GF_OK;
}

static int case_modifiers_preserved(void)
{
	SrcW b;
	GF_TextSample *tx, *p;
	GF_ISOFile *src, *dst;
	GF_Box *a;
	GF_TextHyperTextBox *href;
	u32 idx = 0, out_tr = 0, i, di, k;
	u64 dts;
	u32 dur;

	ensure_sys();
	TASK_CHECK(src_open(&b, ".task-build/kb_mods_src.mp4", 1000)==0);
	TASK_CHECK(src_add_desc(&b, 0xFFFFFFFF, 18, 0, &idx)==0);
	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx!=NULL);
	TASK_CHECK(gf_isom_text_add_text(tx, "AB", 2)==GF_OK);
	TASK_CHECK(gf_isom_text_set_box(tx, 10, 20, 30, 40)==GF_OK);
	TASK_CHECK(gf_isom_text_set_wrap(tx, 1)==GF_OK);
	TASK_CHECK(gf_isom_text_set_forced(tx, GF_TRUE)==GF_OK);
	TASK_CHECK(gf_isom_text_add_hyperlink(tx, "http://example.com", "hint", 0, 1)==GF_OK);
	TASK_CHECK(gf_isom_text_set_highlight_color(tx, 0xFFFF0000)==GF_OK);
	TASK_CHECK(gf_isom_text_add_karaoke(tx, 100)==GF_OK);
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 400, 0, 1)==GF_OK);
	TASK_CHECK(src_sample(&b, tx, idx, 1000)==0);
	gf_isom_delete_text_sample(tx);
	TASK_CHECK(gf_isom_close(b.m)==GF_OK);

	src = gf_isom_open(".task-build/kb_mods_src.mp4", GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(src!=NULL);
	dst = gf_isom_open(".task-build/kb_mods_dst.mp4", GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(dst!=NULL);
	TASK_CHECK(gf_isom_text_bake_karaoke(src, 1, dst, &out_tr)==GF_OK);
	TASK_CHECK(gf_isom_close(src)==GF_OK);
	TASK_CHECK(gf_isom_close(dst)==GF_OK);

	dst = gf_isom_open(".task-build/kb_mods_dst.mp4", GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(dst!=NULL);
	TASK_CHECK(gf_isom_get_sample_count(dst, 1)==3);
	for (i=1; i<=3; i++) {
		p = read_sample(dst, 1, i, &di, &dts, &dur);
		TASK_CHECK(p!=NULL);
		/*tbox, twrp, frcd and href are preserved on every split sample*/
		TASK_CHECK(p->box!=NULL);
		TASK_CHECK(p->box->box.top==10 && p->box->box.left==20);
		TASK_CHECK(p->box->box.bottom==30 && p->box->box.right==40);
		TASK_CHECK(p->wrap!=NULL && p->wrap->wrap_flag==1);
		TASK_CHECK(p->is_forced==GF_TRUE);
		TASK_CHECK(count_type(p, GF_ISOM_BOX_TYPE_HREF)==1);
		TASK_CHECK(count_type(p, GF_ISOM_BOX_TYPE_KROK)==0);
		TASK_CHECK(count_type(p, GF_ISOM_BOX_TYPE_HLIT)==0);
		TASK_CHECK(p->highlight_color==NULL);
		href = NULL;
		k = 0;
		while ((a = (GF_Box*)gf_list_enum(p->others, &k))) {
			if (a->type==GF_ISOM_BOX_TYPE_HREF) href = (GF_TextHyperTextBox *)a;
		}
		TASK_CHECK(href!=NULL);
		TASK_CHECK(href->startcharoffset==0 && href->endcharoffset==1);
		TASK_CHECK(href->URL!=NULL && !strcmp(href->URL, "http://example.com"));
		TASK_CHECK(href->URL_hint!=NULL && !strcmp(href->URL_hint, "hint"));
		if (i==2) {
			TASK_CHECK(p->styles!=NULL && p->styles->entry_count==1);
			TASK_CHECK(p->styles->styles[0].text_color==0xFFFF0000);
		} else {
			TASK_CHECK(p->styles==NULL || p->styles->entry_count==0);
		}
		gf_isom_delete_text_sample(p);
	}
	TASK_CHECK(gf_isom_close(dst)==GF_OK);
	return 0;
}

static int case_failure_recovery(void)
{
	SrcW b;
	GF_TextSample *tx;
	GF_ISOFile *src, *dst, *bad;
	GF_Err e;
	u32 idx = 0, out_tr = 0xFFFFFFFF, i;

	ensure_sys();
	/*a valid source*/
	TASK_CHECK(src_open(&b, ".task-build/kb_good_src.mp4", 1000)==0);
	TASK_CHECK(src_add_desc(&b, 0xFFFFFFFF, 18, 0, &idx)==0);
	tx = gf_isom_new_text_sample();
	TASK_CHECK(tx!=NULL);
	TASK_CHECK(gf_isom_text_add_text(tx, "AB", 2)==GF_OK);
	TASK_CHECK(gf_isom_text_set_highlight_color(tx, 0xFFFF0000)==GF_OK);
	TASK_CHECK(gf_isom_text_add_karaoke(tx, 100)==GF_OK);
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 400, 0, 1)==GF_OK);
	TASK_CHECK(gf_isom_text_set_karaoke_segment(tx, 800, 1, 2)==GF_OK);
	TASK_CHECK(src_sample(&b, tx, idx, 1000)==0);
	gf_isom_delete_text_sample(tx);
	TASK_CHECK(gf_isom_close(b.m)==GF_OK);
	/*invalid sources*/
	TASK_CHECK(make_bad_source(".task-build/kb_bad1_src.mp4", 1)==0);
	TASK_CHECK(make_bad_source(".task-build/kb_bad2_src.mp4", 2)==0);
	TASK_CHECK(make_bad_source(".task-build/kb_bad3_src.mp4", 3)==0);
	TASK_CHECK(make_bad_source(".task-build/kb_bad4_src.mp4", 4)==0);
	TASK_CHECK(make_bad_source(".task-build/kb_bad5_src.mp4", 5)==0);

	dst = gf_isom_open(".task-build/kb_fail_dst.mp4", GF_ISOM_WRITE_EDIT, NULL);
	TASK_CHECK(dst!=NULL);

	/*first a successful bake*/
	src = gf_isom_open(".task-build/kb_good_src.mp4", GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(src!=NULL);
	TASK_CHECK(gf_isom_text_bake_karaoke(src, 1, dst, &out_tr)==GF_OK);
	TASK_CHECK(out_tr==1);
	/*src == dst is rejected*/
	out_tr = 0xFFFFFFFF;
	TASK_CHECK(gf_isom_text_bake_karaoke(src, 1, src, &out_tr)==GF_BAD_PARAM);
	TASK_CHECK(out_tr==0);
	TASK_CHECK(gf_isom_close(src)==GF_OK);
	TASK_CHECK(gf_isom_get_track_count(dst)==1);

	/*failing bakes leave no partial track and keep the destination usable*/
	for (i=1; i<=5; i++) {
		char path[64];
		GF_Err expected = (i==2) ? GF_NOT_SUPPORTED : GF_CORRUPTED_DATA;
		snprintf(path, sizeof(path), ".task-build/kb_bad%u_src.mp4", i);
		bad = gf_isom_open(path, GF_ISOM_OPEN_READ, NULL);
		TASK_CHECK(bad!=NULL);
		out_tr = 0xFFFFFFFF;
		e = gf_isom_text_bake_karaoke(bad, 1, dst, &out_tr);
		TASK_CHECK(e==expected);
		TASK_CHECK(out_tr==0);
		TASK_CHECK(gf_isom_get_track_count(dst)==1);
		/*the failed source is still readable*/
		TASK_CHECK(gf_isom_get_sample_count(bad, 1)==1);
		TASK_CHECK(gf_isom_close(bad)==GF_OK);
	}

	/*the destination still accepts new tracks after the failures*/
	src = gf_isom_open(".task-build/kb_good_src.mp4", GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(src!=NULL);
	out_tr = 0;
	TASK_CHECK(gf_isom_text_bake_karaoke(src, 1, dst, &out_tr)==GF_OK);
	TASK_CHECK(out_tr==2);
	TASK_CHECK(gf_isom_close(src)==GF_OK);
	TASK_CHECK(gf_isom_close(dst)==GF_OK);

	/*reopen and check both baked tracks*/
	dst = gf_isom_open(".task-build/kb_fail_dst.mp4", GF_ISOM_OPEN_READ, NULL);
	TASK_CHECK(dst!=NULL);
	TASK_CHECK(gf_isom_get_track_count(dst)==2);
	TASK_CHECK(gf_isom_get_sample_count(dst, 1)==4);
	TASK_CHECK(gf_isom_get_sample_count(dst, 2)==4);
	TASK_CHECK(gf_isom_get_media_duration(dst, 1)==1000);
	TASK_CHECK(gf_isom_get_media_duration(dst, 2)==1000);
	TASK_CHECK(gf_isom_close(dst)==GF_OK);
	return 0;
}

const struct task_case task_cases[] = {
	{"event_intervals", "basic_split", case_basic_split},
	{"event_intervals", "overlap_union", case_overlap_union},
	{"event_intervals", "zero_length_segments", case_zero_length_segments},
	{"style_resolution", "styl_override_order", case_styl_override_order},
	{"style_resolution", "merge_same_style", case_merge_same_style},
	{"style_resolution", "utf16_text", case_utf16_text},
	{"style_resolution", "missing_hclr", case_missing_hclr},
	{"movie_roundtrip", "desc_switch", case_desc_switch},
	{"movie_roundtrip", "clear_screen_and_edits", case_clear_screen_and_edits},
	{"movie_roundtrip", "modifiers_preserved", case_modifiers_preserved},
	{"movie_roundtrip", "failure_recovery", case_failure_recovery},
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
