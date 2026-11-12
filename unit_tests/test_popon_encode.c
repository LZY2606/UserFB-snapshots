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
#include "caption/mpeg.h"
#include "verification/driver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                                                       \
    do {                                                                                                  \
        if (!(cond)) {                                                                                    \
            fprintf(stderr, "CHECK failed (%s) at %s:%d\n", #cond, __FILE__, __LINE__);                  \
            return 0;                                                                                     \
        }                                                                                                 \
    } while (0)

static void frame_reset(caption_frame_t* frame, double timestamp)
{
    caption_frame_init(frame);
    frame->timestamp = timestamp;
    frame->write = &frame->front;
}

static void frame_put(caption_frame_t* frame, int row, int col, eia608_style_t sty, int uln, const char* ch)
{
    if (!caption_frame_write_char(frame, row, col, sty, uln, ch)) {
        fprintf(stderr, "caption_frame_write_char failed row=%d col=%d ch='%s'\n", row, col, ch);
        abort();
    }
}

// Prints the first cell whose character, presence, style or underline differs.
static int grids_equal(const caption_frame_t* want, const caption_frame_t* got, const char* tag)
{
    int r, c;

    for (r = 0; r < SCREEN_ROWS; ++r) {
        for (c = 0; c < SCREEN_COLS; ++c) {
            eia608_style_t sw, sg;
            int uw, ug;
            const char* cw = caption_frame_read_char((caption_frame_t*)want, r, c, &sw, &uw);
            const char* cg = caption_frame_read_char((caption_frame_t*)got, r, c, &sg, &ug);

            if (0 != strcmp(cw, cg) || sw != sg || uw != ug) {
                fprintf(stderr,
                    "%s first mismatch at row=%d col=%d: want ch='%s' style=%d uln=%d, got ch='%s' style=%d uln=%d\n",
                    tag, r, c, cw, (int)sw, uw, cg, (int)sg, ug);
                return 0;
            }
        }
    }

    return 1;
}

// Full export path: sei_from_caption_frame -> sei_render -> sei_parse -> sei_to_caption_frame
static int export_import(const caption_frame_t* src, caption_frame_t* dst, sei_t* sei, sei_t* parsed)
{
    size_t size, rendered;
    uint8_t* buf;

    if (LIBCAPTION_OK != sei_from_caption_frame(sei, (caption_frame_t*)src)) {
        return 0;
    }

    size = sei_render_size(sei);
    buf = (uint8_t*)malloc(size);
    rendered = sei_render(sei, buf);

    if (0 == rendered) {
        free(buf);
        return 0;
    }

    // sei_parse takes the SEI payload after the NAL unit header byte, while
    // the standalone byte interpreter below consumes the whole NALU itself.
    if (LIBCAPTION_OK != sei_parse(parsed, buf + 1, rendered - 1, src->timestamp)) {
        free(buf);
        return 0;
    }

    free(buf);
    caption_frame_init(dst);

    if (LIBCAPTION_READY != sei_to_caption_frame(parsed, dst)) {
        return 0;
    }

    return 1;
}

////////////////////////////////////////////////////////////////////////////////
// Minimal independent byte interpreter. It walks the rendered SEI NALU bytes
// itself (including emulation prevention removal), extracts every field 1
// cc_data word and reconstructs the 15x32 pop-on screen straight from the
// CEA-608 control words, without going through sei_to_caption_frame.
typedef struct {
    char data[5];
    eia608_style_t sty;
    int uln;
    int used;
} wire_cell_t;

typedef struct {
    wire_cell_t cell[SCREEN_ROWS][SCREEN_COLS];
} wire_screen_t;

static void wire_cell_write(wire_screen_t* screen, int row, int col, eia608_style_t sty, int uln, const char* ch)
{
    wire_cell_t* cell;

    if (0 > row || SCREEN_ROWS <= row || 0 > col || SCREEN_COLS <= col) {
        return;
    }

    cell = &screen->cell[row][col];
    cell->used = 1;
    cell->sty = sty;
    cell->uln = uln;
    strcpy(cell->data, ch);
}

static void wire_screen_clear(wire_screen_t* screen) { memset(screen, 0, sizeof(wire_screen_t)); }

static int wire_collect_words(const uint8_t* nalu, size_t size, uint16_t* words, size_t max_words, size_t* word_count)
{
    const uint8_t* p = nalu;
    *word_count = 0;

    if (1 > size || 0x06 != *p) {
        return 0;
    }

    ++p, --size;

    while (1 < size) {
        size_t payload_type = 0, payload_size = 0, i;
        uint8_t payload[CEA608_MAX_SIZE + 16];
        const uint8_t* q;
        size_t out = 0;

        while (0 < size && 255 == *p) {
            payload_type += 255;
            ++p, --size;
        }

        if (0 == size) {
            return 0;
        }

        payload_type += *p;
        ++p, --size;

        while (0 < size && 255 == *p) {
            payload_size += 255;
            ++p, --size;
        }

        if (0 == size || payload_size > sizeof(payload) || payload_size > size) {
            return 0;
        }

        payload_size += *p;
        ++p, --size;

        // Remove RBSP emulation prevention bytes (0x00 0x00 0x03).
        for (i = 0; i < payload_size; ++i) {
            if (i + 2 < payload_size && 0 == p[i] && 0 == p[i + 1] && 3 == p[i + 2]) {
                payload[out++] = p[i];
                payload[out++] = p[i + 1];
                i += 2;
            } else {
                payload[out++] = p[i];
            }
        }

        p += payload_size;
        size -= payload_size;

        if (sei_type_user_data_registered_itu_t_t35 != payload_type || 10 > out) {
            continue;
        }

        // country(1) provider(2) GA94(4) user_data_type_code(1) cc_header(1) em(1)
        uint32_t ident = ((uint32_t)payload[3] << 24) | ((uint32_t)payload[4] << 16) | ((uint32_t)payload[5] << 8) | payload[6];

        if (GA94 != ident || 3 != payload[7]) {
            continue;
        }

        int cc_count = payload[8] & 0x1F;
        q = payload + 10;
        out -= 10;

        for (i = 0; i < (size_t)cc_count && 3 <= out; ++i, q += 3, out -= 3) {
            int valid = (q[0] >> 2) & 1;
            int type = q[0] & 3;
            uint16_t cc_data = ((uint16_t)q[1] << 8) | q[2];

            if (valid && cc_type_ntsc_cc_field_1 == type) {
                if (*word_count >= max_words) {
                    return 0;
                }

                words[(*word_count)++] = cc_data;
            }
        }
    }

    return 1;
}

static int wire_interpret(const uint16_t* words, size_t word_count, wire_screen_t* front_out, int* eoc_swaps)
{
    wire_screen_t back, front;
    int row = SCREEN_ROWS - 1, col = 0;
    eia608_style_t sty = eia608_style_white;
    int uln = 0;
    uint16_t last_cc = 0;
    size_t i;

    wire_screen_clear(&back);
    wire_screen_clear(&front);
    *eoc_swaps = 0;

    for (i = 0; i < word_count; ++i) {
        uint16_t cc_data = words[i];

        if (eia608_is_padding(cc_data)) {
            continue;
        }

        // Receivers discard two identical control/Special NA words in a row;
        // this is why a swapped EOC pair only exchanges the buffers once.
        if ((eia608_is_control(cc_data) || eia608_is_specialna(cc_data)) && cc_data == last_cc) {
            continue;
        }

        last_cc = cc_data;

        if (eia608_is_preamble(cc_data)) {
            int pr, pc, pchn, puln;
            eia608_style_t psty;
            eia608_parse_preamble(cc_data, &pr, &pc, &psty, &pchn, &puln);
            row = pr;
            col = pc;
            sty = psty;
            uln = puln;
        } else if (eia608_is_midrowchange(cc_data)) {
            int mchn, muln;
            eia608_style_t msty;
            eia608_parse_midrowchange(cc_data, &mchn, &msty, &muln);
            sty = msty;
            uln = muln;
            // A mid-row code occupies one display position as a styled space.
            // Writing at column 31 leaves the pen one cell off screen (32).
            wire_cell_write(&back, row, col, sty, uln, EIA608_CHAR_SPACE);
            col += 1;
        } else if (eia608_is_control(cc_data)) {
            int cc;
            eia608_control_t cmd = eia608_parse_control(cc_data, &cc);

            switch (cmd) {
            case eia608_control_end_of_caption:
                memcpy(&front, &back, sizeof(front));
                wire_screen_clear(&back);
                ++(*eoc_swaps);
                break;

            case eia608_control_erase_non_displayed_memory:
                wire_screen_clear(&back);
                break;

            case eia608_control_erase_display_memory:
                wire_screen_clear(&front);
                break;

            case eia608_control_backspace:
                if (0 < col) {
                    --col;
                }
                break;

            case eia608_tab_offset_0:
            case eia608_tab_offset_1:
            case eia608_tab_offset_2:
            case eia608_tab_offset_3:
                col += cmd - eia608_tab_offset_0;
                col = (SCREEN_COLS < col) ? (SCREEN_COLS - 1) : col;
                break;

            default:
                break;
            }
        } else if (eia608_is_basicna(cc_data) || eia608_is_specialna(cc_data) || eia608_is_westeu(cc_data)) {
            int chan;
            char char1[5], char2[5];
            int chars = eia608_to_utf8(cc_data, &chan, char1, char2);

            if (eia608_is_westeu(cc_data)) {
                // Western European words replace the previous cell.
                if (0 < col) {
                    --col;
                }
            }

            if (0 < chars) {
                wire_cell_write(&back, row, col, sty, uln, char1);
                col += 1;
            }

            if (1 < chars) {
                wire_cell_write(&back, row, col, sty, uln, char2);
                col += 1;
            }
        }
    }

    memcpy(front_out, &front, sizeof(front));
    return 1;
}

// Reconstruct the screen with the byte interpreter and compare every cell.
static int wire_roundtrip_equal(const caption_frame_t* src, sei_t* sei, int* eoc_swaps)
{
    size_t size = sei_render_size(sei);
    uint8_t* buf = (uint8_t*)malloc(size);
    size_t rendered = sei_render(sei, buf);
    uint16_t words[4096];
    size_t word_count;
    wire_screen_t screen;
    int r, c;

    CHECK(0 != rendered);
    CHECK(wire_collect_words(buf, rendered, words, 4096, &word_count));
    CHECK(0 < word_count);
    free(buf);

    CHECK(wire_interpret(words, word_count, &screen, eoc_swaps));

    for (r = 0; r < SCREEN_ROWS; ++r) {
        for (c = 0; c < SCREEN_COLS; ++c) {
            eia608_style_t sw;
            int uw;
            const char* cw = caption_frame_read_char((caption_frame_t*)src, r, c, &sw, &uw);
            wire_cell_t* cell = &screen.cell[r][c];

            if (0 == cw[0]) {
                if (cell->used) {
                    fprintf(stderr, "wire first mismatch at row=%d col=%d: want hole, got ch='%s' style=%d uln=%d\n",
                        r, c, cell->data, (int)cell->sty, cell->uln);
                    return 0;
                }
            } else if (!cell->used || 0 != strcmp(cw, cell->data) || sw != cell->sty || uw != cell->uln) {
                fprintf(stderr,
                    "wire first mismatch at row=%d col=%d: want ch='%s' style=%d uln=%d, got ch='%s' style=%d uln=%d\n",
                    r, c, cw, (int)sw, uw, cell->used ? cell->data : "", (int)cell->sty, cell->uln);
                return 0;
            }
        }
    }

    return 1;
}

////////////////////////////////////////////////////////////////////////////////
// 1. Mixed style screen: mid-row style boundary, hole crossing and last column.
static int test_style_boundary(void)
{
    caption_frame_t src, dst;
    sei_t sei, parsed;
    int eoc_swaps = 0;

    frame_reset(&src, 1.0);
    // The exact screen from the bug report: white A, red underlined B next to
    // it, a two cell hole, then a green C.
    frame_put(&src, 14, 0, eia608_style_white, 0, "A");
    frame_put(&src, 14, 1, eia608_style_red, 1, "B");
    frame_put(&src, 14, 4, eia608_style_green, 0, "C");

    // Row starting in a non-white style exercises the row style preamble.
    frame_put(&src, 2, 0, eia608_style_cyan, 1, "R");
    frame_put(&src, 2, 1, eia608_style_white, 0, "S");
    frame_put(&src, 2, 2, eia608_style_yellow, 0, "T");

    // Style changes in the last columns, ending on column 31.
    frame_put(&src, 0, 28, eia608_style_white, 0, "W");
    frame_put(&src, 0, 29, eia608_style_green, 0, "G");
    frame_put(&src, 0, 30, eia608_style_magenta, 1, "Y");
    frame_put(&src, 0, 31, eia608_style_blue, 0, "Z");

    CHECK(export_import(&src, &dst, &sei, &parsed));
    CHECK(grids_equal(&src, &dst, "style-boundary"));
    CHECK(wire_roundtrip_equal(&src, &sei, &eoc_swaps));
    CHECK(1 == eoc_swaps);
    sei_free(&parsed);
    sei_free(&sei);
    return 1;
}

// 2. Explicit spaces are retained; holes stay holes, including after them.
static int test_sparse_grid(void)
{
    caption_frame_t src, dst;
    sei_t sei, parsed;
    eia608_style_t sty;
    int uln, eoc_swaps = 0;

    frame_reset(&src, 2.0);
    frame_put(&src, 5, 2, eia608_style_white, 0, EIA608_CHAR_SPACE);
    frame_put(&src, 5, 3, eia608_style_white, 0, "A");
    // columns 4-6 deliberately left as holes
    frame_put(&src, 5, 7, eia608_style_yellow, 0, "B");
    frame_put(&src, 5, 31, eia608_style_white, 0, "Z");

    frame_put(&src, 6, 0, eia608_style_white, 0, "X");
    frame_put(&src, 6, 1, eia608_style_white, 0, EIA608_CHAR_SPACE);
    frame_put(&src, 6, 2, eia608_style_white, 0, "Y");

    // Last row, last two columns.
    frame_put(&src, SCREEN_ROWS - 1, 30, eia608_style_red, 0, "P");
    frame_put(&src, SCREEN_ROWS - 1, 31, eia608_style_red, 1, "Q");

    CHECK(export_import(&src, &dst, &sei, &parsed));
    CHECK(grids_equal(&src, &dst, "sparse-grid"));
    CHECK(wire_roundtrip_equal(&src, &sei, &eoc_swaps));
    CHECK(1 == eoc_swaps);

    // Explicit presence checks for the space/hole distinction.
    CHECK(0 == strcmp(EIA608_CHAR_SPACE, caption_frame_read_char(&dst, 5, 2, &sty, &uln)));
    CHECK(0 == strcmp("A", caption_frame_read_char(&dst, 5, 3, 0, 0)));
    CHECK(0 == caption_frame_read_char(&dst, 5, 4, 0, 0)[0]);
    CHECK(0 == caption_frame_read_char(&dst, 5, 6, 0, 0)[0]);
    CHECK(0 == strcmp("B", caption_frame_read_char(&dst, 5, 7, &sty, &uln)));
    CHECK(eia608_style_yellow == sty);
    CHECK(0 == strcmp("Z", caption_frame_read_char(&dst, 5, 31, 0, 0)));
    CHECK(0 == strcmp("Q", caption_frame_read_char(&dst, 14, 31, &sty, &uln)));
    CHECK(1 == uln && eia608_style_red == sty);

    sei_free(&parsed);
    sei_free(&sei);
    return 1;
}

// 3. Every character set, repeated Special NA codes and interleaved styles.
static int test_charsets(void)
{
    caption_frame_t src, dst;
    sei_t sei, parsed;
    int eoc_swaps = 0;

    frame_reset(&src, 3.0);

    // Basic NA pair, Special NA, two identical consecutive Special NA codes,
    // then one character from each Western European sub-table.
    frame_put(&src, 0, 0, eia608_style_white, 0, "A");
    frame_put(&src, 0, 1, eia608_style_white, 0, "B");
    frame_put(&src, 0, 2, eia608_style_white, 0, EIA608_CHAR_REGISTERED_SIGN);
    frame_put(&src, 0, 3, eia608_style_white, 0, EIA608_CHAR_DEGREE_SIGN);
    frame_put(&src, 0, 4, eia608_style_white, 0, EIA608_CHAR_EIGHTH_NOTE);
    frame_put(&src, 0, 5, eia608_style_white, 0, EIA608_CHAR_EIGHTH_NOTE);
    frame_put(&src, 0, 6, eia608_style_white, 0, EIA608_CHAR_LATIN_CAPITAL_LETTER_A_WITH_ACUTE);
    frame_put(&src, 0, 7, eia608_style_white, 0, EIA608_CHAR_LATIN_CAPITAL_LETTER_E_WITH_ACUTE);
    frame_put(&src, 0, 8, eia608_style_white, 0, EIA608_CHAR_LATIN_SMALL_LETTER_A_WITH_DIAERESIS);
    frame_put(&src, 0, 9, eia608_style_white, 0, EIA608_CHAR_LATIN_SMALL_LETTER_O_WITH_STROKE);
    frame_put(&src, 0, 10, eia608_style_white, 0, EIA608_CHAR_LEFT_CURLY_BRACKET);
    frame_put(&src, 0, 11, eia608_style_white, 0, EIA608_CHAR_RIGHT_POINTING_DOUBLE_ANGLE_QUOTATION_MARK);
    frame_put(&src, 0, 12, eia608_style_white, 0, EIA608_CHAR_EM_DASH);

    // A lone Western European character on a row: no preceding cell may be
    // consumed by its replacement semantics.
    frame_put(&src, 1, 0, eia608_style_green, 0, EIA608_CHAR_LATIN_CAPITAL_LETTER_A_WITH_GRAVE);

    // Pending Basic NA, a mid-row style switch and then a Western European
    // character: neither ordering may overwrite the previous character.
    frame_put(&src, 2, 0, eia608_style_white, 0, "A");
    frame_put(&src, 2, 1, eia608_style_red, 1, EIA608_CHAR_LATIN_CAPITAL_LETTER_A_WITH_ACUTE);
    frame_put(&src, 2, 2, eia608_style_white, 0, "C");

    // An odd, unpaired Basic NA at the end of a row must survive the flush and
    // must not leak into the next row's Western European character.
    frame_put(&src, 3, 0, eia608_style_green, 0, "Z");
    frame_put(&src, 4, 0, eia608_style_white, 0, EIA608_CHAR_LATIN_CAPITAL_LETTER_E_WITH_ACUTE);

    CHECK(export_import(&src, &dst, &sei, &parsed));
    CHECK(grids_equal(&src, &dst, "charsets"));
    CHECK(wire_roundtrip_equal(&src, &sei, &eoc_swaps));
    CHECK(1 == eoc_swaps);
    CHECK(0 == strcmp(EIA608_CHAR_EIGHTH_NOTE, caption_frame_read_char(&dst, 0, 5, 0, 0)));
    CHECK(0 == strcmp("A", caption_frame_read_char(&dst, 2, 0, 0, 0)));
    CHECK(0 == strcmp("Z", caption_frame_read_char(&dst, 3, 0, 0, 0)));
    CHECK(0 == strcmp(EIA608_CHAR_LATIN_CAPITAL_LETTER_E_WITH_ACUTE, caption_frame_read_char(&dst, 4, 0, 0, 0)));

    sei_free(&parsed);
    sei_free(&sei);
    return 1;
}

// 4. A full screen spans several GA94 messages; pending text, styles and the
// pen position stay continuous across message boundaries.
static int test_packet_boundary(void)
{
    caption_frame_t src, dst;
    sei_t sei, parsed;
    sei_message_t* msg;
    int messages = 0, eoc_swaps = 0;
    int r, c;
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

    frame_reset(&src, 4.0);

    for (r = 0; r < SCREEN_ROWS; ++r) {
        for (c = 0; c < SCREEN_COLS; ++c) {
            eia608_style_t sty = (eia608_style_t)((r + c) % 8);

            if (0 == c % 7 && 0 != c) {
                frame_put(&src, r, c, sty, r % 2, EIA608_CHAR_SPACE);
            } else {
                frame_put(&src, r, c, sty, r % 2, (char[]){ alphabet[c % 26], 0 });
            }
        }
    }

    CHECK(export_import(&src, &dst, &sei, &parsed));
    CHECK(grids_equal(&src, &dst, "packet-boundary"));
    CHECK(wire_roundtrip_equal(&src, &sei, &eoc_swaps));
    CHECK(1 == eoc_swaps);

    // Every GA94 message carries at most 31 cc_data items and there must be
    // more than one message for this screen.
    for (msg = sei_message_head(&sei); msg; msg = sei_message_next(msg)) {
        if (sei_type_user_data_registered_itu_t_t35 == sei_message_type(msg)) {
            uint8_t* data = sei_message_data(msg);
            int cc_count = data[8] & 0x1F;
            CHECK(cc_count <= 31);
            ++messages;
        }
    }

    CHECK(1 < messages);
    sei_free(&parsed);
    sei_free(&sei);
    return 1;
}

// 5. Hand-authored CEA-608 words: mid-row codes occupy a display cell, PAC and
// tabs only move the pen, and the encoder's mid-row/backspace pattern decodes
// without leaving an extra space between visible characters.
static int test_wire_midrow(void)
{
    caption_frame_t frame;
    eia608_style_t sty;
    int uln;
    double ts = 5.0;

    frame_reset(&frame, ts);

    CHECK(LIBCAPTION_OK == caption_frame_decode(&frame, eia608_control_command(eia608_control_resume_caption_loading, 0), ts));
    CHECK(LIBCAPTION_OK == caption_frame_decode(&frame, eia608_control_command(eia608_control_erase_non_displayed_memory, 0), ts));
    CHECK(LIBCAPTION_OK == caption_frame_decode(&frame, eia608_row_style_pramble(14, 0, eia608_style_white, 0), ts));
    CHECK(LIBCAPTION_OK == caption_frame_decode(&frame, eia608_from_utf8_2("A", "B"), ts));

    // This mid-row code must display a red underlined space in column 2.
    CHECK(LIBCAPTION_OK == caption_frame_decode(&frame, eia608_midrow_change(0, eia608_style_red, 1), ts));
    CHECK(LIBCAPTION_OK == caption_frame_decode(&frame, eia608_from_utf8_1("C", 0), ts));

    // Column PAC to column 8, then two tab offsets lands on column 10.
    CHECK(LIBCAPTION_OK == caption_frame_decode(&frame, eia608_row_column_pramble(14, 8, 0, 0), ts));
    CHECK(LIBCAPTION_OK == caption_frame_decode(&frame, eia608_tab(2, 0), ts));
    CHECK(LIBCAPTION_OK == caption_frame_decode(&frame, eia608_from_utf8_1("D", 0), ts));

    // Mid-row space immediately followed by backspace and a character mirrors
    // the export pattern: no extra visible space may remain. Backspace reports
    // READY like the other correction commands.
    CHECK(LIBCAPTION_OK == caption_frame_decode(&frame, eia608_midrow_change(0, eia608_style_green, 0), ts));
    CHECK(LIBCAPTION_READY == caption_frame_decode(&frame, eia608_control_command(eia608_control_backspace, 0), ts));
    CHECK(LIBCAPTION_OK == caption_frame_decode(&frame, eia608_from_utf8_1("E", 0), ts));

    CHECK(LIBCAPTION_READY == caption_frame_decode(&frame, eia608_control_command(eia608_control_end_of_caption, 0), ts));

    CHECK(0 == strcmp("A", caption_frame_read_char(&frame, 14, 0, &sty, &uln)));
    CHECK(eia608_style_white == sty && 0 == uln);
    CHECK(0 == strcmp("B", caption_frame_read_char(&frame, 14, 1, &sty, &uln)));
    CHECK(eia608_style_white == sty && 0 == uln);
    CHECK(0 == strcmp(EIA608_CHAR_SPACE, caption_frame_read_char(&frame, 14, 2, &sty, &uln)));
    CHECK(eia608_style_red == sty && 1 == uln);
    CHECK(0 == strcmp("C", caption_frame_read_char(&frame, 14, 3, &sty, &uln)));
    CHECK(eia608_style_red == sty && 1 == uln);
    CHECK(0 == caption_frame_read_char(&frame, 14, 4, 0, 0)[0]);
    CHECK(0 == caption_frame_read_char(&frame, 14, 7, 0, 0)[0]);
    CHECK(0 == caption_frame_read_char(&frame, 14, 8, 0, 0)[0]);
    CHECK(0 == caption_frame_read_char(&frame, 14, 9, 0, 0)[0]);
    CHECK(0 == strcmp("D", caption_frame_read_char(&frame, 14, 10, &sty, &uln)));
    CHECK(eia608_style_white == sty && 0 == uln);
    CHECK(0 == strcmp("E", caption_frame_read_char(&frame, 14, 11, &sty, &uln)));
    CHECK(eia608_style_green == sty && 0 == uln);

    return 1;
}

// 6. Reusing the same objects for two different screens: the input grid and
// write pointer are untouched and the second export inherits nothing.
static int test_reuse(void)
{
    caption_frame_t src, dst;
    sei_t sei, parsed;
    caption_frame_buffer_t snapshot;
    const caption_frame_buffer_t* write_ptr;
    int eoc_swaps = 0;

    frame_reset(&src, 6.0);
    frame_put(&src, 0, 0, eia608_style_red, 0, "H");
    frame_put(&src, 0, 1, eia608_style_red, 0, "I");
    frame_put(&src, 0, 2, eia608_style_red, 1, "!");
    frame_put(&src, 1, 4, eia608_style_blue, 0, EIA608_CHAR_EIGHTH_NOTE);

    memcpy(&snapshot, &src.front, sizeof(snapshot));
    write_ptr = src.write;

    CHECK(export_import(&src, &dst, &sei, &parsed));
    CHECK(grids_equal(&src, &dst, "reuse-first"));
    CHECK(wire_roundtrip_equal(&src, &sei, &eoc_swaps));
    CHECK(1 == eoc_swaps);
    CHECK(0 == memcmp(&snapshot, &src.front, sizeof(snapshot)));
    CHECK(write_ptr == src.write);
    sei_free(&parsed);
    sei_free(&sei);

    // Second, completely different screen through the same stack objects.
    frame_reset(&src, 7.0);
    frame_put(&src, 14, 31, eia608_style_green, 0, "Z");
    frame_put(&src, 13, 0, eia608_style_white, 0, EIA608_CHAR_SPACE);
    frame_put(&src, 13, 1, eia608_style_yellow, 1, "Q");

    eoc_swaps = 0;
    CHECK(export_import(&src, &dst, &sei, &parsed));
    CHECK(grids_equal(&src, &dst, "reuse-second"));
    CHECK(wire_roundtrip_equal(&src, &sei, &eoc_swaps));
    CHECK(1 == eoc_swaps);

    // Nothing from the first screen survived into the second import.
    CHECK(0 == caption_frame_read_char(&dst, 0, 0, 0, 0)[0]);
    CHECK(0 == caption_frame_read_char(&dst, 1, 4, 0, 0)[0]);
    CHECK(0 == strcmp("Z", caption_frame_read_char(&dst, 14, 31, 0, 0)));
    CHECK(0 == strcmp("Q", caption_frame_read_char(&dst, 13, 1, 0, 0)));

    sei_free(&parsed);
    sei_free(&sei);
    return 1;
}

test_case cases[] = {
    { "style-boundary", test_style_boundary },
    { "sparse-grid", test_sparse_grid },
    { "charsets", test_charsets },
    { "packet-boundary", test_packet_boundary },
    { "wire-midrow", test_wire_midrow },
    { "reuse", test_reuse },
};

size_t case_count = sizeof(cases) / sizeof(cases[0]);
