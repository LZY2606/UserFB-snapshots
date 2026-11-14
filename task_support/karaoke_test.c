// Task test suite for structured, editable WebVTT cue text and karaoke
// preview. Registered through task_support/check.h in three groups:
// parse_edit, phases and retime_export.

#include "check.h"
#include "caption/vtt.h"
#include "caption/srt.h"
#include "caption/caption.h"
#include "caption/eia608.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static vtt_t* parse_cue_ms(int64_t start_ms, int64_t dur_ms, const char* payload)
{
    char doc[4096];
    int64_t end_ms = start_ms + dur_ms;
    int n = snprintf(doc, sizeof(doc),
        "WEBVTT\r\n\r\n"
        "%02lld:%02lld:%02lld.%03lld --> %02lld:%02lld:%02lld.%03lld\r\n"
        "%s\r\n\r\n",
        (long long)(start_ms / 3600000), (long long)((start_ms / 60000) % 60),
        (long long)((start_ms / 1000) % 60), (long long)(start_ms % 1000),
        (long long)(end_ms / 3600000), (long long)((end_ms / 60000) % 60),
        (long long)((end_ms / 1000) % 60), (long long)(end_ms % 1000), payload);

    if (n <= 0 || (size_t)n >= sizeof(doc)) {
        return NULL;
    }

    return vtt_parse(doc, (size_t)n);
}

static const vtt_text_token_t* find_first(vtt_block_t* cue, enum VTT_TEXT_TOKEN_TYPE type)
{
    for (size_t i = 0; i < vtt_cue_token_count(cue); ++i) {
        const vtt_text_token_t* tok = vtt_cue_token(cue, i);

        if (tok->type == type) {
            return tok;
        }
    }

    return NULL;
}

////////////////////////////////////////////////////////////////////////////////
// group: parse_edit
static int parse_basic_tokens(void)
{
    vtt_t* vtt = parse_cue_ms(10000, 4000, "<i>A&amp;B</i><00:00:12.000><u>CD</u>");
    TASK_CHECK(vtt && vtt->cue_head);
    vtt_block_t* cue = vtt->cue_head;

    TASK_CHECK(3 == vtt_cue_token_count(cue));
    const vtt_text_token_t* t0 = vtt_cue_token(cue, 0);
    const vtt_text_token_t* t1 = vtt_cue_token(cue, 1);
    const vtt_text_token_t* t2 = vtt_cue_token(cue, 2);
    TASK_CHECK(t0 && t1 && t2);
    TASK_CHECK(VTT_TOKEN_TEXT == t0->type);
    TASK_CHECK(0 == strcmp(t0->text, "A&B"));
    TASK_CHECK(t0->italic && !t0->underline && !t0->bold);
    TASK_CHECK(VTT_TOKEN_TIME == t1->type && 12000 == t1->time_ms);
    TASK_CHECK(VTT_TOKEN_TEXT == t2->type);
    TASK_CHECK(0 == strcmp(t2->text, "CD") && t2->underline && !t2->italic);

    // the original payload stays available, untouched
    const char* raw = vtt_block_data(cue);
    TASK_CHECK(strstr(raw, "A&amp;B") && strstr(raw, "<00:00:12.000>"));

    // mm:ss.mmm form and hh:mm:ss.mmm form parse identically
    vtt_t* v2 = parse_cue_ms(10000, 4000, "<00:12.000>x");
    TASK_CHECK(v2);
    const vtt_text_token_t* mm = find_first(v2->cue_head, VTT_TOKEN_TIME);
    TASK_CHECK(mm && 12000 == mm->time_ms);
    vtt_free(v2);

    vtt_free(vtt);
    return 0;
}

static int parse_entities_nesting_newline(void)
{
    vtt_t* vtt = parse_cue_ms(0, 10000, "<b><i>x</i></b> a&nbsp;b&lt;c&gt;d&amp;e\nsecond");
    TASK_CHECK(vtt && vtt->cue_head);
    vtt_block_t* cue = vtt->cue_head;

    const vtt_text_token_t* styled = vtt_cue_token(cue, 0);
    TASK_CHECK(styled && VTT_TOKEN_TEXT == styled->type);
    TASK_CHECK(styled->bold && styled->italic && 0 == strcmp(styled->text, "x"));

    // &nbsp; decodes to U+00A0 (0xC2 0xA0), entities only one layer deep
    const vtt_text_token_t* decoded = NULL;
    for (size_t i = 0; i < vtt_cue_token_count(cue); ++i) {
        const vtt_text_token_t* tok = vtt_cue_token(cue, i);
        if (VTT_TOKEN_TEXT == tok->type && strstr(tok->text, "\xC2\xA0")) {
            decoded = tok;
        }
    }
    TASK_CHECK(decoded && strstr(decoded->text, "<c>d&e"));

    TASK_CHECK(find_first(cue, VTT_TOKEN_NEWLINE) != NULL);

    // serialization escapes the decoded entities and wraps the style state
    char buf[1024];
    size_t n = vtt_cue_serialize(cue, buf, sizeof(buf));
    TASK_CHECK((size_t)-1 != n);
    TASK_CHECK(strstr(buf, "&nbsp;") && strstr(buf, "&lt;c&gt;") && strstr(buf, "&amp;"));
    TASK_CHECK(strstr(buf, "<b>") && strstr(buf, "<i>x</i>") && strstr(buf, "</b>"));

    // the re-parsed serialization has the same text and token kinds
    vtt_t* again = parse_cue_ms(0, 10000, buf);
    TASK_CHECK(again && again->cue_head);
    TASK_CHECK(vtt_cue_token_count(again->cue_head) == vtt_cue_token_count(cue));
    TASK_CHECK(0 == strcmp(vtt_cue_token(again->cue_head, 0)->text, "x"));
    vtt_free(again);

    // unclosed format tags are rejected
    vtt_t* bad = parse_cue_ms(0, 10000, "<i>x");
    TASK_CHECK(bad && 0 == vtt_cue_token_count(bad->cue_head));
    // the rejected cue stays usable (retime) and releasable
    TASK_CHECK(LIBCAPTION_OK == vtt_cue_retime(bad->cue_head, 1.0, 2.0));
    vtt_free(bad);

    vtt_free(vtt);
    return 0;
}

static int edit_text_escapes_and_reparse(void)
{
    vtt_t* vtt = parse_cue_ms(10000, 4000, "<i>A&amp;B</i><00:00:12.000><u>CD</u>");
    TASK_CHECK(vtt && vtt->cue_head);
    vtt_block_t* cue = vtt->cue_head;

    // literal "<X&" is stored verbatim and must be escaped on output
    TASK_CHECK(LIBCAPTION_OK == vtt_cue_token_set_text(cue, 0, "<X&"));
    TASK_CHECK(0 == strcmp(vtt_cue_token(cue, 0)->text, "<X&"));

    char buf[1024];
    size_t n = vtt_cue_serialize(cue, buf, sizeof(buf));
    TASK_CHECK((size_t)-1 != n);
    TASK_CHECK(strstr(buf, "<i>&lt;X&amp;</i>") != NULL);
    TASK_CHECK(strstr(buf, "<X&") == NULL);

    // re-reading the edited text cannot activate it as markup
    vtt_t* again = parse_cue_ms(10000, 4000, buf);
    TASK_CHECK(again && again->cue_head);
    vtt_block_t* cue2 = again->cue_head;
    TASK_CHECK(3 == vtt_cue_token_count(cue2));
    TASK_CHECK(VTT_TOKEN_TEXT == vtt_cue_token(cue2, 0)->type);
    TASK_CHECK(0 == strcmp(vtt_cue_token(cue2, 0)->text, "<X&"));
    TASK_CHECK(12000 == vtt_cue_token(cue2, 1)->time_ms);
    vtt_free(again);

    // invalid edits are rejected and leave the cue editable
    TASK_CHECK(LIBCAPTION_ERROR == vtt_cue_token_set_text(cue, 1, "x")); // time token
    TASK_CHECK(LIBCAPTION_ERROR == vtt_cue_token_set_text(cue, 99, "x")); // bad index
    TASK_CHECK(LIBCAPTION_ERROR == vtt_cue_token_set_text(cue, 0, "\xC0\x80")); // overlong utf8
    TASK_CHECK(LIBCAPTION_ERROR == vtt_cue_token_set_time(cue, 0, 13.0)); // text token
    TASK_CHECK(LIBCAPTION_OK == vtt_cue_token_set_text(cue, 2, "EF"));
    TASK_CHECK(0 == strcmp(vtt_cue_token(cue, 2)->text, "EF"));

    // original payload is still the compatibility copy
    TASK_CHECK(strstr(vtt_block_data(cue), "A&amp;B") != NULL);
    vtt_free(vtt);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// group: phases
static int phases_past_future_and_bounds(void)
{
    vtt_t* vtt = parse_cue_ms(10000, 4000, "<i>A&amp;B</i><00:00:12.000><u>CD</u>");
    TASK_CHECK(vtt && vtt->cue_head);
    vtt_block_t* cue = vtt->cue_head;

    caption_frame_t early;
    TASK_CHECK(LIBCAPTION_OK == vtt_cue_to_caption_frame_at(cue, 11.0, &early));
    // five visible cells, time tag occupies none
    TASK_CHECK(early.front.cell[0][0].data[0] == 'A');
    TASK_CHECK(early.front.cell[0][1].data[0] == '&');
    TASK_CHECK(early.front.cell[0][2].data[0] == 'B');
    TASK_CHECK(early.front.cell[0][3].data[0] == 'C');
    TASK_CHECK(early.front.cell[0][4].data[0] == 'D');
    TASK_CHECK(early.front.cell[0][5].data[0] == 0);
    TASK_CHECK(caption_phase_past == caption_frame_read_phase(&early, 0, 0));
    TASK_CHECK(caption_phase_past == caption_frame_read_phase(&early, 0, 1));
    TASK_CHECK(caption_phase_past == caption_frame_read_phase(&early, 0, 2));
    TASK_CHECK(caption_phase_future == caption_frame_read_phase(&early, 0, 3));
    TASK_CHECK(caption_phase_future == caption_frame_read_phase(&early, 0, 4));
    // styles travel with the cells; bold has no cell representation
    TASK_CHECK(eia608_style_italics == early.front.cell[0][0].sty);
    TASK_CHECK(early.front.cell[0][3].uln);

    // equal to the marker counts as PAST
    caption_frame_t at;
    TASK_CHECK(LIBCAPTION_OK == vtt_cue_to_caption_frame_at(cue, 12.0, &at));
    for (int c = 0; c < 5; ++c) {
        TASK_CHECK(caption_phase_past == caption_frame_read_phase(&at, 0, c));
    }

    // identical layout at both times
    for (int c = 0; c < 5; ++c) {
        TASK_CHECK(0 == strcmp(early.front.cell[0][c].data, at.front.cell[0][c].data));
    }

    // outside the half-open interval: empty screen, all NONE
    caption_frame_t before;
    TASK_CHECK(LIBCAPTION_OK == vtt_cue_to_caption_frame_at(cue, 9.999, &before));
    TASK_CHECK(before.front.cell[0][0].data[0] == 0);
    TASK_CHECK(caption_phase_none == caption_frame_read_phase(&before, 0, 0));
    caption_frame_t after;
    TASK_CHECK(LIBCAPTION_OK == vtt_cue_to_caption_frame_at(cue, 14.0, &after));
    TASK_CHECK(after.front.cell[0][0].data[0] == 0);
    TASK_CHECK(caption_phase_none == caption_frame_read_phase(&after, 0, 0));

    vtt_free(vtt);
    return 0;
}

static int phases_final_conversion_is_none(void)
{
    vtt_t* vtt = parse_cue_ms(10000, 4000, "<i>A&amp;B</i><00:00:12.000><u>CD</u>");
    TASK_CHECK(vtt && vtt->cue_head);

    caption_frame_t frame;
    TASK_CHECK(0 == vtt_cue_to_caption_frame(vtt->cue_head, &frame));
    char text[CAPTION_FRAME_TEXT_BYTES];
    caption_frame_to_text(&frame, text);
    TASK_CHECK(0 == strcmp(text, "A&BCD"));

    for (int c = 0; c < 5; ++c) {
        TASK_CHECK(caption_phase_none == caption_frame_read_phase(&frame, 0, c));
    }

    TASK_CHECK(eia608_style_italics == frame.front.cell[0][0].sty);
    TASK_CHECK(frame.front.cell[0][3].uln && !frame.front.cell[0][0].uln);
    vtt_free(vtt);
    return 0;
}

static int phases_plain_608_and_clear_are_none(void)
{
    caption_frame_t frame;
    caption_frame_from_text(&frame, "Hello");
    for (int c = 0; c < 5; ++c) {
        TASK_CHECK(caption_phase_none == caption_frame_read_phase(&frame, 0, c));
    }

    // genuine 608 decode path: load, text, end of caption
    caption_frame_init(&frame);
    caption_frame_decode(&frame, eia608_control_command(eia608_control_resume_caption_loading, 0), 0.0);
    caption_frame_decode(&frame, eia608_from_utf8_1("A", 0), 0.0);
    caption_frame_decode(&frame, eia608_control_command(eia608_control_end_of_caption, 0), 0.0);
    TASK_CHECK(frame.front.cell[14][0].data[0] == 'A');
    TASK_CHECK(caption_phase_none == caption_frame_read_phase(&frame, 14, 0));

    // clear screen: every cell empty and NONE
    caption_frame_decode(&frame, eia608_control_command(eia608_control_erase_display_memory, 0), 1.0);
    TASK_CHECK(frame.front.cell[14][0].data[0] == 0);
    for (int r = 0; r < SCREEN_ROWS; ++r) {
        for (int c = 0; c < SCREEN_COLS; ++c) {
            TASK_CHECK(caption_phase_none == caption_frame_read_phase(&frame, r, c));
        }
    }
    return 0;
}

static int phases_layout_unicode_and_wrap(void)
{
    // one legal Unicode codepoint occupies one cell, regardless of bytes or
    // EIA-608 encodability
    vtt_t* vu = parse_cue_ms(0, 10000, "\xC3\xA9\xE4\xB8\xAD");
    TASK_CHECK(vu && vu->cue_head);
    caption_frame_t uf;
    TASK_CHECK(LIBCAPTION_OK == vtt_cue_to_caption_frame_at(vu->cue_head, 5.0, &uf));
    TASK_CHECK((unsigned char)uf.front.cell[0][0].data[0] == 0xC3);
    TASK_CHECK((unsigned char)uf.front.cell[0][1].data[0] == 0xE4);
    TASK_CHECK(uf.front.cell[0][2].data[0] == 0);
    vtt_free(vu);

    // future text must not change wrapping: 30 A's, a space, marker, then B's
    char payload[128];
    memset(payload, 'A', 30);
    strcpy(payload + 30, " <00:00:05.000>BBBB");
    vtt_t* vw = parse_cue_ms(0, 10000, payload);
    TASK_CHECK(vw && vw->cue_head);

    caption_frame_t early;
    TASK_CHECK(LIBCAPTION_OK == vtt_cue_to_caption_frame_at(vw->cue_head, 1.0, &early));
    TASK_CHECK(early.front.cell[0][29].data[0] == 'A');
    TASK_CHECK(caption_phase_past == caption_frame_read_phase(&early, 0, 29));
    TASK_CHECK(early.front.cell[1][0].data[0] == 'B');
    TASK_CHECK(caption_phase_future == caption_frame_read_phase(&early, 1, 0));

    caption_frame_t final;
    TASK_CHECK(0 == vtt_cue_to_caption_frame(vw->cue_head, &final));
    TASK_CHECK(final.front.cell[1][0].data[0] == 'B');
    TASK_CHECK(0 == strcmp(early.front.cell[1][0].data, final.front.cell[1][0].data));

    // whole-frame guarantee: preview and final layouts are cell-identical in
    // text and style at every position; only the phase bits may differ
    for (int r = 0; r < SCREEN_ROWS; ++r) {
        for (int c = 0; c < SCREEN_COLS; ++c) {
            TASK_CHECK(0 == strcmp(early.front.cell[r][c].data, final.front.cell[r][c].data));
            TASK_CHECK(early.front.cell[r][c].sty == final.front.cell[r][c].sty);
            TASK_CHECK(early.front.cell[r][c].uln == final.front.cell[r][c].uln);
            TASK_CHECK(caption_phase_none == caption_frame_read_phase(&final, r, c));
        }
    }
    vtt_free(vw);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// group: retime_export
static int retime_affine_and_marker_edits(void)
{
    vtt_t* vtt = parse_cue_ms(10000, 4000, "<i>A&amp;B</i><00:00:12.000><u>CD</u>");
    TASK_CHECK(vtt && vtt->cue_head);
    vtt_block_t* cue = vtt->cue_head;

    TASK_CHECK(LIBCAPTION_OK == vtt_cue_retime(cue, 20.0, 8.0));
    TASK_CHECK(fabs(cue->timestamp - 20.0) < 0.001);
    TASK_CHECK(fabs(cue->duration - 8.0) < 0.001);
    const vtt_text_token_t* marker = find_first(cue, VTT_TOKEN_TIME);
    TASK_CHECK(marker && 24000 == marker->time_ms);

    caption_frame_t f;
    TASK_CHECK(LIBCAPTION_OK == vtt_cue_to_caption_frame_at(cue, 23.999, &f));
    TASK_CHECK(caption_phase_future == caption_frame_read_phase(&f, 0, 3));
    TASK_CHECK(LIBCAPTION_OK == vtt_cue_to_caption_frame_at(cue, 24.0, &f));
    TASK_CHECK(caption_phase_past == caption_frame_read_phase(&f, 0, 3));

    // marker edits
    size_t midx = 0;
    while (VTT_TOKEN_TIME != vtt_cue_token(cue, midx)->type) {
        ++midx;
    }
    TASK_CHECK(LIBCAPTION_OK == vtt_cue_token_set_time(cue, midx, 25.0));
    TASK_CHECK(25000 == vtt_cue_token(cue, midx)->time_ms);
    TASK_CHECK(LIBCAPTION_ERROR == vtt_cue_token_set_time(cue, midx, 20.0)); // == start
    TASK_CHECK(LIBCAPTION_ERROR == vtt_cue_token_set_time(cue, midx, 28.0)); // == end
    TASK_CHECK(LIBCAPTION_ERROR == vtt_cue_token_set_time(cue, midx, INFINITY));
    TASK_CHECK(25000 == vtt_cue_token(cue, midx)->time_ms); // unchanged

    // retime rejects bad input
    TASK_CHECK(LIBCAPTION_ERROR == vtt_cue_retime(cue, 0.0, 0.0));
    TASK_CHECK(LIBCAPTION_ERROR == vtt_cue_retime(cue, 0.0, -1.0));
    TASK_CHECK(LIBCAPTION_ERROR == vtt_cue_retime(cue, NAN, 1.0));
    TASK_CHECK(fabs(cue->timestamp - 20.0) < 0.001); // untouched
    vtt_free(vtt);
    return 0;
}

static int retime_collision_rejected(void)
{
    vtt_t* vtt = parse_cue_ms(0, 10000, "<00:00:04.000>a<00:00:05.000>b");
    TASK_CHECK(vtt && vtt->cue_head);
    vtt_block_t* cue = vtt->cue_head;
    TASK_CHECK(4 == vtt_cue_token_count(cue)); // two markers and two text runs
    unsigned markers = 0;
    for (size_t i = 0; i < vtt_cue_token_count(cue); ++i) {
        if (VTT_TOKEN_TIME == vtt_cue_token(cue, i)->type) {
            ++markers;
        }
    }
    TASK_CHECK(2 == markers);

    // shrinking to one millisecond pushes the markers onto the same ms/out
    TASK_CHECK(LIBCAPTION_ERROR == vtt_cue_retime(cue, 0.0, 0.001));
    TASK_CHECK(0 == cue->timestamp);
    TASK_CHECK(fabs(cue->duration - 10.0) < 0.001);
    TASK_CHECK(4000 == find_first(cue, VTT_TOKEN_TIME)->time_ms);

    // ordering between markers is enforced
    size_t second = 0;
    for (size_t i = vtt_cue_token_count(cue); i-- > 0;) {
        if (VTT_TOKEN_TIME == vtt_cue_token(cue, i)->type) {
            second = i;
            break;
        }
    }
    TASK_CHECK(5000 == vtt_cue_token(cue, second)->time_ms);
    TASK_CHECK(LIBCAPTION_ERROR == vtt_cue_token_set_time(cue, second, 3.0));
    TASK_CHECK(5000 == vtt_cue_token(cue, second)->time_ms);
    vtt_free(vtt);
    return 0;
}

static int export_dump_reparse_and_plain_files(void)
{
    vtt_t* vtt = parse_cue_ms(10000, 4000, "<i>A&amp;B</i><00:00:12.000><u>CD</u>");
    TASK_CHECK(vtt && vtt->cue_head);
    vtt_block_t* cue = vtt->cue_head;
    TASK_CHECK(LIBCAPTION_OK == vtt_cue_token_set_text(cue, 0, "<X&"));
    TASK_CHECK(LIBCAPTION_OK == vtt_cue_retime(cue, 20.0, 8.0));

    char buf[1024];
    size_t n = vtt_cue_serialize(cue, buf, sizeof(buf));
    TASK_CHECK((size_t)-1 != n);
    TASK_CHECK(0 == strcmp(buf, "<i>&lt;X&amp;</i><00:00:24.000><u>CD</u>"));

    // vtt_dump must emit the edited document, never the stale payload
    const char* path = ".task-build/karaoke_dump.vtt";
    int saved = dup(fileno(stdout));
    FILE* redirected = freopen(path, "w", stdout);
    TASK_CHECK(redirected != NULL);
    vtt_dump(vtt);
    fflush(stdout);
    dup2(saved, fileno(stdout));
    close(saved);

    size_t file_size = 0;
    utf8_char_t* dumped = utf8_load_text_file(path, &file_size);
    TASK_CHECK(dumped && file_size);
    TASK_CHECK(strstr(dumped, "00:00:20.000 --> 00:00:28.000"));
    TASK_CHECK(strstr(dumped, "&lt;X&amp;"));
    TASK_CHECK(NULL == strstr(dumped, "A&amp;B"));

    vtt_t* reparsed = vtt_parse(dumped, file_size);
    TASK_CHECK(reparsed && reparsed->cue_head);
    vtt_block_t* rc = reparsed->cue_head;
    TASK_CHECK(fabs(rc->timestamp - 20.0) < 0.001 && fabs(rc->duration - 8.0) < 0.001);
    TASK_CHECK(0 == strcmp(vtt_cue_token(rc, 0)->text, "<X&"));
    TASK_CHECK(24000 == find_first(rc, VTT_TOKEN_TIME)->time_ms);
    vtt_free(reparsed);
    free(dumped);
    vtt_free(vtt);

    // plain WebVTT without any markup
    vtt_t* plain = parse_cue_ms(0, 2000, "just plain text");
    TASK_CHECK(plain && plain->cue_head);
    TASK_CHECK((size_t)-1 != vtt_cue_serialize(plain->cue_head, buf, sizeof(buf)));
    TASK_CHECK(0 == strcmp(buf, "just plain text"));
    caption_frame_t pf;
    TASK_CHECK(0 == vtt_cue_to_caption_frame(plain->cue_head, &pf));
    TASK_CHECK(caption_phase_none == caption_frame_read_phase(&pf, 0, 0));
    vtt_free(plain);

    // marker-free SRT still converts and serializes as plain text
    static const char srt_doc[] = "1\r\n00:00:01,000 --> 00:00:02,000\r\nHello\r\n\r\n";
    vtt_t* srt = srt_parse(srt_doc, sizeof(srt_doc) - 1);
    TASK_CHECK(srt && srt->cue_head);
    caption_frame_t sf;
    TASK_CHECK(0 == vtt_cue_to_caption_frame(srt->cue_head, &sf));
    char text[CAPTION_FRAME_TEXT_BYTES];
    caption_frame_to_text(&sf, text);
    TASK_CHECK(0 == strcmp(text, "Hello"));
    TASK_CHECK((size_t)-1 != vtt_cue_serialize(srt->cue_head, buf, sizeof(buf)));
    TASK_CHECK(0 == strcmp(buf, "Hello"));
    vtt_free(srt);
    return 0;
}

static int invalid_inline_time_is_not_text(void)
{
    // comma instead of dot, and a tag that is not a legal timestamp shape
    vtt_t* v1 = parse_cue_ms(10000, 4000, "<00:00:12,000>x");
    TASK_CHECK(v1 && v1->cue_head);
    TASK_CHECK(0 == vtt_cue_token_count(v1->cue_head));
    caption_frame_t f;
    TASK_CHECK(0 != vtt_cue_to_caption_frame(v1->cue_head, &f));
    TASK_CHECK(f.front.cell[0][0].data[0] == 0);
    // failed cue remains editable in timing and releasable
    TASK_CHECK(LIBCAPTION_OK == vtt_cue_retime(v1->cue_head, 30.0, 5.0));
    TASK_CHECK(fabs(v1->cue_head->timestamp - 30.0) < 0.001);
    vtt_free(v1);

    vtt_t* v2 = parse_cue_ms(10000, 4000, "<12.000>x");
    TASK_CHECK(v2 && 0 == vtt_cue_token_count(v2->cue_head));
    // markers outside the cue are rejected too
    vtt_t* v3 = parse_cue_ms(10000, 4000, "<00:00:14.000>x");
    TASK_CHECK(v3 && 0 == vtt_cue_token_count(v3->cue_head));
    vtt_free(v2);
    vtt_free(v3);
    return 0;
}

const struct task_case task_cases[] = {
    { "parse_edit", "basic_tokens", parse_basic_tokens },
    { "parse_edit", "entities_nesting_newline", parse_entities_nesting_newline },
    { "parse_edit", "edit_text_escapes_and_reparse", edit_text_escapes_and_reparse },
    { "phases", "past_future_and_bounds", phases_past_future_and_bounds },
    { "phases", "final_conversion_is_none", phases_final_conversion_is_none },
    { "phases", "plain_608_and_clear_are_none", phases_plain_608_and_clear_are_none },
    { "phases", "layout_unicode_and_wrap", phases_layout_unicode_and_wrap },
    { "retime_export", "affine_and_marker_edits", retime_affine_and_marker_edits },
    { "retime_export", "collision_rejected", retime_collision_rejected },
    { "retime_export", "export_dump_reparse_and_plain", export_dump_reparse_and_plain_files },
    { "retime_export", "invalid_inline_time", invalid_inline_time_is_not_text },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
