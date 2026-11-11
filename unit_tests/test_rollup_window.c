/**********************************************************************************************/
/* The MIT License                                                                            */
/*                                                                                            */
/* Copyright 2016-2017 Twitch Interactive, Inc. or its affiliates. All Rights Reserved.       */
/*                                                                                            */
/* Permission is hereby granted, free of charge, to any person obtaining a copy               */
/* of this software and associated documentation files (the "Software"), to deal              */
/* in the Software without restriction, including without limitation the rights               */
/* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell                  */
/* copies of the Software, and to permit persons to whom the Software is                      */
/* furnished to do so, subject to the following conditions:                                   */
/*                                                                                            */
/* The above copyright notice and this permission notice shall be included in                 */
/* all copies or substantial portions of the Software.                                        */
/*                                                                                            */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR                 */
/* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,                   */
/* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE                */
/* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER                     */
/* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,              */
/* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN                  */
/* THE SOFTWARE.                                                                              */
/**********************************************************************************************/
#include "caption/caption.h"
#include "cea708.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

// A scenario records every cc word it feeds to caption_frame_decode so the
// exact same control sequence can be replayed through the CEA708 wrapper.
typedef struct {
    uint16_t words[256];
    int n;
} seq_t;

static void seq_send(seq_t* s, caption_frame_t* f, uint16_t w)
{
    s->words[s->n++] = w;
    caption_frame_decode(f, w, 1.0);
}

static void ctrl(seq_t* s, caption_frame_t* f, eia608_control_t cmd)
{
    seq_send(s, f, eia608_control_command(cmd, 0));
}

static void pac_rc(seq_t* s, caption_frame_t* f, int row, int col, int uln)
{
    seq_send(s, f, eia608_row_column_pramble(row, col, 0, uln));
}

static void pac_rs(seq_t* s, caption_frame_t* f, int row, eia608_style_t sty, int uln)
{
    seq_send(s, f, eia608_row_style_pramble(row, 0, sty, uln));
}

static void text2(seq_t* s, caption_frame_t* f, const char* a, const char* b)
{
    seq_send(s, f, eia608_from_utf8_2(a, b));
}

static void text1(seq_t* s, caption_frame_t* f, const char* a)
{
    seq_send(s, f, eia608_from_utf8_1(a, 0));
}

static void check_cell(caption_frame_t* f, int row, int col, const char* expect,
    eia608_style_t sty, int uln, const char* what)
{
    eia608_style_t got_sty;
    int got_uln;
    const utf8_char_t* got = caption_frame_read_char(f, row, col, &got_sty, &got_uln);

    if (strcmp(got, expect) || got_sty != sty || got_uln != uln) {
        printf("FAIL %s: row %d col %d: got '%s' style %d uln %d, want '%s' style %d uln %d\n",
            what, row, col, got, got_sty, got_uln, expect, sty, uln);
        ++failures;
    }
}

static void check_empty(caption_frame_t* f, int row, const char* what)
{
    for (int c = 0; c < SCREEN_COLS; ++c) {
        eia608_style_t sty;
        int uln;
        const utf8_char_t* got = caption_frame_read_char(f, row, c, &sty, &uln);

        if (0 != got[0] || eia608_style_white != sty || 0 != uln) {
            printf("FAIL %s: row %d col %d expected empty, got '%s' style %d uln %d\n",
                what, row, c, got, sty, uln);
            ++failures;
            return;
        }
    }
}

// Full 15x32 cell by cell comparison; reports the first row/column that differs.
static void compare_frames(caption_frame_t* direct, caption_frame_t* wrapped, const char* what)
{
    for (int r = 0; r < SCREEN_ROWS; ++r) {
        for (int c = 0; c < SCREEN_COLS; ++c) {
            eia608_style_t sd, sw;
            int ud, uw;
            const utf8_char_t* cd = caption_frame_read_char(direct, r, c, &sd, &ud);
            const utf8_char_t* cw = caption_frame_read_char(wrapped, r, c, &sw, &uw);

            if (strcmp(cd, cw) || sd != sw || ud != uw) {
                printf("FAIL %s: first mismatch at row %d col %d: direct '%s' style %d uln %d, "
                       "708 '%s' style %d uln %d\n",
                    what, r, c, cd, sd, ud, cw, sw, uw);
                ++failures;
                return;
            }
        }
    }
}

// Replay the recorded sequence as valid field-1 CEA708 cc_data, split across
// as many user data messages as the 31 entry per packet limit requires.
static void replay_through_708(caption_frame_t* f, const seq_t* s)
{
    int i = 0;

    while (i < s->n) {
        cea708_t cea;
        cea708_init(&cea, 1.0);
        int chunk = s->n - i;

        if (31 < chunk) {
            chunk = 31;
        }

        for (int k = 0; k < chunk; ++k) {
            cea708_add_cc_data(&cea, 1, cc_type_ntsc_cc_field_1, s->words[i + k]);
        }

        cea708_to_caption_frame(f, &cea);
        i += chunk;
    }
}

static void run_scenario(const char* name, void (*scenario)(seq_t*, caption_frame_t*))
{
    seq_t seq = { .n = 0 };
    caption_frame_t direct, wrapped;
    caption_frame_init(&direct);
    caption_frame_init(&wrapped);
    scenario(&seq, &direct);
    replay_through_708(&wrapped, &seq);
    compare_frames(&direct, &wrapped, name);
    printf("[%s] direct and CEA708 paths agree across %d cc words\n", name, seq.n);
}

// 1. Mid screen baseline: the window scrolls, rows outside the window stay put.
static void scenario_mid_baseline(seq_t* s, caption_frame_t* f)
{
    ctrl(s, f, eia608_control_resume_direct_captioning);
    pac_rc(s, f, 10, 0, 0);
    text2(s, f, "Z", "Z");
    ctrl(s, f, eia608_control_roll_up_2);
    pac_rs(s, f, 4, eia608_style_green, 1);
    text2(s, f, "A", "B");
    ctrl(s, f, eia608_control_carriage_return);

    check_cell(f, 3, 0, "A", eia608_style_green, 1, "mid_baseline");
    check_cell(f, 3, 1, "B", eia608_style_green, 1, "mid_baseline");
    check_empty(f, 4, "mid_baseline baseline cleared");
    check_cell(f, 10, 0, "Z", eia608_style_white, 0, "mid_baseline outside");
    check_cell(f, 10, 1, "Z", eia608_style_white, 0, "mid_baseline outside");
    check_empty(f, 9, "mid_baseline row above outside text");

    // a later PAC keeps its column offset and does not relocate a same-row window
    pac_rc(s, f, 4, 8, 0);
    text2(s, f, "E", "F");
    check_cell(f, 4, 8, "E", eia608_style_white, 0, "mid_baseline PAC column");
    check_cell(f, 4, 9, "F", eia608_style_white, 0, "mid_baseline PAC column");
    check_cell(f, 3, 0, "A", eia608_style_green, 1, "mid_baseline same-row PAC no scroll");
}

// 2. Window clipped by the top of the screen: CR keeps rolling the visible rows.
static void scenario_top_clip(seq_t* s, caption_frame_t* f)
{
    ctrl(s, f, eia608_control_resume_direct_captioning);
    pac_rc(s, f, 5, 0, 0);
    text2(s, f, "Z", "Z");
    ctrl(s, f, eia608_control_roll_up_4);
    pac_rc(s, f, 1, 0, 0); // window is now the visible rows 0..1
    text2(s, f, "A", "B");
    ctrl(s, f, eia608_control_carriage_return);
    check_cell(f, 0, 0, "A", eia608_style_white, 0, "top_clip first CR");
    check_cell(f, 0, 1, "B", eia608_style_white, 0, "top_clip first CR");
    check_empty(f, 1, "top_clip baseline cleared");

    text2(s, f, "C", "D");
    ctrl(s, f, eia608_control_carriage_return);
    check_cell(f, 0, 0, "C", eia608_style_white, 0, "top_clip second CR");
    check_cell(f, 0, 1, "D", eia608_style_white, 0, "top_clip second CR");
    check_empty(f, 1, "top_clip baseline cleared");

    text2(s, f, "E", "F");
    ctrl(s, f, eia608_control_carriage_return);
    check_cell(f, 0, 0, "E", eia608_style_white, 0, "top_clip third CR");
    check_cell(f, 0, 1, "F", eia608_style_white, 0, "top_clip third CR");

    // single visible row: CR still runs and discards the baseline
    pac_rc(s, f, 0, 0, 0);
    text2(s, f, "G", "H");
    ctrl(s, f, eia608_control_carriage_return);
    check_empty(f, 0, "top_clip single row CR");

    check_cell(f, 5, 0, "Z", eia608_style_white, 0, "top_clip outside");
    check_cell(f, 5, 1, "Z", eia608_style_white, 0, "top_clip outside");
    check_empty(f, 2, "top_clip below window");
}

// 3. PAC relocation in both directions, styles traveling with the cells.
static void scenario_pac_relocate(seq_t* s, caption_frame_t* f)
{
    ctrl(s, f, eia608_control_resume_direct_captioning);
    pac_rc(s, f, 2, 0, 0);
    text2(s, f, "M", "M"); // permanent marker outside every window
    ctrl(s, f, eia608_control_roll_up_3);
    pac_rc(s, f, 8, 0, 0); // window rows 6..8
    text2(s, f, "C", "C");
    ctrl(s, f, eia608_control_carriage_return);
    text2(s, f, "B", "B");
    ctrl(s, f, eia608_control_carriage_return);
    pac_rs(s, f, 8, eia608_style_green, 1); // same row: style only, no move
    text2(s, f, "A", "A");
    check_cell(f, 6, 0, "C", eia608_style_white, 0, "relocate built window");
    check_cell(f, 7, 0, "B", eia608_style_white, 0, "relocate built window");
    check_cell(f, 8, 0, "A", eia608_style_green, 1, "relocate built window");

    pac_rc(s, f, 11, 0, 0); // move down by 3: window rows 9..11
    check_empty(f, 6, "relocate down old top vacated");
    check_empty(f, 7, "relocate down old middle vacated");
    check_empty(f, 8, "relocate down old baseline vacated");
    check_cell(f, 9, 0, "C", eia608_style_white, 0, "relocate down");
    check_cell(f, 10, 0, "B", eia608_style_white, 0, "relocate down");
    check_cell(f, 11, 0, "A", eia608_style_green, 1, "relocate down styled");

    pac_rc(s, f, 5, 0, 0); // move up by 6: window rows 3..5
    check_empty(f, 9, "relocate up old top vacated");
    check_empty(f, 10, "relocate up old middle vacated");
    check_empty(f, 11, "relocate up old baseline vacated");
    check_cell(f, 3, 0, "C", eia608_style_white, 0, "relocate up");
    check_cell(f, 4, 0, "B", eia608_style_white, 0, "relocate up");
    check_cell(f, 5, 0, "A", eia608_style_green, 1, "relocate up styled");

    check_cell(f, 2, 0, "M", eia608_style_white, 0, "relocate outside marker");
    check_cell(f, 2, 1, "M", eia608_style_white, 0, "relocate outside marker");
}

// 4. Styled text keeps color and underline through CR and PAC moves.
static void scenario_styles(seq_t* s, caption_frame_t* f)
{
    ctrl(s, f, eia608_control_resume_direct_captioning);
    ctrl(s, f, eia608_control_roll_up_3);
    pac_rs(s, f, 10, eia608_style_red, 1);
    text2(s, f, "R", "S");
    ctrl(s, f, eia608_control_carriage_return);
    check_cell(f, 9, 0, "R", eia608_style_red, 1, "styles CR");
    check_cell(f, 9, 1, "S", eia608_style_red, 1, "styles CR");
    check_empty(f, 10, "styles baseline cleared");

    pac_rs(s, f, 12, eia608_style_cyan, 0); // relocate down by 2
    check_cell(f, 11, 0, "R", eia608_style_red, 1, "styles PAC");
    check_cell(f, 11, 1, "S", eia608_style_red, 1, "styles PAC");
    text1(s, f, "T");
    check_cell(f, 12, 0, "T", eia608_style_cyan, 0, "styles PAC new style");
}

// 5. Window grows then shrinks around a fixed baseline.
static void scenario_resize(seq_t* s, caption_frame_t* f)
{
    ctrl(s, f, eia608_control_resume_direct_captioning);
    pac_rc(s, f, 3, 0, 0);
    text2(s, f, "Z", "Z");
    ctrl(s, f, eia608_control_roll_up_2);
    pac_rc(s, f, 10, 0, 0); // window rows 9..10
    text2(s, f, "A", "A");
    ctrl(s, f, eia608_control_carriage_return);
    text2(s, f, "B", "B");
    check_cell(f, 9, 0, "A", eia608_style_white, 0, "resize RU2 content");
    check_cell(f, 10, 0, "B", eia608_style_white, 0, "resize RU2 content");

    ctrl(s, f, eia608_control_roll_up_4); // grow to rows 7..10
    check_empty(f, 7, "resize grown rows empty");
    check_empty(f, 8, "resize grown rows empty");
    check_cell(f, 9, 0, "A", eia608_style_white, 0, "resize kept after grow");
    check_cell(f, 10, 0, "B", eia608_style_white, 0, "resize kept after grow");

    ctrl(s, f, eia608_control_roll_up_2); // shrink back to rows 9..10
    check_empty(f, 7, "resize shrunk rows cleared");
    check_empty(f, 8, "resize shrunk rows cleared");
    check_cell(f, 9, 0, "A", eia608_style_white, 0, "resize kept after shrink");
    check_cell(f, 10, 0, "B", eia608_style_white, 0, "resize kept after shrink");
    check_cell(f, 3, 0, "Z", eia608_style_white, 0, "resize outside marker");
}

// 6. Back to back duplicate RU/CR run once; a CR after ordinary text still runs.
static void scenario_duplicates(seq_t* s, caption_frame_t* f)
{
    ctrl(s, f, eia608_control_resume_direct_captioning);
    ctrl(s, f, eia608_control_roll_up_2);
    text2(s, f, "A", "A");
    ctrl(s, f, eia608_control_carriage_return);
    ctrl(s, f, eia608_control_carriage_return); // duplicate: must be ignored
    check_cell(f, 13, 0, "A", eia608_style_white, 0, "duplicate CR ignored");
    check_empty(f, 14, "duplicate CR baseline still empty");

    text2(s, f, "B", "B");
    ctrl(s, f, eia608_control_carriage_return); // text in between: must run
    check_cell(f, 13, 0, "B", eia608_style_white, 0, "CR after text runs");
    check_empty(f, 14, "CR after text clears baseline");

    ctrl(s, f, eia608_control_roll_up_2); // duplicate RU: window contents survive
    ctrl(s, f, eia608_control_roll_up_2);
    check_cell(f, 13, 0, "B", eia608_style_white, 0, "duplicate RU ignored");
}

// 7. Entering roll-up from paint-on/pop-on opens an empty window at the first
//    roll-up PAC and keeps everything else, including unseen non-displayed
//    buffer content, in place.
static void scenario_mode_entry(seq_t* s, caption_frame_t* f)
{
    ctrl(s, f, eia608_control_resume_direct_captioning);
    pac_rc(s, f, 10, 0, 0);
    text2(s, f, "Z", "Z"); // visible paint-on content

    ctrl(s, f, eia608_control_resume_caption_loading); // pop-on: write hidden text
    pac_rc(s, f, 5, 0, 0);
    text2(s, f, "Q", "Q");

    ctrl(s, f, eia608_control_roll_up_2); // roll-up mode only, no window yet
    pac_rc(s, f, 14, 0, 0); // first roll-up PAC: empty window at rows 13..14
    check_cell(f, 10, 0, "Z", eia608_style_white, 0, "mode entry displayed content kept");
    check_empty(f, 13, "mode entry new window empty");
    check_empty(f, 14, "mode entry new window empty");
    check_empty(f, 5, "mode entry hidden buffer not pulled in");

    text2(s, f, "R", "R");
    ctrl(s, f, eia608_control_carriage_return);
    check_cell(f, 13, 0, "R", eia608_style_white, 0, "mode entry rollup works");
    check_cell(f, 10, 0, "Z", eia608_style_white, 0, "mode entry outside survives CR");
    check_empty(f, 5, "mode entry hidden buffer still not shown");
}

// 8. Paint-on direct writing and pop-on background writing plus EOC never scroll.
static void scenario_other_modes(seq_t* s, caption_frame_t* f)
{
    ctrl(s, f, eia608_control_resume_direct_captioning);
    pac_rc(s, f, 4, 0, 0);
    text2(s, f, "H", "H");
    check_cell(f, 4, 0, "H", eia608_style_white, 0, "paint-on direct write");

    ctrl(s, f, eia608_control_resume_caption_loading);
    ctrl(s, f, eia608_control_erase_non_displayed_memory);
    pac_rc(s, f, 6, 0, 0);
    text2(s, f, "P", "P");
    check_empty(f, 6, "pop-on stays hidden until EOC");
    ctrl(s, f, eia608_control_end_of_caption);
    check_cell(f, 6, 0, "P", eia608_style_white, 0, "pop-on EOC publish");
    check_empty(f, 5, "pop-on EOC no window scrolling above");
    check_empty(f, 7, "pop-on EOC no window scrolling below");
}

// Invalid entries and field-2 entries are ignored by the CEA708 wrapper: the
// resulting frame must match a frame fed only the valid field-1 subset.
static void scenario_708_filtering(void)
{
    seq_t seq = { .n = 0 };
    caption_frame_t direct, wrapped;
    caption_frame_init(&direct);
    caption_frame_init(&wrapped);

    // valid field-1 sequence, recorded in seq
    ctrl(&seq, &direct, eia608_control_resume_direct_captioning);
    ctrl(&seq, &direct, eia608_control_roll_up_2);
    pac_rc(&seq, &direct, 8, 0, 0);
    text2(&seq, &direct, "A", "B");
    ctrl(&seq, &direct, eia608_control_carriage_return);
    ctrl(&seq, &direct, eia608_control_carriage_return); // duplicate stays ignored

    cea708_t cea;
    cea708_init(&cea, 1.0);
    // noise that must never reach caption_frame_decode
    cea708_add_cc_data(&cea, 0, cc_type_ntsc_cc_field_1,
        eia608_control_command(eia608_control_roll_up_4, 0)); // invalid entry
    cea708_add_cc_data(&cea, 1, cc_type_ntsc_cc_field_2,
        eia608_control_command(eia608_control_roll_up_3, 0)); // field 2
    cea708_add_cc_data(&cea, 1, cc_type_ntsc_cc_field_1, seq.words[0]);
    cea708_add_cc_data(&cea, 0, cc_type_ntsc_cc_field_1,
        eia608_control_command(eia608_control_carriage_return, 0)); // invalid entry
    cea708_add_cc_data(&cea, 1, cc_type_ntsc_cc_field_2, seq.words[2]); // field 2

    for (int i = 1; i < seq.n; ++i) {
        cea708_add_cc_data(&cea, 1, cc_type_ntsc_cc_field_1, seq.words[i]);
    }

    cea708_to_caption_frame(&wrapped, &cea);
    compare_frames(&direct, &wrapped, "708 filtering");
}

// 9. Rows of a relocated window moving past the top screen edge are discarded;
//    rows the old window vacates are cleared.
static void scenario_offscreen(seq_t* s, caption_frame_t* f)
{
    ctrl(s, f, eia608_control_resume_direct_captioning);
    ctrl(s, f, eia608_control_roll_up_3);
    pac_rc(s, f, 2, 0, 0); // window rows 0..2
    text2(s, f, "T", "T"); // row 2
    ctrl(s, f, eia608_control_carriage_return);
    text2(s, f, "M", "M"); // row 2 again, T moves to row 1
    ctrl(s, f, eia608_control_carriage_return);
    text2(s, f, "B", "B"); // rows now: T on 0, M on 1, B on 2
    check_cell(f, 0, 0, "T", eia608_style_white, 0, "offscreen built window");
    check_cell(f, 1, 0, "M", eia608_style_white, 0, "offscreen built window");
    check_cell(f, 2, 0, "B", eia608_style_white, 0, "offscreen built window");

    pac_rc(s, f, 14, 0, 0); // move down by 12, the screen bottom keeps every row
    check_cell(f, 12, 0, "T", eia608_style_white, 0, "offscreen moved down");
    check_cell(f, 13, 0, "M", eia608_style_white, 0, "offscreen moved down");
    check_cell(f, 14, 0, "B", eia608_style_white, 0, "offscreen moved down");
    check_empty(f, 2, "offscreen old window vacated");

    pac_rc(s, f, 0, 0, 0); // move up by 14: T and M leave the top edge, only B lands
    check_cell(f, 0, 0, "B", eia608_style_white, 0, "offscreen surviving row");
    check_empty(f, 1, "offscreen discarded rows stay empty");
    check_empty(f, 2, "offscreen discarded rows stay empty");
    check_empty(f, 14, "offscreen old baseline vacated");
}

int main(void)
{
    run_scenario("mid_baseline", scenario_mid_baseline);
    run_scenario("top_clip", scenario_top_clip);
    run_scenario("pac_relocate", scenario_pac_relocate);
    run_scenario("styles", scenario_styles);
    run_scenario("resize", scenario_resize);
    run_scenario("duplicates", scenario_duplicates);
    run_scenario("mode_entry", scenario_mode_entry);
    run_scenario("other_modes", scenario_other_modes);
    run_scenario("offscreen", scenario_offscreen);
    scenario_708_filtering();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }

    printf("all roll-up window checks passed\n");
    return 0;
}
